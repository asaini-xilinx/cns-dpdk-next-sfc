/* 
 *   Copyright(c) 2019 Solarflare Inc. TBD 
 */

#include "sfc_vdpa.h"
#include "sfc.h"
#include "efx.h"
#include "rte_ethdev.h"
#include "efx_mcdi.h"
#include "efx_regs_mcdi.h"
#include "efx_regs_mcdi.h"
#include <rte_vhost.h>
#include <rte_common.h>
#include <rte_string_fns.h>

#define TEST_COUNT_1 100
#define TEST_COUNT_2 200

#define MIN_VI_COUNT	2
#define MAX_VI_COUNT	2

#define DRV_LOG(level, fmt, args...) \
	rte_log(RTE_LOG_ ## level, sfc_logtype_driver, \
		"SFC_VDPA %s(): " fmt "\n", __func__, ##args)

void rte_get_vf_to_pf_index(char *vf, char *pf);
struct rte_eth_dev * rte_get_pf_to_eth_dev(const char * pf_name);
uint16_t get_rid_from_pci_addr(struct rte_pci_addr pci_addr);

uint32_t sfc_logtype_driver;

int 
sfc_vdpa_get_vfpf_id(struct sfc_vdpa_ops_data *vdpa_data, uint16_t pf_rid, 
		uint16_t vf_rid, uint32_t *pf_index, uint32_t *vf_index);

int
efx_get_sriov_cfg(efx_nic_t *enp,
		unsigned int *vf_current, unsigned int *vf_offset, unsigned int *vf_stride);

static const char * const sfc_vdpa_valid_arguments[] = {
	SFC_VDPA_MODE,
	SFC_VDPA_MAC_ADDR,
	NULL
};
static pthread_mutex_t sfc_vdpa_adapter_list_lock = PTHREAD_MUTEX_INITIALIZER;

/* Find PF index for a VF */
/* TODO : DPDK does not have any API for VF-PF Mapping, this can be upstreamed */ 
void rte_get_vf_to_pf_index(char *vf, char *pf)
{
	char cmd[100];
	char *token[12];
	int ret;
	unsigned int i=0;
	char pf_str[200];

	snprintf(cmd, 1000, "ls -l  /sys/bus/pci/devices/%s/physfn", vf);
	FILE *fp = popen(cmd, "r");
	if (fp == NULL) {
			printf ("failed to open popen");
			return;
	}

	fgets(pf_str, sizeof(cmd), fp);

	ret = rte_strsplit(pf_str, sizeof(pf_str),
					token, RTE_DIM(token), ' ');
	if (ret <= 0) {
		DRV_LOG(ERR, "Cannot get PF number of VF %s \n", vf);
		return;
	}

	/* PF number is the last token in the ../0000:B:D.F format */
	for(i=0; i < RTE_DIM(token); i++) {
		//printf("\n %s : len : %d", token[i], (int)strlen(token[i]) );
		if(strlen(token[i]) == 16)
			break;
	}

	/* Get only BDF value skip '../0000:' prefix */
	memcpy(pf, token[i] + 8, (strlen(token[i]) - 8));

	printf("\n Parent PF :  %s \n", pf);
	if (!pf)
		printf(" failed to find phy device");

	if (pclose(fp) != 0)
		fprintf(stderr," Error: Failed to close command stream \n");

	return;
}

/* DPDK/SFC API name TBD :: Find ethdev for a PF */
/* TODO : DPDK does not have any API for it, this can be upstremed */
struct rte_eth_dev * rte_get_pf_to_eth_dev(const char * pf_name)
{
	int i = 0;
	uint16_t ports = rte_eth_dev_count_avail();
	struct rte_eth_dev *eth_dev = NULL;
	
	char port_name[RTE_ETH_NAME_MAX_LEN];

	DRV_LOG(DEBUG,"\n In rte_get_pf_to_eth_dev for PF : %s" , pf_name );
	DRV_LOG(DEBUG,"\n Available Port : %d", ports);

	for (i = 0; i < ports; i++)
	{
		DRV_LOG(DEBUG,"\n i=%d Port : %d", i, ports);

		/* Compare PCI address which are in the BDF format */
		if (rte_eth_dev_get_name_by_port(i, port_name) == 0) {
			if (strncmp(port_name, pf_name, 7) == 0) {
				eth_dev = &rte_eth_devices[i];
				if (eth_dev == NULL)
					return NULL;
			}
		}
	
	}

	return eth_dev;
}

struct sfc_vdpa_ops_data *
get_vdpa_data_by_did(int did)
{
	int found = 0;
	struct sfc_vdpa_adapter_list *list;
	pthread_mutex_lock(&sfc_vdpa_adapter_list_lock);

	TAILQ_FOREACH(list, &sfc_vdpa_adapter_list, next) {
		if (did == list->sva->vdpa_data->did) {
			printf("\n did :%d, found", did);
			found = 1;
			break;
		}
	}

	pthread_mutex_unlock(&sfc_vdpa_adapter_list_lock);

	if (!found) {
		return NULL;
	}

	return list->sva->vdpa_data;
}

struct sfc_vdpa_adapter_list *
get_adapter_by_dev(struct rte_pci_device *pdev)
{
	int found = 0;
	struct sfc_vdpa_adapter_list *list;

	pthread_mutex_lock(&sfc_vdpa_adapter_list_lock);

	TAILQ_FOREACH(list, &sfc_vdpa_adapter_list, next) {
		if (pdev == list->sva->pdev) {
			found = 1;
			break;
		}
	}

	pthread_mutex_unlock(&sfc_vdpa_adapter_list_lock);

	if (!found)
		return NULL;

	return list;
}


static int
sfc_vdpa_mem_bar_init(struct sfc_vdpa_adapter *sva, const efx_bar_region_t *mem_ebrp)
{
	struct rte_pci_device *pci_dev = sva->pdev;
	efsys_bar_t *ebp = &sva->mem_bar;
	struct rte_mem_resource *res = &pci_dev->mem_resource[mem_ebrp->ebr_index];

	SFC_BAR_LOCK_INIT(ebp, "memBAR");
	ebp->esb_rid = mem_ebrp->ebr_index;
	ebp->esb_dev = pci_dev;
	ebp->esb_base = res->addr;

	sva->vdpa_data->fcw_offset = mem_ebrp->ebr_offset;

	return 0;
}

static void
sfc_vdpa_mem_bar_fini(struct sfc_vdpa_adapter *sva)
{
	efsys_bar_t *ebp = &sva->mem_bar;

	SFC_BAR_LOCK_DESTROY(ebp);
	memset(ebp, 0, sizeof(*ebp));
}

static int
sfc_vdpa_proxy_driver_attach(efx_nic_t *enp, 
				unsigned int pf_index, unsigned int vf_index, boolean_t attach)
{
	int rc;
	efx_dword_t *proxy_hdr = NULL;
	size_t request_size = 0, req_length = 0;
	size_t response_size = 0;
	size_t response_size_actual;
	sfc_inbuf_t req;
	
	printf("\n In sfc_vdpa_proxy_driver_attach .. ");
	
	EFX_MCDI_DECLARE_BUF(inbuf,
                       sizeof(efx_dword_t) * 2 + MC_CMD_DRV_ATTACH_IN_V2_LEN, 0);

   	proxy_hdr = (efx_dword_t *)inbuf;

	EFX_POPULATE_DWORD_2(proxy_hdr[0],
				MCDI_HEADER_CODE, MC_CMD_V2_EXTN,
					MCDI_HEADER_RESYNC, 1);

	EFX_POPULATE_DWORD_2(proxy_hdr[1],
				MC_CMD_V2_EXTN_IN_EXTENDED_CMD, MC_CMD_DRV_ATTACH,
				MC_CMD_V2_EXTN_IN_ACTUAL_LEN, MC_CMD_DRV_ATTACH_IN_LEN);

	req.emr_in_buf = (uint8_t *)&inbuf[8];

	/* Prepare DRV_ATTACH command */
	if (enp->en_drv_version[0] == '\0') {
		req_length = MC_CMD_DRV_ATTACH_IN_LEN;
	} else {
		req_length = MC_CMD_DRV_ATTACH_IN_V2_LEN;
	}
	
	MCDI_IN_POPULATE_DWORD_2(req, DRV_ATTACH_IN_NEW_STATE,
	    DRV_ATTACH_IN_ATTACH, attach ? 1 : 0,
	    DRV_ATTACH_IN_SUBVARIANT_AWARE, EFSYS_OPT_FW_SUBVARIANT_AWARE);
	
	MCDI_IN_SET_DWORD(req, DRV_ATTACH_IN_UPDATE, 1);
	MCDI_IN_SET_DWORD(req, DRV_ATTACH_IN_FIRMWARE_ID, enp->efv);

	if (req_length >= MC_CMD_DRV_ATTACH_IN_V2_LEN) {
		EFX_STATIC_ASSERT(sizeof (enp->en_drv_version) ==
		    MC_CMD_DRV_ATTACH_IN_V2_DRIVER_VERSION_LEN);
		
		memcpy(MCDI_IN2(req, char, DRV_ATTACH_IN_V2_DRIVER_VERSION),
		    enp->en_drv_version, MC_CMD_DRV_ATTACH_IN_V2_DRIVER_VERSION_LEN);
	}

	/* Populate proxy request buff with driver MCDI command */
	request_size = req_length + PROXY_HDR_SIZE; 
	response_size = MC_CMD_DRV_ATTACH_EXT_OUT_LEN + PROXY_HDR_SIZE;
	
	/* Send proxy command */
	rc = efx_mcdi_proxy_cmd(enp, pf_index, vf_index, 
				inbuf, request_size,
				inbuf, response_size,
				&response_size_actual);
	
	if (rc != 0)
		goto fail_proxy_cmd;
					
	/* Process proxy command response */
	if (response_size_actual < response_size) {
		rc = EMSGSIZE;		
	}
	
	return rc;

fail_proxy_cmd:
	DRV_LOG(ERR, "\n Proxy Cmd failed with error : %d  \n", (int)rc);
	return rc;
}

static int
sfc_vdpa_proxy_vi_alloc(efx_nic_t *enp, 
		unsigned int pf_index, unsigned int vf_index,
		unsigned int min_vi_count, unsigned int max_vi_count)
{
	int rc = 0;
	efx_dword_t *proxy_hdr = NULL;
	size_t request_size = 0;
	size_t response_size = 0;
	size_t response_size_actual;
	uint32_t vi_base=0, vi_count=0, vi_shift=0;
	sfc_inbuf_t req;
	sfc_outbuf_t resp;

	EFX_MCDI_DECLARE_BUF(inbuf,
                       sizeof(efx_dword_t) * 2 + MC_CMD_ALLOC_VIS_IN_LEN, 0);
	EFX_MCDI_DECLARE_BUF(outbuf, 8 + MC_CMD_ALLOC_VIS_EXT_OUT_LEN, 0);

	/* Prepare proxy header */
	proxy_hdr = (efx_dword_t *)inbuf;

	EFX_POPULATE_DWORD_2(proxy_hdr[0],
		MCDI_HEADER_CODE, MC_CMD_V2_EXTN,
			MCDI_HEADER_RESYNC, 1);

	EFX_POPULATE_DWORD_2(proxy_hdr[1],
		MC_CMD_V2_EXTN_IN_EXTENDED_CMD, MC_CMD_ALLOC_VIS,
			MC_CMD_V2_EXTN_IN_ACTUAL_LEN, MC_CMD_ALLOC_VIS_IN_LEN);

	req.emr_in_buf = (uint8_t *)&inbuf[PROXY_HDR_SIZE];

	/* Prepare VI_ALLOC command */	
	MCDI_IN_SET_DWORD(req, ALLOC_VIS_IN_MIN_VI_COUNT, min_vi_count);
	MCDI_IN_SET_DWORD(req, ALLOC_VIS_IN_MAX_VI_COUNT, max_vi_count);
	
	/* Populate proxy request buff with driver MCDI command */
	request_size = MC_CMD_ALLOC_VIS_IN_LEN + PROXY_HDR_SIZE; 
	response_size = MC_CMD_ALLOC_VIS_EXT_OUT_LEN + PROXY_HDR_SIZE;
	
	/* Send proxy command */
	rc = efx_mcdi_proxy_cmd(enp, pf_index, vf_index, 
		inbuf, request_size,
		outbuf, response_size,
		&response_size_actual);

	if (rc != 0)
		goto fail_proxy_cmd;

	/* Process proxy command response */
	if (response_size_actual < response_size) {
		rc = EMSGSIZE;		
	}

  	resp.emr_out_buf = (uint8_t *)&outbuf[MCDI_RESP_HDR_SIZE];

	vi_base = MCDI_OUT_DWORD(resp, ALLOC_VIS_OUT_VI_BASE);
	vi_count = MCDI_OUT_DWORD(resp, ALLOC_VIS_OUT_VI_COUNT);

	/* Report VI_SHIFT if available */
	if (response_size < MC_CMD_ALLOC_VIS_EXT_OUT_LEN)
		vi_shift = 0;
	else
		vi_shift = MCDI_OUT_DWORD(resp, ALLOC_VIS_EXT_OUT_VI_SHIFT);

	printf("\n VI_ALLOC Passed :: vi_base : %d, vi_count : %d, vi_shift : %d \n", vi_base, vi_count, vi_shift);	

	return rc;

fail_proxy_cmd:
	DRV_LOG(ERR, "\n Proxy Cmd failed with error : %d  \n", (int)rc);
	return rc;
}
#if 0
int
sfc_vdpa_proxy_vadapter_alloc(efx_nic_t *enp, 
				unsigned int pf_index, unsigned int vf_index, uint32_t port_id)
{
	int rc;
	efx_dword_t *proxy_hdr = NULL;
	size_t request_size = 0;
	size_t response_size = 0;
	size_t response_size_actual;
	sfc_inbuf_t req;

	/* TBD : MC_CMD_VADAPTOR_ALLOC_IN_LEN is 30 which is not the multiple of 4 */
	EFX_MCDI_DECLARE_BUF(inbuf,
                       sizeof(efx_dword_t) * 2 + MC_CMD_VADAPTOR_ALLOC_IN_LEN +2, 0);
	EFX_MCDI_DECLARE_BUF(outbuf, MC_CMD_VADAPTOR_ALLOC_OUT_LEN, 0);

	/* Prepare proxy header */
	proxy_hdr = (efx_dword_t *)inbuf;

	EFX_POPULATE_DWORD_2(proxy_hdr[0],
				MCDI_HEADER_CODE, MC_CMD_V2_EXTN,
					MCDI_HEADER_RESYNC, 1);

	EFX_POPULATE_DWORD_2(proxy_hdr[1],
				MC_CMD_V2_EXTN_IN_EXTENDED_CMD, MC_CMD_VADAPTOR_ALLOC,
				MC_CMD_V2_EXTN_IN_ACTUAL_LEN, MC_CMD_VADAPTOR_ALLOC_IN_LEN);

	req.emr_in_buf = (uint8_t *)&inbuf[PROXY_HDR_SIZE];
   
	printf("\n  prepare MC_CMD_VADAPTOR_ALLOC command ");
   
	/* Prepare MC_CMD_VADAPTOR_ALLOC command */
	MCDI_IN_SET_DWORD(req, VADAPTOR_ALLOC_IN_UPSTREAM_PORT_ID, port_id);
	MCDI_IN_POPULATE_DWORD_1(req, VADAPTOR_ALLOC_IN_FLAGS,
	    VADAPTOR_ALLOC_IN_FLAG_PERMIT_SET_MAC_WHEN_FILTERS_INSTALLED, 1);
		
	/* Populate proxy request buff with driver MCDI command */
	request_size = MC_CMD_VADAPTOR_ALLOC_IN_LEN + 2 + PROXY_HDR_SIZE;
	response_size = MC_CMD_VADAPTOR_ALLOC_OUT_LEN + PROXY_HDR_SIZE; 
	
	/* Send proxy command */
	rc = efx_mcdi_proxy_cmd(enp, pf_index, vf_index, 
				inbuf, request_size,
				outbuf, response_size,
				&response_size_actual);
	
	if (rc != 0)
		goto fail_proxy_cmd;
					
	/* Process proxy command response */
	if (response_size_actual < response_size) {
		rc = EMSGSIZE;		
	}
	
	return rc;

fail_proxy_cmd:
	DRV_LOG(ERR, "\n Proxy Cmd failed with error : %d  \n", (int)rc);
	return rc;
}

int
sfc_vdpa_proxy_vport_alloc(efx_nic_t *enp, unsigned int pf_index,
							unsigned int vf_index, unsigned int *vport_id)
{
	int rc;
	efx_dword_t *proxy_hdr = NULL;
	size_t request_size = 0;
	size_t response_size = 0;
	size_t response_size_actual;
	sfc_inbuf_t req;
	sfc_outbuf_t resp;
	
	EFX_MCDI_DECLARE_BUF(inbuf,
                       sizeof(efx_dword_t) * 2 + MC_CMD_VPORT_ALLOC_IN_LEN, 0);
	EFX_MCDI_DECLARE_BUF(outbuf, 8 + MC_CMD_VPORT_ALLOC_OUT_LEN, 0);

	/* Prepare proxy header */
	proxy_hdr = (efx_dword_t *)inbuf;

	EFX_POPULATE_DWORD_2(proxy_hdr[0],
				MCDI_HEADER_CODE, MC_CMD_V2_EXTN,
					MCDI_HEADER_RESYNC, 1);

	EFX_POPULATE_DWORD_2(proxy_hdr[1],
				MC_CMD_V2_EXTN_IN_EXTENDED_CMD, MC_CMD_VPORT_ALLOC,
				MC_CMD_V2_EXTN_IN_ACTUAL_LEN, MC_CMD_VPORT_ALLOC_IN_LEN);

	req.emr_in_buf = (uint8_t *)&inbuf[PROXY_HDR_SIZE];
 
	/* Prepare vport alloc command */
	MCDI_IN_SET_DWORD(req, VPORT_ALLOC_IN_UPSTREAM_PORT_ID, EVB_PORT_ID_ASSIGNED);
	MCDI_IN_SET_DWORD(req, VPORT_ALLOC_IN_TYPE, EFX_VPORT_TYPE_NORMAL);
	MCDI_IN_SET_DWORD(req, VPORT_ALLOC_IN_NUM_VLAN_TAGS, EFX_FILTER_VID_UNSPEC);
	MCDI_IN_POPULATE_DWORD_2(req, VPORT_ALLOC_IN_FLAGS,
		VPORT_ALLOC_IN_FLAG_AUTO_PORT, 0,
		VPORT_ALLOC_IN_FLAG_VLAN_RESTRICT, 0); //TBD : Value of VPORT_ALLOC_IN_FLAG_VLAN_RESTRICT field?  
		
	MCDI_IN_POPULATE_DWORD_1(req, VPORT_ALLOC_IN_VLAN_TAGS,
		VPORT_ALLOC_IN_VLAN_TAG_0, 0);
		
	/* Populate proxy request buff with driver MCDI command */
	request_size = MC_CMD_VPORT_ALLOC_IN_LEN + PROXY_HDR_SIZE;
	response_size = MC_CMD_VPORT_ALLOC_OUT_LEN + PROXY_HDR_SIZE; 
	
	/* Send proxy command */
	rc = efx_mcdi_proxy_cmd(enp, pf_index, vf_index, 
				inbuf, request_size,
				outbuf, response_size,
				&response_size_actual);

	if (rc != 0)
		goto fail_proxy_cmd;

	/* Process proxy command response */
	if (response_size_actual < response_size) {
		rc = EMSGSIZE;		
	}
	
    /* Reasponse is after proxy header */
  	resp.emr_out_buf = (uint8_t *)&outbuf[MCDI_RESP_HDR_SIZE];
	
	*vport_id = *MCDI_OUT2(resp, uint32_t, VPORT_ALLOC_OUT_VPORT_ID);
	
	return rc;

fail_proxy_cmd:
	DRV_LOG(ERR, "\n Proxy Cmd failed with error : %d  \n", (int)rc);
	return rc;
}
#endif
int
efx_get_sriov_cfg(efx_nic_t *enp,
			unsigned int *vf_current, 
			unsigned int *vf_offset, 
			unsigned int *vf_stride)
{
	efx_mcdi_req_t req;
	EFX_MCDI_DECLARE_BUF(payload, MC_CMD_GET_SRIOV_CFG_IN_LEN,
		MC_CMD_GET_SRIOV_CFG_OUT_LEN);
	efx_rc_t rc = 0;
	enp =enp;

	req.emr_cmd = MC_CMD_GET_SRIOV_CFG;
	req.emr_in_buf = payload;
	req.emr_in_length = MC_CMD_GET_SRIOV_CFG_IN_LEN;
	req.emr_out_buf = payload;
	req.emr_out_length = MC_CMD_GET_SRIOV_CFG_OUT_LEN;

	efx_mcdi_execute(enp, &req);

	if (req.emr_rc != 0) {
		rc = req.emr_rc;
		goto fail1;
	}

	if (req.emr_out_length_used < MC_CMD_GET_SRIOV_CFG_OUT_LEN) {
		rc = EMSGSIZE;
		goto fail2;
	}

	*vf_current = MCDI_OUT_DWORD(req, GET_SRIOV_CFG_OUT_VF_CURRENT);
	*vf_offset  = MCDI_OUT_DWORD(req, GET_SRIOV_CFG_OUT_VF_OFFSET);
	*vf_stride  = MCDI_OUT_DWORD(req, GET_SRIOV_CFG_OUT_VF_STRIDE);

fail2:
	EFSYS_PROBE(fail2);
fail1:
	EFSYS_PROBE1(fail1, efx_rc_t, rc);

	return (rc);
}

/*Todo: Remove all debug prints which uses printf */
int
sfc_vdpa_get_vfpf_id(struct sfc_vdpa_ops_data *vdpa_data, uint16_t pf_rid, 
						uint16_t vf_rid, uint32_t *pf_index, uint32_t *vf_index)
{	
	uint32_t vf_current=0, vf_offset=0, vf_stride=0;
	uint32_t vf_rid_base, vf_rid_offset;
	int rc;
	uint32_t pf, vf;
	
	printf("\n vf_rid : %d, pf_rid %d ", vf_rid, pf_rid);

	/* Get PF Index */
	rc = efx_mcdi_get_function_info(vdpa_data->nic, &pf, &vf);
	printf("\n rc from efx_mcdi_get_function_info : %d, vf:%d, pf:%d", rc, vf, pf);
	*pf_index = (uint16_t) pf;
	
	printf("\n pf_index : %d, vf_index : %d (default) ", *pf_index, *vf_index );

	/* Use mcdi MC_CMD_GET_SRIOV_CFG to get vf current/offset/stride tuple */
	rc = efx_get_sriov_cfg(vdpa_data->nic, &vf_current, &vf_offset, &vf_stride);
	printf("\n vf_current :%d, vf_offset: %d, vf_stride : %d", vf_current, vf_offset, vf_stride);
	
	vf_rid_base = pf_rid + vf_offset;
	printf("\n vf_rid_base %d ", vf_rid_base);
    
	if (vf_rid >= vf_rid_base) 
	{
		vf_rid_offset = (vf_rid - vf_rid_base);
		printf("\n vf_rid_offset %d ", vf_rid_offset);
	  
		if (vf_rid_offset % vf_stride == 0) {
			vf = vf_rid_offset / vf_stride;
			printf("\n vf %d ", vf);
			if (vf <= vf_current) {
				*vf_index = (uint16_t)vf;
				printf("\n Found vf_index for vf_rid : %d", vf_rid);
				printf("\n pf_index : %d, vf_index : %d ", *pf_index, *vf_index );
			
				return 0;
			}
	  	}
	}

	/* Error */	
	printf("\n Error : could not found vf_index for vf_rid : %d", vf_rid);
	printf("\n pf_index : %d, vf_index : %d ", *pf_index, *vf_index );

	return rc;
}

uint16_t get_rid_from_pci_addr(struct rte_pci_addr pci_addr)
{
	uint16_t rid;  

	rid = (((pci_addr.bus & 0xff) << 8) | ((pci_addr.devid & 0x1f) << 3) | (pci_addr.function & 0x7));
	return rid;
}

int
sfc_vdpa_device_init(struct sfc_vdpa_adapter *sva)
{
	struct rte_pci_device *pci_dev = sva->pdev;
	efx_nic_t *enp = NULL;
	efx_bar_region_t mem_ebr;
	efsys_pci_config_t espc;
	uint32_t pf_index=0, vf_index=0;
	//uint32_t port_id;
	//uint32_t vport_id = 0;
	int rc;
	struct rte_pci_addr vf_pci_addr;
	uint16_t vf_rid = 0, pf_rid = 0; 
	
	vf_pci_addr = pci_dev->addr;

	/* Initialize NIC pointer with PF's NIC */
	enp = sva->vdpa_data->nic;
	if(enp == NULL)	{
		printf("\n enp : NULL");
		return -1;
	}
	
	/* Get VF's RID from vf pci address */
	vf_rid = get_rid_from_pci_addr(vf_pci_addr);
	pf_rid = get_rid_from_pci_addr(sva->vdpa_data->pf_pci_addr);

	printf("\n vf_rid : %d", vf_rid);
	printf("\n pf_rid : %d \n\n", pf_rid);
	
	espc.espc_dev = pci_dev;
	rc = efx_family_probe_bar(pci_dev->id.vendor_id, pci_dev->id.device_id,
				  &espc, &sva->family, &mem_ebr);

	/* Get vf index */
	rc = sfc_vdpa_get_vfpf_id(sva->vdpa_data, pf_rid, vf_rid, &pf_index, &vf_index);
	if(rc != 0)
		goto fail_get_vf_vf_init;

	sva->vdpa_data->pf_index = pf_index;
	sva->vdpa_data->vf_index = vf_index;

	/* Send proxy cmd for DRIVER_ATTACH */
	rc = sfc_vdpa_proxy_driver_attach(enp, pf_index, vf_index, 1);

	/* Send proxy cmd for VIs_ALLOC */
	printf("\n Call proxy_vi_alloc() ... ");
	rc = sfc_vdpa_proxy_vi_alloc(enp, pf_index, vf_index, MIN_VI_COUNT, MAX_VI_COUNT);

#if 0
	/* Send proxy cmd for VADAPTOR_ALLOC */
	port_id = EVB_PORT_ID_ASSIGNED;

	printf("\n Call proxy_vadapter_alloc() ... ");
	/* On a VF, this may fail with MC_CMD_ERR_NO_EVB_PORT (ENOENT) if the PF
	 * driver has yet to bring up the EVB port */
	rc = sfc_vdpa_proxy_vadapter_alloc(enp, pf_index, vf_index, port_id);
	
	/* Send proxy cmd for VPORT_ALLOC */
	printf("\n Call proxy_vport_alloc() ... ");
	rc = sfc_vdpa_proxy_vport_alloc(enp, pf_index, vf_index, &vport_id);
	if (rc == 0)
	{
		/* Store vport_id in the vdpa_data */
		sva->vdpa_data->vport_id = vport_id;
	}
#endif	
	rc = sfc_vdpa_mem_bar_init(sva, &mem_ebr);
	if (rc != 0)
		goto fail_mem_bar_init;

	rc = efx_virtio_init(enp);
	if (rc != 0)
		goto fail_virtio_init;

	sva->vdpa_data->state = SFC_VDPA_STATE_INITIALIZED;

	DRV_LOG(ERR,"\n Exit from probe");
	return 0;

fail_get_vf_vf_init:
fail_virtio_init:
fail_mem_bar_init:

	return rc;
}

void
sfc_vdpa_device_fini(struct sfc_vdpa_adapter *sva)
{
	SFC_ASSERT(sfc_vdpa_adapter_is_locked(sva->vdpa_data));

	sfc_vdpa_mem_bar_fini(sva);

	sva->vdpa_data->state = SFC_VDPA_STATE_UNINITIALIZED;
}

/* TODO: Remove all debug logs from this function */
static int
sfc_vdpa_vfio_setup(struct sfc_vdpa_adapter *sva)
{
	struct rte_pci_device *dev = sva->pdev;
	struct sfc_vdpa_ops_data *vdpa_data = sva->vdpa_data;
	
	char dev_name[RTE_DEV_NAME_MAX_LEN] = {0};
	int iommu_group_num;
	
	if ((vdpa_data == NULL) || (dev == NULL))
		return -1;
		
	vdpa_data->vfio_dev_fd = -1;
	vdpa_data->vfio_group_fd = -1;
	vdpa_data->vfio_container_fd = -1;

	rte_pci_device_name(&dev->addr, dev_name, RTE_DEV_NAME_MAX_LEN);

	vdpa_data->vfio_container_fd = rte_vfio_container_create();
	if (vdpa_data->vfio_container_fd < 0)
		return -1;

	rte_vfio_get_group_num(rte_pci_get_sysfs_path(), dev_name,
			&iommu_group_num);

	vdpa_data->vfio_group_fd = rte_vfio_container_group_bind(
			vdpa_data->vfio_container_fd, iommu_group_num);
	
	if (vdpa_data->vfio_group_fd < 0)
		goto error;
	if (rte_pci_map_device(dev))
		goto error;
	
	vdpa_data->vfio_dev_fd = dev->intr_handle.vfio_dev_fd;
	
	return 0;

error:
	rte_vfio_container_destroy(vdpa_data->vfio_container_fd);
	return -1;
}

static inline int
check_vdpa_mode(const char *key __rte_unused, const char *value, void *extra_args)
{
	uint16_t *n = extra_args;

	if (value == NULL || extra_args == NULL)
		return -EINVAL;

	*n = (uint16_t)strtoul(value, NULL, 0);
	if (*n == USHRT_MAX && errno == ERANGE)
		return -1;

	return 0;
}

static inline int
get_eth_addr(const char *key __rte_unused, const char *value, void *extra_args)
{
	struct rte_ether_addr *mac_addr = extra_args;

	if (value == NULL || extra_args == NULL)
		return -EINVAL;
	
	/* Convert string with Ethernet address to an ether_addr */
	rte_ether_unformat_addr(value, mac_addr);
	
	return 0;
}

static struct rte_pci_id pci_id_sfc_vdpa_efx_map[] = {
#define RTE_PCI_DEV_ID_DECL_XNIC(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
	RTE_PCI_DEV_ID_DECL_XNIC(EFX_PCI_VENID_XILINX, EFX_PCI_DEVID_RIVERHEAD_VF)
    { .vendor_id = 0, /* sentinel */ },
};

static int sfc_vdpa_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	struct rte_pci_device *pci_dev)
{
	struct sfc_vdpa_adapter *sva = NULL;
	struct sfc_vdpa_adapter_list  *sva_list = NULL;
	int vdpa_mode = 0, ret = 0;
	struct rte_ether_addr mac_addr;
	struct rte_kvargs *kvlist = NULL;
	struct sfc_vdpa_ops_data *vdpa_data;
	struct rte_pci_device *pf_pci_dev = NULL;
	struct rte_eth_dev *pf_eth_dev = NULL; 
	struct sfc_adapter *sa = NULL;
	char pf_dev_name[RTE_DEV_NAME_MAX_LEN] = {0};	
	char vf_dev_name[RTE_DEV_NAME_MAX_LEN] = {0};		
	int i=0;

	DRV_LOG(DEBUG,"\n Enter sfc_vdpa_pci_probe : ");

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	kvlist = rte_kvargs_parse(pci_dev->device.devargs->args,
				sfc_vdpa_valid_arguments);
	if (kvlist == NULL)
		return 1;

	/* Do not probe if vdpa mode is not specified */
	if (rte_kvargs_count(kvlist, SFC_VDPA_MODE) == 0) {
		rte_kvargs_free(kvlist);
		return 1;
	}

	ret = rte_kvargs_process(kvlist, SFC_VDPA_MODE, &check_vdpa_mode,
				&vdpa_mode);
	if (ret < 0 || vdpa_mode == 0) {
		rte_kvargs_free(kvlist);
		return 1;
	}
	
	/* TODO: Do not probe if MAC addr is not specified ? */
	if (rte_kvargs_count(kvlist, SFC_VDPA_MAC_ADDR) == 0) {
		rte_kvargs_free(kvlist);
		return 1;
	}
	
	ret = rte_kvargs_process(kvlist, SFC_VDPA_MAC_ADDR, &get_eth_addr,
				&mac_addr);
	if (ret < 0) {
		rte_kvargs_free(kvlist);
		return 1;
	}
	
	sva_list = rte_zmalloc("sfc_vdpa", sizeof(struct sfc_vdpa_adapter_list), 0);
	if (sva_list == NULL)
		goto error;

	sva = rte_zmalloc("sfc_vdpa", sizeof(struct sfc_vdpa_adapter), 0);
	if (sva == NULL)
		goto error;

	sva->pdev = pci_dev;

	/* Create vdpa context */
	vdpa_data = sfc_vdpa_create_context();
	if (vdpa_data == NULL)
		goto error;
		
	vdpa_data->vdpa_context = SFC_VDPA_AS_VF;
	vdpa_data->pci_dev = pci_dev;

	/* Store vdpa context in the adpopter structure */
	sva->vdpa_data = vdpa_data;
	
	
	/* Populate MAC address */
	for (i=0;i<6;i++) {
		vdpa_data->eth_addr[i]  = mac_addr.addr_bytes[i];
	}

	if (sfc_vdpa_vfio_setup(sva) < 0) {
		DRV_LOG(ERR, "failed to setup device %s", pci_dev->name);
		goto error;
	} else
		DRV_LOG(ERR, "Successfully setup devices %s", pci_dev->name);

	/* Find Parent PF's and its rte_eth_dev to access process_private fields */
	rte_pci_device_name(&pci_dev->addr, vf_dev_name, RTE_DEV_NAME_MAX_LEN);
	
	rte_get_vf_to_pf_index(vf_dev_name, pf_dev_name);
	if(pf_dev_name == NULL) {
		DRV_LOG(ERR,"\n Could not find any PF device ");
		return 0;
	}

	DRV_LOG(DEBUG,"\n\n\n\n vf_dev_name : %s, pf_dev_name %s \n\n\n\n ", vf_dev_name, pf_dev_name);

	/* Get PF's rte_eth_dev to access process_private (PF's adapter) fields */
	pf_eth_dev = rte_get_pf_to_eth_dev(pf_dev_name);

	DRV_LOG(DEBUG,"\n pf_eth_dev : %p ", pf_eth_dev);

	if (pf_eth_dev != NULL) {
        	sa = (struct sfc_adapter *)pf_eth_dev->process_private;
		if (sa == NULL)
			goto error;
	}
	else {		
		DRV_LOG(ERR,"\n PF's ethdev could not found");
		return 0;
	}
	
	/* Update vdpa context vdpa_data fields */
	vdpa_data->nic = sa->nic;
	pf_pci_dev = RTE_ETH_DEV_TO_PCI(pf_eth_dev);
	vdpa_data->pf_pci_addr = pf_pci_dev->addr;
	
	rte_spinlock_init(&sva->lock);
	
	vdpa_data->lock = sva->lock;

	if (sfc_vdpa_device_init(sva) != 0) {
		DRV_LOG(ERR, "failed to init device %s", pci_dev->name);
		printf("\n Decice Init Failed .. ");
		goto error;
	}
	
	sva->dev_addr.pci_addr = pci_dev->addr;
	sva->dev_addr.type = PCI_ADDR;
	sva_list->sva = sva;
	
	/* Register vdpa ops */
	sfc_vdpa_register_device(vdpa_data, &sva->dev_addr);
	
	DRV_LOG(DEBUG,"\n sfc_vdpa_register_device Done");

	pthread_mutex_lock(&sfc_vdpa_adapter_list_lock);
	TAILQ_INSERT_TAIL(&sfc_vdpa_adapter_list, sva_list, next);
	pthread_mutex_unlock(&sfc_vdpa_adapter_list_lock);

	rte_kvargs_free(kvlist);
	
	DRV_LOG(ERR,"\n Probe Complete");
	
	return 0;

error:
	rte_kvargs_free(kvlist);
	rte_free(sva_list);
	rte_free(sva);
	return -1;
}

static int sfc_vdpa_pci_remove(struct rte_pci_device *pci_dev)
{
	struct sfc_vdpa_adapter *sva = NULL;
	struct sfc_vdpa_adapter_list  *sva_list = NULL;

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	sva_list = get_adapter_by_dev(pci_dev);
	if (sva_list == NULL) {
		DRV_LOG(ERR, "Invalid device: %s", pci_dev->name);
		return -1;
	}

	sva = sva_list->sva;

	sfc_vdpa_device_fini(sva);

	rte_pci_unmap_device(sva->pdev);
	rte_vfio_container_destroy(sva->vdpa_data->vfio_container_fd);
	sfc_vdpa_unregister_device(sva->vdpa_data);

	pthread_mutex_lock(&sfc_vdpa_adapter_list_lock);
	TAILQ_REMOVE(&sfc_vdpa_adapter_list, sva_list, next);
	pthread_mutex_unlock(&sfc_vdpa_adapter_list_lock);

	rte_free(sva_list);
	rte_free(sva);

	return 0;
}

static struct rte_pci_driver rte_sfc_vdpa = {
	.id_table = pci_id_sfc_vdpa_efx_map,
	.drv_flags = 0,
	.probe = sfc_vdpa_pci_probe,
	.remove = sfc_vdpa_pci_remove,
};

RTE_PMD_REGISTER_PCI(net_sfc_vdpa, rte_sfc_vdpa);
RTE_PMD_REGISTER_PCI_TABLE(net_sfc_vdpa, pci_id_sfc_vdpa_efx_map);
RTE_PMD_REGISTER_KMOD_DEP(net_sfc_vdpa, "vfio-pci");

RTE_INIT(sfc_driver_register_logtype)
{
	int ret;

	ret = rte_log_register_type_and_pick_level(SFC_LOGTYPE_PREFIX "driver",
						   RTE_LOG_NOTICE);
	sfc_logtype_driver = (ret < 0) ? RTE_LOGTYPE_PMD : ret;
}

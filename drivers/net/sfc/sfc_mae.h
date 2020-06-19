/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2019-2020 Xilinx, Inc.
 * Copyright(c) 2019 Solarflare Communications Inc.
 *
 * This software was jointly developed between OKTET Labs (under contract
 * for Solarflare) and Solarflare Communications, Inc.
 */

#ifndef _SFC_MAE_H
#define _SFC_MAE_H

#include <stdbool.h>

#include <rte_spinlock.h>

#include "efx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** FW-allocatable resource context */
struct sfc_mae_fw_rsrc {
	unsigned int			refcnt;
	RTE_STD_C11
	union {
		efx_mae_aset_id_t	aset_id;
	};
};

/** Action set registry entry */
struct sfc_mae_action_set {
	TAILQ_ENTRY(sfc_mae_action_set)	entries;
	unsigned int			refcnt;
	efx_mae_actions_t		*spec;
	struct sfc_mae_fw_rsrc		fw_rsrc;
};

TAILQ_HEAD(sfc_mae_action_sets, sfc_mae_action_set);

/** Options for MAE support status */
enum sfc_mae_status {
	SFC_MAE_STATUS_UNKNOWN = 0,
	SFC_MAE_STATUS_UNSUPPORTED,
	SFC_MAE_STATUS_SUPPORTED
};

/** Rule class registration cache */
struct sfc_mae_rc_cache {
	/**
	 * The last EFX match specification for which class registration
	 * has been conducted successfully
	 */
	efx_mae_match_spec_t		*match_spec;
	/** Handle of the last class registered with the FW */
	efx_mae_rc_handle_t		class_handle;
};

struct sfc_mae {
	/** Assigned switch domain identifier */
	uint16_t			switch_domain_id;
	/** Assigned switch port identifier */
	uint16_t			switch_port_id;
	/** NIC support for MAE status */
	enum sfc_mae_status		status;
	/** Priority level limit for MAE action rules */
	unsigned int			nb_action_rule_prios_max;
	/** Action rule class registration cache */
	struct sfc_mae_rc_cache		action_rc_cache;
	/** Action set registry */
	struct sfc_mae_action_sets	action_sets;
};

struct sfc_adapter;
struct sfc_flow_spec;

/** This implementation supports double-tagging */
#define SFC_MAE_MATCH_VLAN_MAX_NTAGS	(2)

/** It is possible to keep track of one item ETH and two items VLAN */
#define SFC_MAE_L2_MAX_NITEMS		(SFC_MAE_MATCH_VLAN_MAX_NTAGS + 1)

/** Auxiliary entry format to keep track of L2 "type" ("inner_type") */
struct sfc_mae_ethertype {
	rte_be16_t	value;
	rte_be16_t	mask;
};

struct sfc_mae_pattern_data {
	/**
	 * Keeps track of "type" ("inner_type") mask and value for each
	 * parsed L2 item in a pattern. These values/masks get filled
	 * in MAE match specification at the end of parsing. Also, this
	 * information is used to conduct consistency checks:
	 *
	 * - If an item ETH is followed by a single item VLAN,
	 *   the former must have "type" set to one of supported
	 *   TPID values (0x8100, 0x88a8, 0x9100, 0x9200, 0x9300).
	 *
	 * - If an item ETH is followed by two items VLAN, the
	 *   item ETH must have "type" set to one of supported TPID
	 *   values (0x88a8, 0x9100, 0x9200, 0x9300), and the outermost
	 *   VLAN item must have "inner_type" set to TPID value 0x8100.
	 *
	 * In turn, mapping between RTE convention (above requirements) and
	 * MAE fields is non-trivial. The following scheme indicates
	 * which item EtherTypes go to which MAE fields in the case
	 * of single tag:
	 *
	 * ETH	(0x8100)	--> VLAN0_PROTO_BE
	 * VLAN	(L3 EtherType)	--> ETHER_TYPE_BE
	 *
	 * Similarly, in the case of double tagging:
	 *
	 * ETH	(0x88a8)	--> VLAN0_PROTO_BE
	 * VLAN	(0x8100)	--> VLAN1_PROTO_BE
	 * VLAN	(L3 EtherType)	--> ETHER_TYPE_BE
	 */
	struct sfc_mae_ethertype	ethertypes[SFC_MAE_L2_MAX_NITEMS];
	unsigned int			nb_vlan_tags;
};

struct sfc_mae_parse_ctx {
	struct sfc_adapter		*sa;
	efx_mae_match_spec_t		*match_spec_action;
	bool				match_mport_set;
	struct sfc_mae_pattern_data	pattern_data;
};

int sfc_mae_attach(struct sfc_adapter *sa);
void sfc_mae_detach(struct sfc_adapter *sa);
sfc_flow_cleanup_cb_t sfc_mae_flow_cleanup;
int sfc_mae_rule_parse_pattern(struct sfc_adapter *sa,
			       const struct rte_flow_item pattern[],
			       struct sfc_flow_spec_mae *spec,
			       struct rte_flow_error *error);
void sfc_mae_validation_cache_drop(struct sfc_adapter *sa,
				   struct sfc_mae_rc_cache *rc_cache);
int sfc_mae_rule_parse_actions(struct sfc_adapter *sa,
			       const struct rte_flow_action actions[],
			       struct sfc_mae_action_set **action_setp,
			       struct rte_flow_error *error);
sfc_flow_verify_cb_t sfc_mae_flow_verify;
sfc_flow_insert_cb_t sfc_mae_flow_insert;
sfc_flow_remove_cb_t sfc_mae_flow_remove;

#ifdef __cplusplus
}
#endif
#endif /* _SFC_MAE_H */

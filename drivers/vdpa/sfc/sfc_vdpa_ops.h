/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020-2021 Xilinx, Inc.
 */

#ifndef _SFC_VDPA_OPS_H
#define _SFC_VDPA_OPS_H

#include <rte_vdpa.h>
#include <vdpa_driver.h>

#define SFC_VDPA_MAX_QUEUE_PAIRS		8
#define SFC_VDPA_USED_RING_LEN(size) \
	((size) * sizeof(struct vring_used_elem) + sizeof(uint16_t) * 3)

enum sfc_vdpa_context {
	SFC_VDPA_AS_VF
};

enum sfc_vdpa_state {
	SFC_VDPA_STATE_UNINITIALIZED = 0,
	SFC_VDPA_STATE_INITIALIZED,
	SFC_VDPA_STATE_CONFIGURING,
	SFC_VDPA_STATE_CONFIGURED,
	SFC_VDPA_STATE_CLOSING,
	SFC_VDPA_STATE_CLOSED,
	SFC_VDPA_STATE_STARTING,
	SFC_VDPA_STATE_STARTED,
	SFC_VDPA_STATE_STOPPING,
};

struct sfc_vdpa_vring_info {
	uint64_t	desc;
	uint64_t	avail;
	uint64_t	used;
	uint64_t	size;
	uint16_t	last_avail_idx;
	uint16_t	last_used_idx;
};

typedef struct sfc_vdpa_vq_context_s {
	volatile void			*doorbell;
	uint8_t				enable;
	efx_virtio_vq_t			*vq;

	uint64_t			sw_vq_iova;
	uint64_t			sw_vq_size;
} sfc_vdpa_vq_context_t;

struct sfc_vdpa_iova_list {
	TAILQ_ENTRY(sfc_vdpa_iova_list)	next;
	uint64_t			iova;
	uint64_t			size;
};

TAILQ_HEAD(sfc_vdpa_iova_list_head,
	   sfc_vdpa_iova_list);

struct sfc_vdpa_ops_data {
	void				*dev_handle;
	int				vid;
	struct rte_vdpa_device		*vdpa_dev;
	enum sfc_vdpa_context		vdpa_context;
	enum sfc_vdpa_state		state;
	pthread_t			notify_tid;
	bool				is_notify_thread_started;

	uint64_t			dev_features;
	uint64_t			drv_features;
	uint64_t			req_features;

	uint16_t			vq_count;
	struct sfc_vdpa_vq_context_s	vq_cxt[SFC_VDPA_MAX_QUEUE_PAIRS * 2];

	/*
	 * In case multiple VF(s) have overlapping IOVA(s), it is possible
	 * that sfc_vdpa_add_iova_in_list() is invoked concurrently.
	 * Use a mutex to protect against it.
	 */
	pthread_mutex_t			iova_list_lock;
	struct sfc_vdpa_iova_list_head	iova_list_head;

	int				epfd;
	uint64_t			sw_vq_iova;
	bool				sw_fallback_mode;
	pthread_t			sw_relay_thread_id;
	struct vring			sw_vq[SFC_VDPA_MAX_QUEUE_PAIRS * 2];
	int				intr_fd[SFC_VDPA_MAX_QUEUE_PAIRS * 2];
};

struct sfc_vdpa_ops_data *
sfc_vdpa_device_init(void *adapter, enum sfc_vdpa_context context);
void
sfc_vdpa_device_fini(struct sfc_vdpa_ops_data *ops_data);

#endif /* _SFC_VDPA_OPS_H */

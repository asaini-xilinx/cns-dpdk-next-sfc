/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#ifndef _CN10K_CRYPTODEV_OPS_H_
#define _CN10K_CRYPTODEV_OPS_H_

#include <rte_cryptodev.h>
#include <cryptodev_pmd.h>

extern struct rte_cryptodev_ops cn10k_cpt_ops;

void cn10k_cpt_set_enqdeq_fns(struct rte_cryptodev *dev);

__rte_internal
uint16_t cn10k_cpt_crypto_adapter_enqueue(uintptr_t tag_op,
					  struct rte_crypto_op *op);
__rte_internal
uintptr_t cn10k_cpt_crypto_adapter_dequeue(uintptr_t get_work1);

#endif /* _CN10K_CRYPTODEV_OPS_H_ */

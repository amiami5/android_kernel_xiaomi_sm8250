/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 */

#ifndef __ADRENO_IOMMU_H
#define __ADRENO_IOMMU_H

#ifdef CONFIG_QCOM_KGSL_IOMMU
int adreno_iommu_set_pt_ctx(struct adreno_ringbuffer *rb,
			struct kgsl_pagetable *new_pt,
			struct adreno_context *drawctxt,
			unsigned long flags);

int adreno_iommu_init(struct adreno_device *adreno_dev);

unsigned int adreno_iommu_set_pt_generate_cmds(
				struct adreno_ringbuffer *rb,
				unsigned int *cmds,
				struct kgsl_pagetable *pt);
#else
static inline int adreno_iommu_init(struct adreno_device *adreno_dev)
{
	return 0;
}

static inline int adreno_iommu_set_pt_ctx(struct adreno_ringbuffer *rb,
			struct kgsl_pagetable *new_pt,
			struct adreno_context *drawctxt,
			unsigned long flags)
{
	return 0;
}

static inline unsigned int adreno_iommu_set_pt_generate_cmds(
				struct adreno_ringbuffer *rb,
				unsigned int *cmds,
				struct kgsl_pagetable *pt)
{
	return 0;
}

#endif
#endif

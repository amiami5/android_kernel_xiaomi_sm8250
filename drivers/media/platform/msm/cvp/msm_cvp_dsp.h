/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#ifndef MSM_CVP_DSP_H
#define MSM_CVP_DSP_H

#include <linux/types.h>
#include "msm_cvp_debug.h"

#define CVP_APPS_DSP_GLINK_GUID "cvp-glink-apps-dsp"
#define CVP_APPS_DSP_SMD_GUID "cvp-smd-apps-dsp"

/*
 * API for CVP driver to send physical address to dsp driver
 * @param phys_addr
 * Physical address of command message queue
 * that needs to be mapped to CDSP.
 * It should be allocated from CMA adsp_mem region.
 *
 * @param size_in_bytes
 * Size in bytes of command message queue
 */
int cvp_dsp_send_cmd_hfi_queue(phys_addr_t *phys_addr,
	uint32_t size_in_bytes);

/*
 * API for CVP driver to suspend CVP session during
 * power collapse
 *
 * @param session_flag
 * Flag to share details of session.
 */
int cvp_dsp_suspend(uint32_t session_flag);

/*
 * API for CVP driver to resume CVP session during
 * power collapse
 *
 * @param session_flag
 * Flag to share details of session.
 */
int cvp_dsp_resume(uint32_t session_flag);

/*
 * API for CVP driver to shutdown CVP session during
 * cvp subsystem error.
 *
 * @param session_flag
 * Flag to share details of session.
 */
int cvp_dsp_shutdown(uint32_t session_flag);

/*
 * API to register iova buffer address with CDSP
 *
 * @iova_buff_addr: IOVA buffer address
 * @buff_index:     buffer index
 * @buff_size:      size in bytes of cvp buffer
 * @session_id:     cvp session id
 */
int cvp_dsp_register_buffer(uint32_t iova_buff_addr,
	uint32_t buff_index, uint32_t buff_size,
	uint32_t session_id);

/*
 * API to de-register iova buffer address from CDSP
 *
 * @iova_buff_addr: IOVA buffer address
 * @buff_index:     buffer index
 * @buff_size:      size in bytes of cvp buffer
 * @session_id:     cvp session id
 */
int cvp_dsp_deregister_buffer(uint32_t iova_buff_addr,
	uint32_t buff_index, uint32_t buff_size,
	uint32_t session_id);

#endif // MSM_CVP_DSP_H


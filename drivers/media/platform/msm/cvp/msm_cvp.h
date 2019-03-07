/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#ifndef _MSM_CVP_H_
#define _MSM_CVP_H_

#include "msm_cvp_internal.h"
#include "msm_cvp_common.h"
#include "msm_cvp_clocks.h"
#include "msm_cvp_debug.h"
#include "msm_cvp_dsp.h"
int msm_cvp_handle_syscall(struct msm_cvp_inst *inst, struct msm_cvp_arg *arg);
int msm_cvp_session_init(struct msm_cvp_inst *inst);
int msm_cvp_session_deinit(struct msm_cvp_inst *inst);
int msm_cvp_session_pause(struct msm_cvp_inst *inst);
int msm_cvp_session_resume(struct msm_cvp_inst *inst);
int msm_cvp_control_init(struct msm_cvp_inst *inst,
		const struct v4l2_ctrl_ops *ctrl_ops);
#endif

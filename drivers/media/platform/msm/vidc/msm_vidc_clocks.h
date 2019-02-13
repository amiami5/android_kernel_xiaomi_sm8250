/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#ifndef _MSM_VIDC_CLOCKS_H_
#define _MSM_VIDC_CLOCKS_H_
#include "msm_vidc_internal.h"

void msm_clock_data_reset(struct msm_vidc_inst *inst);
int msm_vidc_validate_operating_rate(struct msm_vidc_inst *inst,
	u32 operating_rate);
int msm_vidc_set_clocks(struct msm_vidc_core *core);
int msm_comm_vote_bus(struct msm_vidc_core *core);
int msm_dcvs_try_enable(struct msm_vidc_inst *inst);
int msm_vidc_get_mbs_per_frame(struct msm_vidc_inst *inst);
int msm_comm_scale_clocks_and_bus(struct msm_vidc_inst *inst);
int msm_comm_init_clocks_and_bus_data(struct msm_vidc_inst *inst);
void msm_comm_free_freq_table(struct msm_vidc_inst *inst);
int msm_vidc_decide_work_route_iris1(struct msm_vidc_inst *inst);
int msm_vidc_decide_work_mode_iris1(struct msm_vidc_inst *inst);
int msm_vidc_decide_work_route_iris2(struct msm_vidc_inst *inst);
int msm_vidc_decide_work_mode_iris2(struct msm_vidc_inst *inst);
int msm_vidc_decide_core_and_power_mode(struct msm_vidc_inst *inst);
void msm_print_core_status(struct msm_vidc_core *core, u32 core_id);
void msm_vidc_clear_freq_entry(struct msm_vidc_inst *inst,
	u32 device_addr);
void msm_comm_free_input_cr_table(struct msm_vidc_inst *inst);
void msm_comm_update_input_cr(struct msm_vidc_inst *inst, u32 index,
	u32 cr);
void update_recon_stats(struct msm_vidc_inst *inst,
	struct recon_stats_type *recon_stats);
void msm_vidc_init_core_clk_ops(struct msm_vidc_core *core);
#endif

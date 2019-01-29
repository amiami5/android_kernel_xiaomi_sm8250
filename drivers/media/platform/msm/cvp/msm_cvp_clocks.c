// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#include "msm_cvp_common.h"
#include "cvp_hfi_api.h"
#include "msm_cvp_debug.h"
#include "msm_cvp_clocks.h"

#define MSM_CVP_MIN_UBWC_COMPLEXITY_FACTOR (1 << 16)
#define MSM_CVP_MAX_UBWC_COMPLEXITY_FACTOR (4 << 16)

#define MSM_CVP_MIN_UBWC_COMPRESSION_RATIO (1 << 16)
#define MSM_CVP_MAX_UBWC_COMPRESSION_RATIO (5 << 16)

static unsigned long msm_cvp_calc_freq_ar50(struct msm_cvp_inst *inst,
	u32 filled_len);
static int msm_cvp_decide_work_mode_ar50(struct msm_cvp_inst *inst);
static unsigned long msm_cvp_calc_freq(struct msm_cvp_inst *inst,
	u32 filled_len);

struct msm_cvp_core_ops cvp_core_ops_vpu4 = {
	.calc_freq = msm_cvp_calc_freq_ar50,
	.decide_work_route = NULL,
	.decide_work_mode = msm_cvp_decide_work_mode_ar50,
};

struct msm_cvp_core_ops cvp_core_ops_vpu5 = {
	.calc_freq = msm_cvp_calc_freq,
	.decide_work_route = msm_cvp_decide_work_route,
	.decide_work_mode = msm_cvp_decide_work_mode,
};

static inline void msm_dcvs_print_dcvs_stats(struct clock_data *dcvs)
{
	dprintk(CVP_PROF,
		"DCVS: Load_Low %d, Load Norm %d, Load High %d\n",
		dcvs->load_low,
		dcvs->load_norm,
		dcvs->load_high);

	dprintk(CVP_PROF,
		"DCVS: min_threshold %d, max_threshold %d\n",
		dcvs->min_threshold, dcvs->max_threshold);
}

static inline unsigned long get_ubwc_compression_ratio(
	struct ubwc_cr_stats_info_type ubwc_stats_info)
{
	unsigned long sum = 0, weighted_sum = 0;
	unsigned long compression_ratio = 1 << 16;

	weighted_sum =
		32  * ubwc_stats_info.cr_stats_info0 +
		64  * ubwc_stats_info.cr_stats_info1 +
		96  * ubwc_stats_info.cr_stats_info2 +
		128 * ubwc_stats_info.cr_stats_info3 +
		160 * ubwc_stats_info.cr_stats_info4 +
		192 * ubwc_stats_info.cr_stats_info5 +
		256 * ubwc_stats_info.cr_stats_info6;

	sum =
		ubwc_stats_info.cr_stats_info0 +
		ubwc_stats_info.cr_stats_info1 +
		ubwc_stats_info.cr_stats_info2 +
		ubwc_stats_info.cr_stats_info3 +
		ubwc_stats_info.cr_stats_info4 +
		ubwc_stats_info.cr_stats_info5 +
		ubwc_stats_info.cr_stats_info6;

	compression_ratio = (weighted_sum && sum) ?
		((256 * sum) << 16) / weighted_sum : compression_ratio;

	return compression_ratio;
}

int msm_cvp_get_mbs_per_frame(struct msm_cvp_inst *inst)
{
	int height, width;

	if (!inst->in_reconfig) {
		height = max(inst->prop.height[CAPTURE_PORT],
			inst->prop.height[OUTPUT_PORT]);
		width = max(inst->prop.width[CAPTURE_PORT],
			inst->prop.width[OUTPUT_PORT]);
	} else {
		height = inst->reconfig_height;
		width = inst->reconfig_width;
	}

	return NUM_MBS_PER_FRAME(height, width);
}

static int msm_cvp_get_fps(struct msm_cvp_inst *inst)
{
	int fps;

	if ((inst->clk_data.operating_rate >> 16) > inst->prop.fps)
		fps = (inst->clk_data.operating_rate >> 16) ?
			(inst->clk_data.operating_rate >> 16) : 1;
	else
		fps = inst->prop.fps;

	return fps;
}

void cvp_update_recon_stats(struct msm_cvp_inst *inst,
	struct recon_stats_type *recon_stats)
{
	struct recon_buf *binfo;
	u32 CR = 0, CF = 0;
	u32 frame_size;

	CR = get_ubwc_compression_ratio(recon_stats->ubwc_stats_info);

	frame_size = (msm_cvp_get_mbs_per_frame(inst) / (32 * 8) * 3) / 2;

	if (frame_size)
		CF = recon_stats->complexity_number / frame_size;
	else
		CF = MSM_CVP_MAX_UBWC_COMPLEXITY_FACTOR;

	mutex_lock(&inst->reconbufs.lock);
	list_for_each_entry(binfo, &inst->reconbufs.list, list) {
		if (binfo->buffer_index ==
				recon_stats->buffer_index) {
			binfo->CR = CR;
			binfo->CF = CF;
		}
	}
	mutex_unlock(&inst->reconbufs.lock);
}

static int fill_dynamic_stats(struct msm_cvp_inst *inst,
	struct cvp_bus_vote_data *vote_data)
{
	struct recon_buf *binfo, *nextb;
	struct cvp_input_cr_data *temp, *next;
	u32 min_cf = MSM_CVP_MAX_UBWC_COMPLEXITY_FACTOR, max_cf = 0;
	u32 min_input_cr = MSM_CVP_MAX_UBWC_COMPRESSION_RATIO,
		max_input_cr = 0;
	u32 min_cr = MSM_CVP_MAX_UBWC_COMPRESSION_RATIO, max_cr = 0;

	mutex_lock(&inst->reconbufs.lock);
	list_for_each_entry_safe(binfo, nextb, &inst->reconbufs.list, list) {
		if (binfo->CR) {
			min_cr = min(min_cr, binfo->CR);
			max_cr = max(max_cr, binfo->CR);
		}
		if (binfo->CF) {
			min_cf = min(min_cf, binfo->CF);
			max_cf = max(max_cf, binfo->CF);
		}
	}
	mutex_unlock(&inst->reconbufs.lock);

	mutex_lock(&inst->input_crs.lock);
	list_for_each_entry_safe(temp, next, &inst->input_crs.list, list) {
		min_input_cr = min(min_input_cr, temp->input_cr);
		max_input_cr = max(max_input_cr, temp->input_cr);
	}
	mutex_unlock(&inst->input_crs.lock);

	/* Sanitize CF values from HW . */
	max_cf = min_t(u32, max_cf, MSM_CVP_MAX_UBWC_COMPLEXITY_FACTOR);
	min_cf = max_t(u32, min_cf, MSM_CVP_MIN_UBWC_COMPLEXITY_FACTOR);
	max_cr = min_t(u32, max_cr, MSM_CVP_MAX_UBWC_COMPRESSION_RATIO);
	min_cr = max_t(u32, min_cr, MSM_CVP_MIN_UBWC_COMPRESSION_RATIO);
	max_input_cr = min_t(u32,
		max_input_cr, MSM_CVP_MAX_UBWC_COMPRESSION_RATIO);
	min_input_cr = max_t(u32,
		min_input_cr, MSM_CVP_MIN_UBWC_COMPRESSION_RATIO);

	vote_data->compression_ratio = min_cr;
	vote_data->complexity_factor = max_cf;
	vote_data->input_cr = min_input_cr;
	vote_data->use_dpb_read = false;

	/* Check if driver can vote for lower bus BW */
	if (inst->clk_data.load < inst->clk_data.load_norm) {
		vote_data->compression_ratio = max_cr;
		vote_data->complexity_factor = min_cf;
		vote_data->input_cr = max_input_cr;
		vote_data->use_dpb_read = true;
	}

	dprintk(CVP_PROF,
		"Input CR = %d Recon CR = %d Complexity Factor = %d\n",
			vote_data->input_cr, vote_data->compression_ratio,
			vote_data->complexity_factor);

	return 0;
}

int msm_cvp_comm_vote_bus(struct msm_cvp_core *core)
{
	int rc = 0, vote_data_count = 0, i = 0;
	struct hfi_device *hdev;
	struct msm_cvp_inst *inst = NULL;
	struct cvp_bus_vote_data *vote_data = NULL;
	bool is_turbo = false;

	if (!core || !core->device) {
		dprintk(CVP_ERR, "%s Invalid args: %pK\n", __func__, core);
		return -EINVAL;
	}

	if (!core->resources.bus_devfreq_on)
		return 0;

	hdev = core->device;
	vote_data = kzalloc(sizeof(struct cvp_bus_vote_data) *
			MAX_SUPPORTED_INSTANCES, GFP_ATOMIC);
	if (!vote_data) {
		dprintk(CVP_DBG,
			"vote_data allocation with GFP_ATOMIC failed\n");
		vote_data = kzalloc(sizeof(struct cvp_bus_vote_data) *
			MAX_SUPPORTED_INSTANCES, GFP_KERNEL);
		if (!vote_data) {
			dprintk(CVP_DBG,
				"vote_data allocation failed\n");
			return -EINVAL;
		}
	}

	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		int codec = 0;
		struct msm_video_buffer *temp, *next;
		u32 filled_len = 0;
		u32 device_addr = 0;

		mutex_lock(&inst->registeredbufs.lock);
		list_for_each_entry_safe(temp, next,
				&inst->registeredbufs.list, list) {
			if (temp->vvb.vb2_buf.type ==
				V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
				filled_len = max(filled_len,
					temp->vvb.vb2_buf.planes[0].bytesused);
				device_addr = temp->smem[0].device_addr;
			}
			if (inst->session_type == MSM_CVP_ENCODER &&
				(temp->vvb.flags &
				V4L2_BUF_FLAG_PERF_MODE)) {
				is_turbo = true;
			}
		}
		mutex_unlock(&inst->registeredbufs.lock);

		if ((!filled_len || !device_addr) &&
			(inst->session_type != MSM_CVP_CORE)) {
			dprintk(CVP_DBG, "%s: no input for session %x\n",
				__func__, hash32_ptr(inst->session));
			continue;
		}

		++vote_data_count;

		switch (inst->session_type) {
		case MSM_CVP_CORE:
			codec = V4L2_PIX_FMT_CVP;
			break;
		default:
			dprintk(CVP_ERR, "%s: invalid session_type %#x\n",
				__func__, inst->session_type);
			break;
		}

		memset(&(vote_data[i]), 0x0, sizeof(struct cvp_bus_vote_data));

		vote_data[i].domain = get_cvp_hal_domain(inst->session_type);
		vote_data[i].codec = get_cvp_hal_codec(codec);
		vote_data[i].input_width =  max(inst->prop.width[OUTPUT_PORT],
				inst->prop.width[OUTPUT_PORT]);
		vote_data[i].input_height = max(inst->prop.height[OUTPUT_PORT],
				inst->prop.height[OUTPUT_PORT]);
		vote_data[i].output_width =  max(inst->prop.width[CAPTURE_PORT],
				inst->prop.width[OUTPUT_PORT]);
		vote_data[i].output_height =
				max(inst->prop.height[CAPTURE_PORT],
				inst->prop.height[OUTPUT_PORT]);
		vote_data[i].lcu_size = (codec == V4L2_PIX_FMT_HEVC ||
				codec == V4L2_PIX_FMT_VP9) ? 32 : 16;
		vote_data[i].b_frames_enabled = false;
			//msm_cvp_comm_g_ctrl_for_id(inst,
				//V4L2_CID_MPEG_VIDC_VIDEO_NUM_B_FRAMES) != 0;

		vote_data[i].fps = msm_cvp_get_fps(inst);
		if (inst->session_type == MSM_CVP_ENCODER) {
			vote_data[i].bitrate = inst->clk_data.bitrate;
			/* scale bitrate if operating rate is larger than fps */
			if (vote_data[i].fps > inst->prop.fps
				&& inst->prop.fps) {
				vote_data[i].bitrate = vote_data[i].bitrate /
				inst->prop.fps * vote_data[i].fps;
			}
		}

		vote_data[i].power_mode = 0;
		if (inst->clk_data.buffer_counter < DCVS_FTB_WINDOW &&
			inst->session_type != MSM_CVP_CORE)
			vote_data[i].power_mode = CVP_POWER_TURBO;
		if (msm_cvp_clock_voting || is_turbo)
			vote_data[i].power_mode = CVP_POWER_TURBO;

		if (msm_cvp_comm_get_stream_output_mode(inst) ==
				HAL_VIDEO_DECODER_PRIMARY) {
			vote_data[i].color_formats[0] =
				msm_cvp_comm_get_hal_uncompressed(
				inst->clk_data.opb_fourcc);
			vote_data[i].num_formats = 1;
		} else {
			vote_data[i].color_formats[0] =
				msm_cvp_comm_get_hal_uncompressed(
				inst->clk_data.dpb_fourcc);
			vote_data[i].color_formats[1] =
				msm_cvp_comm_get_hal_uncompressed(
				inst->clk_data.opb_fourcc);
			vote_data[i].num_formats = 2;
		}
		vote_data[i].work_mode = inst->clk_data.work_mode;
		fill_dynamic_stats(inst, &vote_data[i]);

		if (core->resources.sys_cache_res_set)
			vote_data[i].use_sys_cache = true;

		if (inst->session_type == MSM_CVP_CORE) {
			vote_data[i].domain =
				get_cvp_hal_domain(inst->session_type);
			vote_data[i].ddr_bw = inst->clk_data.ddr_bw;
			vote_data[i].sys_cache_bw =
				inst->clk_data.sys_cache_bw;
		}

		i++;
	}
	mutex_unlock(&core->lock);
	if (vote_data_count)
		rc = call_hfi_op(hdev, vote_bus, hdev->hfi_device_data,
			vote_data, vote_data_count);

	kfree(vote_data);
	return rc;
}

static int msm_dcvs_scale_clocks(struct msm_cvp_inst *inst,
		unsigned long freq)
{
	int rc = 0;
	int bufs_with_fw = 0;
	int bufs_with_client = 0;
	struct hal_buffer_requirements *buf_reqs;
	struct clock_data *dcvs;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(CVP_ERR, "%s Invalid params\n", __func__);
		return -EINVAL;
	}

	/* assume no increment or decrement is required initially */
	inst->clk_data.dcvs_flags = 0;

	if (!inst->clk_data.dcvs_mode || inst->batch.enable) {
		dprintk(CVP_DBG, "Skip DCVS (dcvs %d, batching %d)\n",
			inst->clk_data.dcvs_mode, inst->batch.enable);
		/* update load (freq) with normal value */
		inst->clk_data.load = inst->clk_data.load_norm;
		return 0;
	}

	dcvs = &inst->clk_data;

	if (is_decode_session(inst))
		bufs_with_fw = msm_cvp_comm_num_queued_bufs(inst,
			V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	else
		bufs_with_fw = msm_cvp_comm_num_queued_bufs(inst,
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	/* +1 as one buffer is going to be queued after the function */
	bufs_with_fw += 1;

	buf_reqs = get_cvp_buff_req_buffer(inst, dcvs->buffer_type);
	if (!buf_reqs) {
		dprintk(CVP_ERR, "%s: invalid buf type %d\n",
			__func__, dcvs->buffer_type);
		return -EINVAL;
	}
	bufs_with_client = buf_reqs->buffer_count_actual - bufs_with_fw;

	/*
	 * PMS decides clock level based on below algo

	 * Limits :
	 * max_threshold : Client extra allocated buffers. Client
	 * reserves these buffers for it's smooth flow.
	 * min_output_buf : HW requested buffers for it's smooth
	 * flow of buffers.
	 * min_threshold : Driver requested extra buffers for PMS.

	 * 1) When buffers outside FW are reaching client's extra buffers,
	 *    FW is slow and will impact pipeline, Increase clock.
	 * 2) When pending buffers with FW are same as FW requested,
	 *    pipeline has cushion to absorb FW slowness, Decrease clocks.
	 * 3) When none of 1) or 2) FW is just fast enough to maintain
	 *    pipeline, request Right Clocks.
	 */

	if (bufs_with_client <= dcvs->max_threshold) {
		dcvs->load = dcvs->load_high;
		dcvs->dcvs_flags |= MSM_CVP_DCVS_INCR;
	} else if (bufs_with_fw < buf_reqs->buffer_count_min) {
		dcvs->load = dcvs->load_low;
		dcvs->dcvs_flags |= MSM_CVP_DCVS_DECR;
	} else {
		dcvs->load = dcvs->load_norm;
		dcvs->dcvs_flags = 0;
	}

	dprintk(CVP_PROF,
		"DCVS: %x : total bufs %d outside fw %d max threshold %d with fw %d min bufs %d flags %#x\n",
		hash32_ptr(inst->session), buf_reqs->buffer_count_actual,
		bufs_with_client, dcvs->max_threshold, bufs_with_fw,
		buf_reqs->buffer_count_min, dcvs->dcvs_flags);
	return rc;
}

static void msm_cvp_update_freq_entry(struct msm_cvp_inst *inst,
	unsigned long freq, u32 device_addr, bool is_turbo)
{
	struct cvp_freq_data *temp, *next;
	bool found = false;

	mutex_lock(&inst->freqs.lock);
	list_for_each_entry_safe(temp, next, &inst->freqs.list, list) {
		if (temp->device_addr == device_addr) {
			temp->freq = freq;
			found = true;
			break;
		}
	}

	if (!found) {
		temp = kzalloc(sizeof(*temp), GFP_KERNEL);
		if (!temp) {
			dprintk(CVP_WARN, "%s: malloc failure.\n", __func__);
			goto exit;
		}
		temp->freq = freq;
		temp->device_addr = device_addr;
		list_add_tail(&temp->list, &inst->freqs.list);
	}
	temp->turbo = !!is_turbo;
exit:
	mutex_unlock(&inst->freqs.lock);
}

void msm_cvp_clear_freq_entry(struct msm_cvp_inst *inst,
	u32 device_addr)
{
	struct cvp_freq_data *temp, *next;

	mutex_lock(&inst->freqs.lock);
	list_for_each_entry_safe(temp, next, &inst->freqs.list, list) {
		if (temp->device_addr == device_addr)
			temp->freq = 0;
	}
	mutex_unlock(&inst->freqs.lock);

	inst->clk_data.buffer_counter++;
}

static unsigned long msm_cvp_max_freq(struct msm_cvp_core *core)
{
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	unsigned long freq = 0;

	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	freq = allowed_clks_tbl[0].clock_rate;
	dprintk(CVP_PROF, "Max rate = %lu\n", freq);
	return freq;
}

void msm_cvp_comm_free_freq_table(struct msm_cvp_inst *inst)
{
	struct cvp_freq_data *temp, *next;

	mutex_lock(&inst->freqs.lock);
	list_for_each_entry_safe(temp, next, &inst->freqs.list, list) {
		list_del(&temp->list);
		kfree(temp);
	}
	INIT_LIST_HEAD(&inst->freqs.list);
	mutex_unlock(&inst->freqs.lock);
}

void msm_cvp_comm_free_input_cr_table(struct msm_cvp_inst *inst)
{
	struct cvp_input_cr_data *temp, *next;

	mutex_lock(&inst->input_crs.lock);
	list_for_each_entry_safe(temp, next, &inst->input_crs.list, list) {
		list_del(&temp->list);
		kfree(temp);
	}
	INIT_LIST_HEAD(&inst->input_crs.list);
	mutex_unlock(&inst->input_crs.lock);
}

void msm_cvp_comm_update_input_cr(struct msm_cvp_inst *inst,
	u32 index, u32 cr)
{
	struct cvp_input_cr_data *temp, *next;
	bool found = false;

	mutex_lock(&inst->input_crs.lock);
	list_for_each_entry_safe(temp, next, &inst->input_crs.list, list) {
		if (temp->index == index) {
			temp->input_cr = cr;
			found = true;
			break;
		}
	}

	if (!found) {
		temp = kzalloc(sizeof(*temp), GFP_KERNEL);
		if (!temp)  {
			dprintk(CVP_WARN, "%s: malloc failure.\n", __func__);
			goto exit;
		}
		temp->index = index;
		temp->input_cr = cr;
		list_add_tail(&temp->list, &inst->input_crs.list);
	}
exit:
	mutex_unlock(&inst->input_crs.lock);
}

static unsigned long msm_cvp_calc_freq_ar50(struct msm_cvp_inst *inst,
	u32 filled_len)
{
	unsigned long freq = 0;
	unsigned long vpp_cycles = 0, vsp_cycles = 0;
	u32 vpp_cycles_per_mb;
	u32 mbs_per_second;
	struct msm_cvp_core *core = NULL;
	int i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 rate = 0, fps;
	struct clock_data *dcvs = NULL;

	core = inst->core;
	dcvs = &inst->clk_data;

	mbs_per_second = msm_cvp_comm_get_inst_load_per_core(inst,
		LOAD_CALC_NO_QUIRKS);

	fps = msm_cvp_get_fps(inst);

	/*
	 * Calculate vpp, vsp cycles separately for encoder and decoder.
	 * Even though, most part is common now, in future it may change
	 * between them.
	 */

	if (inst->session_type == MSM_CVP_ENCODER) {
		vpp_cycles_per_mb = inst->flags & CVP_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			inst->clk_data.entry->vpp_cycles;

		vpp_cycles = mbs_per_second * vpp_cycles_per_mb;

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;

		/* 10 / 7 is overhead factor */
		vsp_cycles += (inst->clk_data.bitrate * 10) / 7;
	} else if (inst->session_type == MSM_CVP_DECODER) {
		vpp_cycles = mbs_per_second * inst->clk_data.entry->vpp_cycles;

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;
		/* 10 / 7 is overhead factor */
		vsp_cycles += ((fps * filled_len * 8) * 10) / 7;

	} else {
		dprintk(CVP_ERR, "Unknown session type = %s\n", __func__);
		return msm_cvp_max_freq(inst->core);
	}

	freq = max(vpp_cycles, vsp_cycles);

	dprintk(CVP_DBG, "Update DCVS Load\n");
	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq)
			break;
	}

	dcvs->load_norm = rate;
	dcvs->load_low = i < (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	msm_dcvs_print_dcvs_stats(dcvs);

	dprintk(CVP_PROF, "%s Inst %pK : Filled Len = %d Freq = %lu\n",
		__func__, inst, filled_len, freq);

	return freq;
}

static unsigned long msm_cvp_calc_freq(struct msm_cvp_inst *inst,
	u32 filled_len)
{
	unsigned long freq = 0;
	unsigned long vpp_cycles = 0, vsp_cycles = 0, fw_cycles = 0;
	u32 mbs_per_second;
	struct msm_cvp_core *core = NULL;
	int i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 rate = 0, fps;
	struct clock_data *dcvs = NULL;

	core = inst->core;
	dcvs = &inst->clk_data;

	mbs_per_second = msm_cvp_comm_get_inst_load_per_core(inst,
		LOAD_CALC_NO_QUIRKS);

	fps = msm_cvp_get_fps(inst);

	dprintk(CVP_ERR, "Unknown session type = %s\n", __func__);
	return msm_cvp_max_freq(inst->core);

	freq = max(vpp_cycles, vsp_cycles);
	freq = max(freq, fw_cycles);

	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq)
			break;
	}

	dcvs->load_norm = rate;
	dcvs->load_low = i < (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	dprintk(CVP_PROF,
		"%s: inst %pK: %x : filled len %d required freq %lu load_norm %lu\n",
		__func__, inst, hash32_ptr(inst->session),
		filled_len, freq, dcvs->load_norm);

	return freq;
}

int msm_cvp_set_clocks(struct msm_cvp_core *core)
{
	struct hfi_device *hdev;
	unsigned long freq_core_1 = 0, freq_core_2 = 0, rate = 0;
	unsigned long freq_core_max = 0;
	struct msm_cvp_inst *temp = NULL;
	int rc = 0, i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	bool increment, decrement;

	hdev = core->device;
	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	if (!allowed_clks_tbl) {
		dprintk(CVP_ERR,
			"%s Invalid parameters\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&core->lock);
	increment = false;
	decrement = true;
	list_for_each_entry(temp, &core->instances, list) {

		if (temp->clk_data.core_id == CVP_CORE_ID_1)
			freq_core_1 += temp->clk_data.min_freq;
		else if (temp->clk_data.core_id == CVP_CORE_ID_2)
			freq_core_2 += temp->clk_data.min_freq;
		else if (temp->clk_data.core_id == CVP_CORE_ID_3) {
			freq_core_1 += temp->clk_data.min_freq;
			freq_core_2 += temp->clk_data.min_freq;
		}

		freq_core_max = max_t(unsigned long, freq_core_1, freq_core_2);

		if (msm_cvp_clock_voting) {
			dprintk(CVP_PROF,
				"msm_cvp_clock_voting %d\n",
				 msm_cvp_clock_voting);
			freq_core_max = msm_cvp_clock_voting;
			break;
		}

		if (temp->clk_data.turbo_mode) {
			dprintk(CVP_PROF,
				"Found an instance with Turbo request\n");
			freq_core_max = msm_cvp_max_freq(core);
			break;
		}
		/* increment even if one session requested for it */
		if (temp->clk_data.dcvs_flags & MSM_CVP_DCVS_INCR)
			increment = true;
		/* decrement only if all sessions requested for it */
		if (!(temp->clk_data.dcvs_flags & MSM_CVP_DCVS_DECR))
			decrement = false;
	}

	/*
	 * keep checking from lowest to highest rate until
	 * table rate >= requested rate
	 */
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq_core_max)
			break;
	}
	if (increment) {
		if (i > 0)
			rate = allowed_clks_tbl[i-1].clock_rate;
	} else if (decrement) {
		if (i < (core->resources.allowed_clks_tbl_size - 1))
			rate = allowed_clks_tbl[i+1].clock_rate;
	}

	core->min_freq = freq_core_max;
	core->curr_freq = rate;
	mutex_unlock(&core->lock);

	dprintk(CVP_PROF,
		"%s: clock rate %lu requested %lu increment %d decrement %d\n",
		__func__, core->curr_freq, core->min_freq,
		increment, decrement);
	rc = call_hfi_op(hdev, scale_clocks,
			hdev->hfi_device_data, core->curr_freq);

	return rc;
}

int msm_cvp_validate_operating_rate(struct msm_cvp_inst *inst,
	u32 operating_rate)
{
	struct msm_cvp_inst *temp;
	struct msm_cvp_core *core;
	unsigned long max_freq, freq_left, ops_left, load, cycles, freq = 0;
	unsigned long mbs_per_second;
	int rc = 0;
	u32 curr_operating_rate = 0;

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s Invalid args\n", __func__);
		return -EINVAL;
	}
	core = inst->core;
	curr_operating_rate = inst->clk_data.operating_rate >> 16;

	mutex_lock(&core->lock);
	max_freq = msm_cvp_max_freq(core);
	list_for_each_entry(temp, &core->instances, list) {
		if (temp == inst ||
				temp->state < MSM_CVP_START_DONE ||
				temp->state >= MSM_CVP_RELEASE_RESOURCES_DONE)
			continue;

		freq += temp->clk_data.min_freq;
	}

	freq_left = max_freq - freq;

	mbs_per_second = msm_cvp_comm_get_inst_load_per_core(inst,
		LOAD_CALC_NO_QUIRKS);

	cycles = inst->clk_data.entry->vpp_cycles;
	if (inst->session_type == MSM_CVP_ENCODER)
		cycles = inst->flags & CVP_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			cycles;

	load = cycles * mbs_per_second;

	ops_left = load ? (freq_left / load) : 0;

	operating_rate = operating_rate >> 16;

	if ((curr_operating_rate * (1 + ops_left)) >= operating_rate ||
			msm_cvp_clock_voting ||
			inst->clk_data.buffer_counter < DCVS_FTB_WINDOW) {
		dprintk(CVP_DBG,
			"Requestd operating rate is valid %u\n",
			operating_rate);
		rc = 0;
	} else {
		dprintk(CVP_DBG,
			"Current load is high for requested settings. Cannot set operating rate to %u\n",
			operating_rate);
		rc = -EINVAL;
	}
	mutex_unlock(&core->lock);

	return rc;
}

int msm_cvp_comm_scale_clocks(struct msm_cvp_inst *inst)
{
	struct msm_video_buffer *temp, *next;
	unsigned long freq = 0;
	u32 filled_len = 0;
	u32 device_addr = 0;
	bool is_turbo = false;

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	if (!inst->core->resources.bus_devfreq_on)
		return 0;

	mutex_lock(&inst->registeredbufs.lock);
	list_for_each_entry_safe(temp, next, &inst->registeredbufs.list, list) {
		if (temp->vvb.vb2_buf.type ==
				V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
			filled_len = max(filled_len,
				temp->vvb.vb2_buf.planes[0].bytesused);
			if (inst->session_type == MSM_CVP_ENCODER &&
				(temp->vvb.flags &
				 V4L2_BUF_FLAG_PERF_MODE)) {
				is_turbo = true;
			}
			device_addr = temp->smem[0].device_addr;
		}
	}
	mutex_unlock(&inst->registeredbufs.lock);

	if (!filled_len || !device_addr) {
		dprintk(CVP_DBG, "%s no input for session %x\n",
			__func__, hash32_ptr(inst->session));
		goto no_clock_change;
	}

	freq = call_core_op(inst->core, calc_freq, inst, filled_len);
	inst->clk_data.min_freq = freq;
	/* update dcvs flags */
	msm_dcvs_scale_clocks(inst, freq);

	if (inst->clk_data.buffer_counter < DCVS_FTB_WINDOW || is_turbo ||
		msm_cvp_clock_voting) {
		inst->clk_data.min_freq = msm_cvp_max_freq(inst->core);
		inst->clk_data.dcvs_flags = 0;
	}

	msm_cvp_update_freq_entry(inst, freq, device_addr, is_turbo);

	msm_cvp_set_clocks(inst->core);

no_clock_change:
	return 0;
}

int msm_cvp_comm_scale_clocks_and_bus(struct msm_cvp_inst *inst)
{
	struct msm_cvp_core *core;
	struct hfi_device *hdev;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(CVP_ERR, "%s Invalid params\n", __func__);
		return -EINVAL;
	}
	core = inst->core;
	hdev = core->device;

	if (msm_cvp_comm_scale_clocks(inst)) {
		dprintk(CVP_WARN,
			"Failed to scale clocks. Performance might be impacted\n");
	}
	if (msm_cvp_comm_vote_bus(core)) {
		dprintk(CVP_WARN,
			"Failed to scale DDR bus. Performance might be impacted\n");
	}
	return 0;
}

int msm_cvp_dcvs_try_enable(struct msm_cvp_inst *inst)
{
	if (!inst) {
		dprintk(CVP_ERR, "%s: Invalid args: %p\n", __func__, inst);
		return -EINVAL;
	}

	if (msm_cvp_clock_voting ||
			inst->flags & CVP_THUMBNAIL ||
			inst->clk_data.low_latency_mode ||
			inst->batch.enable) {
		dprintk(CVP_PROF, "DCVS disabled: %pK\n", inst);
		inst->clk_data.dcvs_mode = false;
		return false;
	}
	inst->clk_data.dcvs_mode = true;
	dprintk(CVP_PROF, "DCVS enabled: %pK\n", inst);

	return true;
}

int msm_cvp_comm_init_clocks_and_bus_data(struct msm_cvp_inst *inst)
{
	int rc = 0, j = 0;
	int fourcc, count;

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s Invalid args: Inst = %pK\n",
				__func__, inst);
		return -EINVAL;
	}

	if (inst->session_type == MSM_CVP_CORE) {
		dprintk(CVP_DBG, "%s: cvp session\n", __func__);
		return 0;
	}

	count = inst->core->resources.codec_data_count;
	fourcc = inst->session_type == MSM_CVP_DECODER ?
		inst->fmts[OUTPUT_PORT].fourcc :
		inst->fmts[CAPTURE_PORT].fourcc;

	for (j = 0; j < count; j++) {
		if (inst->core->resources.codec_data[j].session_type ==
				inst->session_type &&
				inst->core->resources.codec_data[j].fourcc ==
				fourcc) {
			inst->clk_data.entry =
				&inst->core->resources.codec_data[j];
			break;
		}
	}

	if (!inst->clk_data.entry) {
		dprintk(CVP_ERR, "%s No match found\n", __func__);
		rc = -EINVAL;
	}

	return rc;
}

void msm_cvp_clock_data_reset(struct msm_cvp_inst *inst)
{
	struct msm_cvp_core *core;
	int i = 0, rc = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 total_freq = 0, rate = 0, load;
	int cycles;
	struct clock_data *dcvs;
	struct hal_buffer_requirements *buf_req;

	dprintk(CVP_DBG, "Init DCVS Load\n");

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return;
	}

	core = inst->core;
	dcvs = &inst->clk_data;
	load = msm_cvp_comm_get_inst_load_per_core(inst, LOAD_CALC_NO_QUIRKS);
	cycles = inst->clk_data.entry->vpp_cycles;
	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	if (inst->session_type == MSM_CVP_ENCODER) {
		cycles = inst->flags & CVP_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			cycles;

		dcvs->buffer_type = HAL_BUFFER_INPUT;
		dcvs->min_threshold =
			msm_cvp_get_extra_buff_count(inst, HAL_BUFFER_INPUT);
		buf_req = get_cvp_buff_req_buffer(inst, HAL_BUFFER_INPUT);
		if (buf_req)
			dcvs->max_threshold =
				buf_req->buffer_count_actual -
				buf_req->buffer_count_min_host + 2;
		else
			dprintk(CVP_ERR,
				"%s: No bufer req for buffer type %x\n",
				__func__, HAL_BUFFER_INPUT);

	} else if (inst->session_type == MSM_CVP_DECODER) {
		dcvs->buffer_type = msm_cvp_comm_get_hal_output_buffer(inst);
		buf_req = get_cvp_buff_req_buffer(inst, dcvs->buffer_type);
		if (buf_req)
			dcvs->max_threshold =
				buf_req->buffer_count_actual -
				buf_req->buffer_count_min_host + 2;
		else
			dprintk(CVP_ERR,
				"%s: No bufer req for buffer type %x\n",
				__func__, dcvs->buffer_type);

		dcvs->min_threshold =
			msm_cvp_get_extra_buff_count(inst, dcvs->buffer_type);
	} else {
		dprintk(CVP_ERR, "%s: invalid session type %#x\n",
			__func__, inst->session_type);
		return;
	}

	total_freq = cycles * load;

	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= total_freq)
			break;
	}

	dcvs->load = dcvs->load_norm = rate;

	dcvs->load_low = i < (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	inst->clk_data.buffer_counter = 0;

	msm_dcvs_print_dcvs_stats(dcvs);

	rc = msm_cvp_comm_scale_clocks_and_bus(inst);

	if (rc)
		dprintk(CVP_ERR, "%s Failed to scale Clocks and Bus\n",
			__func__);
}

static bool is_output_buffer(struct msm_cvp_inst *inst,
	enum hal_buffer buffer_type)
{
	if (msm_cvp_comm_get_stream_output_mode(inst) ==
			HAL_VIDEO_DECODER_SECONDARY) {
		return buffer_type == HAL_BUFFER_OUTPUT2;
	} else {
		return buffer_type == HAL_BUFFER_OUTPUT;
	}
}

int msm_cvp_get_extra_buff_count(struct msm_cvp_inst *inst,
	enum hal_buffer buffer_type)
{
	int count = 0;

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s Invalid args\n", __func__);
		return 0;
	}
	/*
	 * no extra buffers for thumbnail session because
	 * neither dcvs nor batching will be enabled
	 */
	if (is_thumbnail_session(inst))
		return 0;

	/* Add DCVS extra buffer count */
	if (inst->core->resources.dcvs) {
		if (is_decode_session(inst) &&
			is_output_buffer(inst, buffer_type)) {
			count += DCVS_DEC_EXTRA_OUTPUT_BUFFERS;
		} else if ((is_encode_session(inst) &&
			buffer_type == HAL_BUFFER_INPUT)) {
			count += DCVS_ENC_EXTRA_INPUT_BUFFERS;
		}
	}

	/*
	 * if platform supports decode batching ensure minimum
	 * batch size count of extra buffers added on output port
	 */
	if (is_output_buffer(inst, buffer_type)) {
		if (inst->core->resources.decode_batching &&
			is_decode_session(inst) &&
			count < inst->batch.size)
			count = inst->batch.size;
	}

	return count;
}

int msm_cvp_decide_work_route(struct msm_cvp_inst *inst)
{
	return -EINVAL;
}

static int msm_cvp_decide_work_mode_ar50(struct msm_cvp_inst *inst)
{
	int rc = 0;
	struct hfi_device *hdev;
	struct hal_video_work_mode pdata;
	struct hal_enable latency;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(CVP_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	hdev = inst->core->device;
	if (inst->clk_data.low_latency_mode) {
		pdata.video_work_mode = CVP_WORK_MODE_1;
		goto decision_done;
	}

	if (inst->session_type == MSM_CVP_DECODER) {
		pdata.video_work_mode = CVP_WORK_MODE_2;
		switch (inst->fmts[OUTPUT_PORT].fourcc) {
		case V4L2_PIX_FMT_MPEG2:
			pdata.video_work_mode = CVP_WORK_MODE_1;
			break;
		case V4L2_PIX_FMT_H264:
		case V4L2_PIX_FMT_HEVC:
			if (inst->prop.height[OUTPUT_PORT] *
				inst->prop.width[OUTPUT_PORT] <=
					1280 * 720)
				pdata.video_work_mode = CVP_WORK_MODE_1;
			break;
		}
	} else if (inst->session_type == MSM_CVP_ENCODER)
		pdata.video_work_mode = CVP_WORK_MODE_1;
	else {
		return -EINVAL;
	}

decision_done:

	inst->clk_data.work_mode = pdata.video_work_mode;
	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HAL_PARAM_VIDEO_WORK_MODE,
			(void *)&pdata);
	if (rc)
		dprintk(CVP_WARN,
				" Failed to configure Work Mode %pK\n", inst);

	/* For WORK_MODE_1, set Low Latency mode by default to HW. */

	if (inst->session_type == MSM_CVP_ENCODER &&
			inst->clk_data.work_mode == CVP_WORK_MODE_1) {
		latency.enable = true;
		rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HAL_PARAM_VENC_LOW_LATENCY,
			(void *)&latency);
	}

	rc = msm_cvp_comm_scale_clocks_and_bus(inst);

	return rc;
}

int msm_cvp_decide_work_mode(struct msm_cvp_inst *inst)
{
	return -EINVAL;
}

static inline int msm_cvp_power_save_mode_enable(struct msm_cvp_inst *inst,
	bool enable)
{
	u32 rc = 0, mbs_per_frame;
	u32 prop_id = 0;
	void *pdata = NULL;
	struct hfi_device *hdev = NULL;
	enum hal_perf_mode venc_mode;
	u32 rc_mode = 0;

	hdev = inst->core->device;
	if (inst->session_type != MSM_CVP_ENCODER) {
		dprintk(CVP_DBG,
			"%s : Not an encoder session. Nothing to do\n",
				__func__);
		return 0;
	}
	mbs_per_frame = msm_cvp_get_mbs_per_frame(inst);
	if (mbs_per_frame > inst->core->resources.max_hq_mbs_per_frame ||
		msm_cvp_get_fps(inst) > inst->core->resources.max_hq_fps) {
		enable = true;
	}
	/* Power saving always disabled for CQ RC mode. */
	rc_mode = msm_cvp_comm_g_ctrl_for_id(inst,
		V4L2_CID_MPEG_VIDEO_BITRATE_MODE);
	if (rc_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ)
		enable = false;

	prop_id = HAL_CONFIG_VENC_PERF_MODE;
	venc_mode = enable ? HAL_PERF_MODE_POWER_SAVE :
		HAL_PERF_MODE_POWER_MAX_QUALITY;
	pdata = &venc_mode;
	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, prop_id, pdata);
	if (rc) {
		dprintk(CVP_ERR,
			"%s: Failed to set power save mode for inst: %pK\n",
			__func__, inst);
		goto fail_power_mode_set;
	}
	inst->flags = enable ?
		inst->flags | CVP_LOW_POWER :
		inst->flags & ~CVP_LOW_POWER;

	dprintk(CVP_PROF,
		"Power Save Mode for inst: %pK Enable = %d\n", inst, enable);
fail_power_mode_set:
	return rc;
}

static int msm_cvp_move_core_to_power_save_mode(struct msm_cvp_core *core,
	u32 core_id)
{
	struct msm_cvp_inst *inst = NULL;

	dprintk(CVP_PROF, "Core %d : Moving all inst to LP mode\n", core_id);
	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		if (inst->clk_data.core_id == core_id &&
			inst->session_type == MSM_CVP_ENCODER)
			msm_cvp_power_save_mode_enable(inst, true);
	}
	mutex_unlock(&core->lock);

	return 0;
}

static u32 get_core_load(struct msm_cvp_core *core,
	u32 core_id, bool lp_mode, bool real_time)
{
	struct msm_cvp_inst *inst = NULL;
	u32 current_inst_mbs_per_sec = 0, load = 0;
	bool real_time_mode = false;

	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		u32 cycles, lp_cycles;

		real_time_mode = inst->flags & CVP_REALTIME ? true : false;
		if (!(inst->clk_data.core_id & core_id))
			continue;
		if (real_time_mode != real_time)
			continue;
		if (inst->session_type == MSM_CVP_DECODER) {
			cycles = lp_cycles = inst->clk_data.entry->vpp_cycles;
		} else if (inst->session_type == MSM_CVP_ENCODER) {
			lp_mode |= inst->flags & CVP_LOW_POWER;
			cycles = lp_mode ?
				inst->clk_data.entry->low_power_cycles :
				inst->clk_data.entry->vpp_cycles;
		} else {
			continue;
		}
		current_inst_mbs_per_sec =
			msm_cvp_comm_get_inst_load_per_core(inst,
			LOAD_CALC_NO_QUIRKS);
		load += current_inst_mbs_per_sec * cycles /
			inst->clk_data.work_route;
	}
	mutex_unlock(&core->lock);

	return load;
}

int msm_cvp_decide_core_and_power_mode(
	struct msm_cvp_inst *inst)
{
	int rc = 0, hier_mode = 0;
	struct hfi_device *hdev;
	struct msm_cvp_core *core;
	unsigned long max_freq, lp_cycles = 0;
	struct hal_videocores_usage_info core_info;
	u32 core0_load = 0, core1_load = 0, core0_lp_load = 0,
		core1_lp_load = 0;
	u32 current_inst_load = 0, current_inst_lp_load = 0,
		min_load = 0, min_lp_load = 0;
	u32 min_core_id, min_lp_core_id;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(CVP_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	core = inst->core;
	hdev = core->device;
	max_freq = msm_cvp_max_freq(inst->core);
	inst->clk_data.core_id = 0;

	core0_load = get_core_load(core, CVP_CORE_ID_1, false, true);
	core1_load = get_core_load(core, CVP_CORE_ID_2, false, true);
	core0_lp_load = get_core_load(core, CVP_CORE_ID_1, true, true);
	core1_lp_load = get_core_load(core, CVP_CORE_ID_2, true, true);

	min_load = min(core0_load, core1_load);
	min_core_id = core0_load < core1_load ?
		CVP_CORE_ID_1 : CVP_CORE_ID_2;
	min_lp_load = min(core0_lp_load, core1_lp_load);
	min_lp_core_id = core0_lp_load < core1_lp_load ?
		CVP_CORE_ID_1 : CVP_CORE_ID_2;

	lp_cycles = inst->session_type == MSM_CVP_ENCODER ?
			inst->clk_data.entry->low_power_cycles :
			inst->clk_data.entry->vpp_cycles;
	/*
	 * Incase there is only 1 core enabled, mark it as the core
	 * with min load. This ensures that this core is selected and
	 * video session is set to run on the enabled core.
	 */
	if (inst->capability.max_video_cores.max <= CVP_CORE_ID_1) {
		min_core_id = min_lp_core_id = CVP_CORE_ID_1;
		min_load = core0_load;
		min_lp_load = core0_lp_load;
	}

	current_inst_load =
		(msm_cvp_comm_get_inst_load(inst, LOAD_CALC_NO_QUIRKS) *
		inst->clk_data.entry->vpp_cycles)/inst->clk_data.work_route;

	current_inst_lp_load = (msm_cvp_comm_get_inst_load(inst,
		LOAD_CALC_NO_QUIRKS) * lp_cycles)/inst->clk_data.work_route;

	dprintk(CVP_DBG, "Core 0 RT Load = %d Core 1 RT Load = %d\n",
		 core0_load, core1_load);
	dprintk(CVP_DBG, "Core 0 RT LP Load = %d\n",
		core0_lp_load);
	dprintk(CVP_DBG, "Core 1 RT LP Load = %d\n",
		core1_lp_load);
	dprintk(CVP_DBG, "Max Load = %lu\n", max_freq);
	dprintk(CVP_DBG, "Current Load = %d Current LP Load = %d\n",
		current_inst_load, current_inst_lp_load);

	/* Hier mode can be normal HP or Hybrid HP. */

	hier_mode = 0; // msm_cvp_comm_g_ctrl_for_id(inst,
		// V4L2_CID_MPEG_VIDC_VIDEO_HIER_P_NUM_LAYERS);
	hier_mode |= 0; //msm_cvp_comm_g_ctrl_for_id(inst,
		//V4L2_CID_MPEG_VIDC_VIDEO_HYBRID_HIERP_MODE);

	if (current_inst_load + min_load < max_freq) {
		inst->clk_data.core_id = min_core_id;
		dprintk(CVP_DBG,
			"Selected normally : Core ID = %d\n",
				inst->clk_data.core_id);
		msm_cvp_power_save_mode_enable(inst, false);
	} else if (current_inst_lp_load + min_load < max_freq) {
		/* Move current instance to LP and return */
		inst->clk_data.core_id = min_core_id;
		dprintk(CVP_DBG,
			"Selected by moving current to LP : Core ID = %d\n",
				inst->clk_data.core_id);
		msm_cvp_power_save_mode_enable(inst, true);

	} else if (current_inst_lp_load + min_lp_load < max_freq) {
		/* Move all instances to LP mode and return */
		inst->clk_data.core_id = min_lp_core_id;
		dprintk(CVP_DBG,
			"Moved all inst's to LP: Core ID = %d\n",
				inst->clk_data.core_id);
		msm_cvp_move_core_to_power_save_mode(core, min_lp_core_id);
	} else {
		rc = -EINVAL;
		dprintk(CVP_ERR,
			"Sorry ... Core Can't support this load\n");
		return rc;
	}

	core_info.video_core_enable_mask = inst->clk_data.core_id;
	dprintk(CVP_DBG,
		"Core Enable Mask %d\n", core_info.video_core_enable_mask);

	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session,
			HAL_PARAM_VIDEO_CORES_USAGE, &core_info);
	if (rc)
		dprintk(CVP_WARN,
				" Failed to configure CORE ID %pK\n", inst);

	rc = msm_cvp_comm_scale_clocks_and_bus(inst);

	msm_cvp_print_core_status(core, CVP_CORE_ID_1);
	msm_cvp_print_core_status(core, CVP_CORE_ID_2);

	return rc;
}

void msm_cvp_init_core_clk_ops(struct msm_cvp_core *core)
{
	if (!core)
		return;

	if (core->platform_data->vpu_ver == VPU_VERSION_4)
		core->core_ops = &cvp_core_ops_vpu4;
	else
		core->core_ops = &cvp_core_ops_vpu5;
}

void msm_cvp_print_core_status(struct msm_cvp_core *core, u32 core_id)
{
	struct msm_cvp_inst *inst = NULL;

	dprintk(CVP_PROF, "Instances running on core %u", core_id);
	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {

		if ((inst->clk_data.core_id != core_id) &&
			(inst->clk_data.core_id != CVP_CORE_ID_3))
			continue;

		dprintk(CVP_PROF,
			"inst %pK (%4ux%4u) to (%4ux%4u) %3u %s %s %s %s %lu\n",
			inst,
			inst->prop.width[OUTPUT_PORT],
			inst->prop.height[OUTPUT_PORT],
			inst->prop.width[CAPTURE_PORT],
			inst->prop.height[CAPTURE_PORT],
			inst->prop.fps,
			inst->session_type == MSM_CVP_ENCODER ? "ENC" : "DEC",
			inst->clk_data.work_mode == CVP_WORK_MODE_1 ?
				"WORK_MODE_1" : "WORK_MODE_2",
			inst->flags & CVP_LOW_POWER ? "LP" : "HQ",
			inst->flags & CVP_REALTIME ? "RealTime" : "NonRTime",
			inst->clk_data.min_freq);
	}
	mutex_unlock(&core->lock);
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#define CREATE_TRACE_POINTS
#define MAX_SSR_STRING_LEN 10
#include "msm_cvp_debug.h"
#include "cvp_hfi_api.h"

int msm_cvp_debug = CVP_ERR | CVP_WARN | CVP_DBG;
EXPORT_SYMBOL(msm_cvp_debug);

int msm_cvp_debug_out = CVP_OUT_PRINTK;
EXPORT_SYMBOL(msm_cvp_debug_out);

int msm_cvp_fw_debug = 0x18;
int msm_cvp_fw_debug_mode = 1;
int msm_cvp_fw_low_power_mode = 1;
bool msm_cvp_fw_coverage = !true;
bool msm_cvp_thermal_mitigation_disabled = !true;
int msm_cvp_clock_voting = !1;
bool msm_cvp_syscache_disable = !true;

#define MAX_DBG_BUF_SIZE 4096

#define DYNAMIC_BUF_OWNER(__binfo) ({ \
	atomic_read(&__binfo->ref_count) >= 2 ? "video driver" : "firmware";\
})

struct core_inst_pair {
	struct msm_cvp_core *core;
	struct msm_cvp_inst *inst;
};

static int core_info_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	dprintk(CVP_INFO, "%s: Enter\n", __func__);
	return 0;
}

static u32 write_str(char *buffer,
		size_t size, const char *fmt, ...)
{
	va_list args;
	u32 len;

	va_start(args, fmt);
	len = vscnprintf(buffer, size, fmt, args);
	va_end(args);
	return len;
}

static ssize_t core_info_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct msm_cvp_core *core = file->private_data;
	struct hfi_device *hdev;
	struct hal_fw_info fw_info = { {0} };
	char *dbuf, *cur, *end;
	int i = 0, rc = 0;
	ssize_t len = 0;

	if (!core || !core->device) {
		dprintk(CVP_ERR, "Invalid params, core: %pK\n", core);
		return 0;
	}

	dbuf = kzalloc(MAX_DBG_BUF_SIZE, GFP_KERNEL);
	if (!dbuf) {
		dprintk(CVP_ERR, "%s: Allocation failed!\n", __func__);
		return -ENOMEM;
	}
	cur = dbuf;
	end = cur + MAX_DBG_BUF_SIZE;
	hdev = core->device;

	cur += write_str(cur, end - cur, "===============================\n");
	cur += write_str(cur, end - cur, "CORE %d: %pK\n", core->id, core);
	cur += write_str(cur, end - cur, "===============================\n");
	cur += write_str(cur, end - cur, "Core state: %d\n", core->state);
	rc = call_hfi_op(hdev, get_fw_info, hdev->hfi_device_data, &fw_info);
	if (rc) {
		dprintk(CVP_WARN, "Failed to read FW info\n");
		goto err_fw_info;
	}

	cur += write_str(cur, end - cur,
		"FW version : %s\n", &fw_info.version);
	cur += write_str(cur, end - cur,
		"base addr: 0x%x\n", fw_info.base_addr);
	cur += write_str(cur, end - cur,
		"register_base: 0x%x\n", fw_info.register_base);
	cur += write_str(cur, end - cur,
		"register_size: %u\n", fw_info.register_size);
	cur += write_str(cur, end - cur, "irq: %u\n", fw_info.irq);

err_fw_info:
	for (i = SYS_MSG_START; i < SYS_MSG_END; i++) {
		cur += write_str(cur, end - cur, "completions[%d]: %s\n", i,
			completion_done(&core->completions[SYS_MSG_INDEX(i)]) ?
			"pending" : "done");
	}
	len = simple_read_from_buffer(buf, count, ppos,
			dbuf, cur - dbuf);

	kfree(dbuf);
	return len;
}

static const struct file_operations core_info_fops = {
	.open = core_info_open,
	.read = core_info_read,
};

static int trigger_ssr_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	dprintk(CVP_INFO, "%s: Enter\n", __func__);
	return 0;
}

static ssize_t trigger_ssr_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *ppos)
{
	unsigned long ssr_trigger_val = 0;
	int rc = 0;
	struct msm_cvp_core *core = filp->private_data;
	size_t size = MAX_SSR_STRING_LEN;
	char kbuf[MAX_SSR_STRING_LEN + 1] = {0};

	if (!buf)
		return -EINVAL;

	if (!count)
		goto exit;

	if (count < size)
		size = count;

	if (copy_from_user(kbuf, buf, size)) {
		dprintk(CVP_WARN, "%s User memory fault\n", __func__);
		rc = -EFAULT;
		goto exit;
	}

	rc = kstrtoul(kbuf, 0, &ssr_trigger_val);
	if (rc) {
		dprintk(CVP_WARN, "returning error err %d\n", rc);
		rc = -EINVAL;
	} else {
		msm_cvp_trigger_ssr(core, ssr_trigger_val);
		rc = count;
	}
exit:
	return rc;
}

static const struct file_operations ssr_fops = {
	.open = trigger_ssr_open,
	.write = trigger_ssr_write,
};

struct dentry *msm_cvp_debugfs_init_drv(void)
{
	bool ok = false;
	struct dentry *dir = NULL;

	dir = debugfs_create_dir("msm_cvp", NULL);
	if (IS_ERR_OR_NULL(dir)) {
		dir = NULL;
		goto failed_create_dir;
	}

#define __debugfs_create(__type, __name, __value) ({                          \
	struct dentry *f = debugfs_create_##__type(__name, 0644,	\
		dir, __value);                                                \
	if (IS_ERR_OR_NULL(f)) {                                              \
		dprintk(CVP_ERR, "Failed creating debugfs file '%pd/%s'\n",  \
			dir, __name);                                         \
		f = NULL;                                                     \
	}                                                                     \
	f;                                                                    \
})

	ok =
	__debugfs_create(x32, "debug_level", &msm_cvp_debug) &&
	__debugfs_create(x32, "fw_level", &msm_cvp_fw_debug) &&
	__debugfs_create(u32, "fw_debug_mode", &msm_cvp_fw_debug_mode) &&
	__debugfs_create(bool, "fw_coverage", &msm_cvp_fw_coverage) &&
	__debugfs_create(u32, "fw_low_power_mode",
			&msm_cvp_fw_low_power_mode) &&
	__debugfs_create(u32, "debug_output", &msm_cvp_debug_out) &&
	__debugfs_create(bool, "disable_thermal_mitigation",
			&msm_cvp_thermal_mitigation_disabled) &&
	__debugfs_create(u32, "core_clock_voting",
			&msm_cvp_clock_voting) &&
	__debugfs_create(bool, "disable_video_syscache",
			&msm_cvp_syscache_disable);

#undef __debugfs_create

	if (!ok)
		goto failed_create_dir;

	return dir;

failed_create_dir:
	if (dir)
		debugfs_remove_recursive(cvp_driver->debugfs_root);

	return NULL;
}

struct dentry *msm_cvp_debugfs_init_core(struct msm_cvp_core *core,
		struct dentry *parent)
{
	struct dentry *dir = NULL;
	char debugfs_name[MAX_DEBUGFS_NAME];

	if (!core) {
		dprintk(CVP_ERR, "Invalid params, core: %pK\n", core);
		goto failed_create_dir;
	}

	snprintf(debugfs_name, MAX_DEBUGFS_NAME, "core%d", core->id);
	dir = debugfs_create_dir(debugfs_name, parent);
	if (!dir) {
		dprintk(CVP_ERR, "Failed to create debugfs for msm_cvp\n");
		goto failed_create_dir;
	}
	if (!debugfs_create_file("info", 0444, dir, core, &core_info_fops)) {
		dprintk(CVP_ERR, "debugfs_create_file: fail\n");
		goto failed_create_dir;
	}
	if (!debugfs_create_file("trigger_ssr", 0200,
			dir, core, &ssr_fops)) {
		dprintk(CVP_ERR, "debugfs_create_file: fail\n");
		goto failed_create_dir;
	}
failed_create_dir:
	return dir;
}

static int inst_info_open(struct inode *inode, struct file *file)
{
	dprintk(CVP_INFO, "Open inode ptr: %pK\n", inode->i_private);
	file->private_data = inode->i_private;
	return 0;
}

static int publish_unreleased_reference(struct msm_cvp_inst *inst,
		char **dbuf, char *end)
{
	struct msm_video_buffer *temp = NULL;
	char *cur = *dbuf;

	if (!inst) {
		dprintk(CVP_ERR, "%s: invalid param\n", __func__);
		return -EINVAL;
	}

	if (inst->buffer_mode_set[CAPTURE_PORT] == HAL_BUFFER_MODE_DYNAMIC) {
		cur += write_str(cur, end - cur, "Pending buffer references\n");

		mutex_lock(&inst->registeredbufs.lock);
		list_for_each_entry(temp, &inst->registeredbufs.list, list) {
			struct vb2_buffer *vb2 = &temp->vvb.vb2_buf;

			if (vb2->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
				cur += write_str(cur, end - cur,
				"\tbuffer: %#x fd[0] = %d size %d refcount = %d\n",
				temp->smem[0].device_addr,
				vb2->planes[0].m.fd,
				vb2->planes[0].length,
				temp->smem[0].refcount);
			}
		}
		mutex_unlock(&inst->registeredbufs.lock);
	}

	*dbuf = cur;
	return 0;
}

static void put_inst_helper(struct kref *kref)
{
	struct msm_cvp_inst *inst = container_of(kref,
			struct msm_cvp_inst, kref);

	msm_cvp_destroy(inst);
}

static ssize_t inst_info_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct core_inst_pair *idata = file->private_data;
	struct msm_cvp_core *core;
	struct msm_cvp_inst *inst, *temp = NULL;
	char *dbuf, *cur, *end;
	int i, j;
	ssize_t len = 0;

	if (!idata || !idata->core || !idata->inst) {
		dprintk(CVP_ERR, "%s: Invalid params\n", __func__);
		return 0;
	}

	core = idata->core;
	inst = idata->inst;

	mutex_lock(&core->lock);
	list_for_each_entry(temp, &core->instances, list) {
		if (temp == inst)
			break;
	}
	inst = ((temp == inst) && kref_get_unless_zero(&inst->kref)) ?
		inst : NULL;
	mutex_unlock(&core->lock);

	if (!inst) {
		dprintk(CVP_ERR, "%s: Instance has become obsolete", __func__);
		return 0;
	}

	dbuf = kzalloc(MAX_DBG_BUF_SIZE, GFP_KERNEL);
	if (!dbuf) {
		dprintk(CVP_ERR, "%s: Allocation failed!\n", __func__);
		len = -ENOMEM;
		goto failed_alloc;
	}
	cur = dbuf;
	end = cur + MAX_DBG_BUF_SIZE;

	cur += write_str(cur, end - cur, "==============================\n");
	cur += write_str(cur, end - cur, "INSTANCE: %pK (%s)\n", inst,
		inst->session_type == MSM_CVP_ENCODER ? "Encoder" : "Decoder");
	cur += write_str(cur, end - cur, "==============================\n");
	cur += write_str(cur, end - cur, "core: %pK\n", inst->core);
	cur += write_str(cur, end - cur, "height: %d\n",
		inst->prop.height[CAPTURE_PORT]);
	cur += write_str(cur, end - cur, "width: %d\n",
		inst->prop.width[CAPTURE_PORT]);
	cur += write_str(cur, end - cur, "fps: %d\n", inst->prop.fps);
	cur += write_str(cur, end - cur, "state: %d\n", inst->state);
	cur += write_str(cur, end - cur, "secure: %d\n",
		!!(inst->flags & CVP_SECURE));
	cur += write_str(cur, end - cur, "-----------Formats-------------\n");
	for (i = 0; i < MAX_PORT_NUM; i++) {
		cur += write_str(cur, end - cur, "capability: %s\n",
			i == OUTPUT_PORT ? "Output" : "Capture");
		cur += write_str(cur, end - cur, "name : %s\n",
			inst->fmts[i].name);
		cur += write_str(cur, end - cur, "planes : %d\n",
			inst->bufq[i].num_planes);
		cur += write_str(cur, end - cur,
			"type: %s\n", inst->fmts[i].type == OUTPUT_PORT ?
			"Output" : "Capture");
		switch (inst->buffer_mode_set[i]) {
		case HAL_BUFFER_MODE_STATIC:
			cur += write_str(cur, end - cur,
				"buffer mode : %s\n", "static");
			break;
		case HAL_BUFFER_MODE_DYNAMIC:
			cur += write_str(cur, end - cur,
				"buffer mode : %s\n", "dynamic");
			break;
		default:
			cur += write_str(cur, end - cur,
				"buffer mode : unsupported\n");
		}

		cur += write_str(cur, end - cur, "count: %u\n",
				inst->bufq[i].vb2_bufq.num_buffers);

		for (j = 0; j < inst->bufq[i].num_planes; j++)
			cur += write_str(cur, end - cur,
				"size for plane %d: %u\n",
				j, inst->bufq[i].plane_sizes[j]);

		if (i < MAX_PORT_NUM - 1)
			cur += write_str(cur, end - cur, "\n");
	}
	cur += write_str(cur, end - cur, "-------------------------------\n");
	for (i = SESSION_MSG_START; i < SESSION_MSG_END; i++) {
		cur += write_str(cur, end - cur, "completions[%d]: %s\n", i,
		completion_done(&inst->completions[SESSION_MSG_INDEX(i)]) ?
		"pending" : "done");
	}
	cur += write_str(cur, end - cur, "ETB Count: %d\n", inst->count.etb);
	cur += write_str(cur, end - cur, "EBD Count: %d\n", inst->count.ebd);
	cur += write_str(cur, end - cur, "FTB Count: %d\n", inst->count.ftb);
	cur += write_str(cur, end - cur, "FBD Count: %d\n", inst->count.fbd);

	publish_unreleased_reference(inst, &cur, end);
	len = simple_read_from_buffer(buf, count, ppos,
		dbuf, cur - dbuf);

	kfree(dbuf);
failed_alloc:
	kref_put(&inst->kref, put_inst_helper);
	return len;
}

static int inst_info_release(struct inode *inode, struct file *file)
{
	dprintk(CVP_INFO, "Release inode ptr: %pK\n", inode->i_private);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations inst_info_fops = {
	.open = inst_info_open,
	.read = inst_info_read,
	.release = inst_info_release,
};

struct dentry *msm_cvp_debugfs_init_inst(struct msm_cvp_inst *inst,
		struct dentry *parent)
{
	struct dentry *dir = NULL, *info = NULL;
	char debugfs_name[MAX_DEBUGFS_NAME];
	struct core_inst_pair *idata = NULL;

	if (!inst) {
		dprintk(CVP_ERR, "Invalid params, inst: %pK\n", inst);
		goto exit;
	}
	snprintf(debugfs_name, MAX_DEBUGFS_NAME, "inst_%p", inst);

	idata = kzalloc(sizeof(struct core_inst_pair), GFP_KERNEL);
	if (!idata) {
		dprintk(CVP_ERR, "%s: Allocation failed!\n", __func__);
		goto exit;
	}

	idata->core = inst->core;
	idata->inst = inst;

	dir = debugfs_create_dir(debugfs_name, parent);
	if (!dir) {
		dprintk(CVP_ERR, "Failed to create debugfs for msm_cvp\n");
		goto failed_create_dir;
	}

	info = debugfs_create_file("info", 0444, dir,
			idata, &inst_info_fops);
	if (!info) {
		dprintk(CVP_ERR, "debugfs_create_file: fail\n");
		goto failed_create_file;
	}

	dir->d_inode->i_private = info->d_inode->i_private;
	inst->debug.pdata[FRAME_PROCESSING].sampling = true;
	return dir;

failed_create_file:
	debugfs_remove_recursive(dir);
	dir = NULL;
failed_create_dir:
	kfree(idata);
exit:
	return dir;
}

void msm_cvp_debugfs_deinit_inst(struct msm_cvp_inst *inst)
{
	struct dentry *dentry = NULL;

	if (!inst || !inst->debugfs_root)
		return;

	dentry = inst->debugfs_root;
	if (dentry->d_inode) {
		dprintk(CVP_INFO, "Destroy %pK\n", dentry->d_inode->i_private);
		kfree(dentry->d_inode->i_private);
		dentry->d_inode->i_private = NULL;
	}
	debugfs_remove_recursive(dentry);
	inst->debugfs_root = NULL;
}

void msm_cvp_debugfs_update(struct msm_cvp_inst *inst,
	enum msm_cvp_debugfs_event e)
{
	struct msm_cvp_debug *d = &inst->debug;
	char a[64] = "Frame processing";

	switch (e) {
	case MSM_CVP_DEBUGFS_EVENT_ETB:
		inst->count.etb++;
		if (inst->count.ebd && inst->count.ftb > inst->count.fbd) {
			d->pdata[FRAME_PROCESSING].name[0] = '\0';
			tic(inst, FRAME_PROCESSING, a);
		}
	break;
	case MSM_CVP_DEBUGFS_EVENT_EBD:
		inst->count.ebd++;
		if (inst->count.ebd && inst->count.ebd == inst->count.etb) {
			toc(inst, FRAME_PROCESSING);
			dprintk(CVP_PROF, "EBD: FW needs input buffers\n");
		}
		if (inst->count.ftb == inst->count.fbd)
			dprintk(CVP_PROF, "EBD: FW needs output buffers\n");
	break;
	case MSM_CVP_DEBUGFS_EVENT_FTB: {
		inst->count.ftb++;
		if (inst->count.ebd && inst->count.etb > inst->count.ebd) {
			d->pdata[FRAME_PROCESSING].name[0] = '\0';
			tic(inst, FRAME_PROCESSING, a);
		}
	}
	break;
	case MSM_CVP_DEBUGFS_EVENT_FBD:
		inst->count.fbd++;
		inst->debug.samples++;
		if (inst->count.fbd &&
			inst->count.fbd == inst->count.ftb) {
			toc(inst, FRAME_PROCESSING);
			dprintk(CVP_PROF, "FBD: FW needs output buffers\n");
		}
		if (inst->count.etb == inst->count.ebd)
			dprintk(CVP_PROF, "FBD: FW needs input buffers\n");
		break;
	default:
		dprintk(CVP_ERR, "Invalid state in debugfs: %d\n", e);
		break;
	}
}


// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 */

#include "msm_cvp.h"
#include "cvp_hfi.h"
#include <synx_api.h>
#include "cvp_core_hfi.h"

#define MSM_CVP_NOMINAL_CYCLES		(444 * 1000 * 1000)
#define MSM_CVP_UHD60E_VPSS_CYCLES	(111 * 1000 * 1000)
#define MSM_CVP_UHD60E_ISE_CYCLES	(175 * 1000 * 1000)
#define MAX_CVP_VPSS_CYCLES		(MSM_CVP_NOMINAL_CYCLES - \
		MSM_CVP_UHD60E_VPSS_CYCLES)
#define MAX_CVP_ISE_CYCLES		(MSM_CVP_NOMINAL_CYCLES - \
		MSM_CVP_UHD60E_ISE_CYCLES)

struct msm_cvp_fence_thread_data {
	struct msm_cvp_inst *inst;
	unsigned int device_id;
	struct cvp_kmd_hfi_fence_packet in_fence_pkt;
	unsigned int arg_type;
};

static struct msm_cvp_fence_thread_data fence_thread_data;

void print_cvp_internal_buffer(u32 tag, const char *str,
		struct msm_cvp_inst *inst, struct msm_cvp_internal_buffer *cbuf)
{
	if (!(tag & msm_cvp_debug) || !inst || !cbuf)
		return;

	dprintk(tag,
		"%s: %x : idx %2d fd %d off %d daddr %x size %d type %d flags 0x%x\n",
		str, hash32_ptr(inst->session), cbuf->buf.index, cbuf->buf.fd,
		cbuf->buf.offset, cbuf->smem.device_addr, cbuf->buf.size,
		cbuf->buf.type, cbuf->buf.flags);
}

static enum hal_buffer get_hal_buftype(const char *str, unsigned int type)
{
	enum hal_buffer buftype = HAL_BUFFER_NONE;

	if (type == CVP_KMD_BUFTYPE_INPUT)
		buftype = HAL_BUFFER_INPUT;
	else if (type == CVP_KMD_BUFTYPE_OUTPUT)
		buftype = HAL_BUFFER_OUTPUT;
	else if (type == CVP_KMD_BUFTYPE_INTERNAL_1)
		buftype = HAL_BUFFER_INTERNAL_SCRATCH_1;
	else if (type == CVP_KMD_BUFTYPE_INTERNAL_2)
		buftype = HAL_BUFFER_INTERNAL_SCRATCH_1;
	else
		dprintk(CVP_ERR, "%s: unknown buffer type %#x\n",
			str, type);

	return buftype;
}

static int msm_cvp_scale_clocks_and_bus(struct msm_cvp_inst *inst)
{
	int rc = 0;

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	rc = msm_cvp_set_clocks(inst->core);
	if (rc) {
		dprintk(CVP_ERR,
			"%s: failed set_clocks for inst %pK (%#x)\n",
			__func__, inst, hash32_ptr(inst->session));
		goto exit;
	}

	rc = msm_cvp_comm_vote_bus(inst->core);
	if (rc) {
		dprintk(CVP_ERR,
			"%s: failed vote_bus for inst %pK (%#x)\n",
			__func__, inst, hash32_ptr(inst->session));
		goto exit;
	}

exit:
	return rc;
}

static int msm_cvp_get_session_info(struct msm_cvp_inst *inst,
		struct cvp_kmd_session_info *session)
{
	int rc = 0;

	if (!inst || !inst->core || !session) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	session->session_id = hash32_ptr(inst->session);
	dprintk(CVP_DBG, "%s: id 0x%x\n", __func__, session->session_id);

	return rc;
}

static int msm_cvp_session_get_iova_addr(
	struct msm_cvp_inst *inst,
	struct msm_cvp_internal_buffer **cbuf_ptr,
	unsigned int search_fd, unsigned int search_size,
	unsigned int *iova,
	unsigned int *iova_size)
{
	bool found = false;
	struct msm_cvp_internal_buffer *cbuf;

	mutex_lock(&inst->cvpcpubufs.lock);
	list_for_each_entry(cbuf, &inst->cvpcpubufs.list, list) {
		if (cbuf->buf.fd == search_fd) {
			found = true;
			break;
		}
	}
	mutex_unlock(&inst->cvpcpubufs.lock);
	if (!found) {
		mutex_lock(&inst->cvpdspbufs.lock);
		list_for_each_entry(cbuf, &inst->cvpdspbufs.list, list) {
			if (cbuf->buf.fd == search_fd) {
				found = true;
				break;
			}
		}
		mutex_unlock(&inst->cvpdspbufs.lock);
	}
	if (!found)
		return -ENOENT;

	if (search_size != cbuf->buf.size) {
		dprintk(CVP_ERR,
			"%s: invalid size received fd = %d, size 0x%x 0x%x\n",
			__func__, search_fd, search_size, cbuf->buf.size);
		return -EINVAL;
	}
	*iova = cbuf->smem.device_addr;
	*iova_size = cbuf->buf.size;

	if (cbuf_ptr)
		*cbuf_ptr = cbuf;

	return 0;
}

static int msm_cvp_map_buf_dsp(struct msm_cvp_inst *inst,
	struct cvp_kmd_buffer *buf)
{
	int rc = 0;
	bool found;
	struct msm_cvp_internal_buffer *cbuf;
	struct cvp_hal_session *session;

	if (!inst || !inst->core || !buf) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	if (buf->offset) {
		dprintk(CVP_ERR,
			"%s: offset is deprecated, set to 0.\n",
			__func__);
		return -EINVAL;
	}

	session = (struct cvp_hal_session *)inst->session;
	mutex_lock(&inst->cvpdspbufs.lock);
	found = false;
	list_for_each_entry(cbuf, &inst->cvpdspbufs.list, list) {
		if (cbuf->buf.fd == buf->fd) {
			if (cbuf->buf.size != buf->size) {
				dprintk(CVP_ERR, "%s: buf size mismatch\n",
					__func__);
				mutex_unlock(&inst->cvpdspbufs.lock);
				return -EINVAL;
			}
			found = true;
			break;
		}
	}
	mutex_unlock(&inst->cvpdspbufs.lock);
	if (found) {
		print_client_buffer(CVP_ERR, "duplicate", inst, buf);
		return -EINVAL;
	}

	cbuf = kzalloc(sizeof(struct msm_cvp_internal_buffer), GFP_KERNEL);
	if (!cbuf) {
		dprintk(CVP_ERR, "%s: cbuf alloc failed\n", __func__);
		return -ENOMEM;
	}

	memcpy(&cbuf->buf, buf, sizeof(struct cvp_kmd_buffer));
	cbuf->smem.buffer_type = get_hal_buftype(__func__, buf->type);
	cbuf->smem.fd = buf->fd;
	cbuf->smem.offset = buf->offset;
	cbuf->smem.size = buf->size;
	cbuf->smem.flags = buf->flags;
	rc = msm_cvp_smem_map_dma_buf(inst, &cbuf->smem);
	if (rc) {
		print_client_buffer(CVP_ERR, "map failed", inst, buf);
		goto exit;
	}

	if (buf->index) {
		rc = cvp_dsp_register_buffer((uint32_t)cbuf->smem.device_addr,
			buf->index, buf->size, hash32_ptr(session));
		if (rc) {
			dprintk(CVP_ERR,
				"%s: failed dsp registration for fd=%d rc=%d",
				__func__, buf->fd, rc);
			goto exit;
		}
	} else {
		dprintk(CVP_ERR, "%s: buf index is 0 fd=%d",
				__func__, buf->fd);
		rc = -EINVAL;
		goto exit;
	}

	mutex_lock(&inst->cvpdspbufs.lock);
	list_add_tail(&cbuf->list, &inst->cvpdspbufs.list);
	mutex_unlock(&inst->cvpdspbufs.lock);

	return rc;

exit:
	if (cbuf->smem.device_addr)
		msm_cvp_smem_unmap_dma_buf(inst, &cbuf->smem);
	kfree(cbuf);
	cbuf = NULL;

	return rc;
}

static int msm_cvp_unmap_buf_dsp(struct msm_cvp_inst *inst,
	struct cvp_kmd_buffer *buf)
{
	int rc = 0;
	bool found;
	struct msm_cvp_internal_buffer *cbuf;
	struct cvp_hal_session *session;

	if (!inst || !inst->core || !buf) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	session = (struct cvp_hal_session *)inst->session;
	if (!session) {
		dprintk(CVP_ERR, "%s: invalid session\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&inst->cvpdspbufs.lock);
	found = false;
	list_for_each_entry(cbuf, &inst->cvpdspbufs.list, list) {
		if (cbuf->buf.fd == buf->fd) {
			found = true;
			break;
		}
	}
	mutex_unlock(&inst->cvpdspbufs.lock);
	if (!found) {
		print_client_buffer(CVP_ERR, "invalid", inst, buf);
		return -EINVAL;
	}

	if (buf->index) {
		rc = cvp_dsp_deregister_buffer((uint32_t)cbuf->smem.device_addr,
			buf->index, buf->size, hash32_ptr(session));
		if (rc) {
			dprintk(CVP_ERR,
				"%s: failed dsp deregistration fd=%d rc=%d",
				__func__, buf->fd, rc);
			return rc;
		}
	}

	if (cbuf->smem.device_addr)
		msm_cvp_smem_unmap_dma_buf(inst, &cbuf->smem);

	mutex_lock(&inst->cvpdspbufs.lock);
	list_del(&cbuf->list);
	mutex_unlock(&inst->cvpdspbufs.lock);

	kfree(cbuf);
	return rc;
}

static int msm_cvp_map_buf_cpu(struct msm_cvp_inst *inst,
	unsigned int fd,
	unsigned int size,
	struct msm_cvp_internal_buffer **cbuf_ptr)
{
	int rc = 0;
	bool found;
	struct msm_cvp_internal_buffer *cbuf;

	if (!inst || !inst->core || !cbuf_ptr) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&inst->cvpcpubufs.lock);
	found = false;
	list_for_each_entry(cbuf, &inst->cvpcpubufs.list, list) {
		if (cbuf->buf.fd == fd) {
			found = true;
			break;
		}
	}
	mutex_unlock(&inst->cvpcpubufs.lock);
	if (found) {
		print_client_buffer(CVP_ERR, "duplicate", inst, &cbuf->buf);
		return -EINVAL;
	}

	cbuf = kzalloc(sizeof(struct msm_cvp_internal_buffer), GFP_KERNEL);
	if (!cbuf)
		return -ENOMEM;

	memset(cbuf, 0, sizeof(struct msm_cvp_internal_buffer));

	cbuf->buf.fd = fd;
	cbuf->buf.size = size;
	/* HFI doesn't have buffer type, set it as HAL_BUFFER_INPUT */
	cbuf->smem.buffer_type = HAL_BUFFER_INPUT;
	cbuf->smem.fd = cbuf->buf.fd;
	cbuf->smem.size = cbuf->buf.size;
	cbuf->smem.flags = 0;
	cbuf->smem.offset = 0;
	rc = msm_cvp_smem_map_dma_buf(inst, &cbuf->smem);
	if (rc) {
		print_client_buffer(CVP_ERR, "map failed", inst, &cbuf->buf);
		goto exit;
	}

	mutex_lock(&inst->cvpcpubufs.lock);
	list_add_tail(&cbuf->list, &inst->cvpcpubufs.list);
	mutex_unlock(&inst->cvpcpubufs.lock);

	*cbuf_ptr = cbuf;

	return rc;

exit:
	if (cbuf->smem.device_addr)
		msm_cvp_smem_unmap_dma_buf(inst, &cbuf->smem);
	kfree(cbuf);
	cbuf = NULL;

	return rc;
}

static bool _cvp_msg_pending(struct msm_cvp_inst *inst,
			struct cvp_session_queue *sq,
			struct cvp_session_msg **msg)
{
	struct cvp_session_msg *mptr = NULL;
	bool result = false;

	spin_lock(&sq->lock);
	if (!kref_read(&inst->kref) ||
		sq->state != QUEUE_ACTIVE) {
		/* The session is being deleted */
		spin_unlock(&sq->lock);
		*msg = NULL;
		return true;
	}
	result = list_empty(&sq->msgs);
	if (!result) {
		mptr =
		list_first_entry(&sq->msgs, struct cvp_session_msg, node);
		list_del_init(&mptr->node);
		sq->msg_count--;
	}
	spin_unlock(&sq->lock);
	*msg = mptr;
	return !result;
}


static int msm_cvp_session_receive_hfi(struct msm_cvp_inst *inst,
			struct cvp_kmd_hfi_packet *out_pkt)
{
	unsigned long wait_time;
	struct cvp_session_msg *msg = NULL;
	struct cvp_session_queue *sq;
	struct cvp_kmd_session_control *sc;
	int rc;

	if (!inst) {
		dprintk(CVP_ERR, "%s invalid session\n", __func__);
		return -EINVAL;
	}

	sq = &inst->session_queue;
	sc = (struct cvp_kmd_session_control *)out_pkt;

	wait_time = msecs_to_jiffies(CVP_MAX_WAIT_TIME);

	if (wait_event_timeout(sq->wq,
		_cvp_msg_pending(inst, sq, &msg), wait_time) == 0) {
		dprintk(CVP_DBG, "session queue wait timeout\n");
		return -ETIMEDOUT;
	}

	if (msg == NULL) {
		dprintk(CVP_DBG,
			"%s: session deleted, queue state %d, msg cnt %d\n",
			__func__, inst->session_queue.state,
			inst->session_queue.msg_count);

		spin_lock(&sq->lock);
		if (sq->msg_count) {
			sc->ctrl_data[0] = sq->msg_count;
			rc = -EUCLEAN;
		} else {
			rc = -ENOLINK;
		}
		spin_unlock(&sq->lock);
		return rc;
	}

	memcpy(out_pkt, &msg->pkt, get_msg_size());
	kmem_cache_free(inst->session_queue.msg_cache, msg);

	return 0;
}

static int msm_cvp_map_buf(struct msm_cvp_inst *inst,
	struct cvp_kmd_hfi_packet *in_pkt,
	unsigned int offset, unsigned int buf_num)
{
	struct msm_cvp_internal_buffer *cbuf = NULL;
	struct cvp_buf_desc *buf_ptr;
	struct cvp_buf_type *new_buf;
	int i, rc = 0;
	struct cvp_hfi_device *hdev = inst->core->device;
	struct iris_hfi_device *hfi = hdev->hfi_device_data;
	u32 version = hfi->version;

	version = (version & HFI_VERSION_MINOR_MASK) >> HFI_VERSION_MINOR_SHIFT;

	if (offset != 0 && buf_num != 0) {
		for (i = 0; i < buf_num; i++) {
			buf_ptr = (struct cvp_buf_desc *)
					&in_pkt->pkt_data[offset];
			if (version >= 1)
				offset += sizeof(*new_buf) >> 2;
			else
				offset += sizeof(*buf_ptr) >> 2;

			if (!buf_ptr->fd)
				continue;

			rc = msm_cvp_session_get_iova_addr(inst, &cbuf,
						buf_ptr->fd,
						buf_ptr->size,
						&buf_ptr->fd,
						&buf_ptr->size);
			if (rc == -ENOENT) {
				dprintk(CVP_DBG, "%s map buf fd %d size %d\n",
					__func__, buf_ptr->fd,
					buf_ptr->size);
				rc = msm_cvp_map_buf_cpu(inst, buf_ptr->fd,
						buf_ptr->size, &cbuf);
				if (rc || !cbuf) {
					dprintk(CVP_ERR,
					"%s: buf %d register failed. rc=%d\n",
					__func__, i, rc);
					return rc;
				}
				buf_ptr->fd = cbuf->smem.device_addr;
				buf_ptr->size = cbuf->buf.size;
			} else if (rc) {
				dprintk(CVP_ERR,
				"%s: buf %d register failed. rc=%d\n",
				__func__, i, rc);
				return rc;
			}
			msm_cvp_smem_cache_operations(cbuf->smem.dma_buf,
						SMEM_CACHE_CLEAN_INVALIDATE,
						0, buf_ptr->size);
		}
	}
	return rc;
}

static int msm_cvp_session_process_hfi(
	struct msm_cvp_inst *inst,
	struct cvp_kmd_hfi_packet *in_pkt,
	unsigned int in_offset,
	unsigned int in_buf_num)
{
	int pkt_idx, rc = 0;
	struct cvp_hfi_device *hdev;
	unsigned int offset, buf_num, signal;
	struct cvp_session_queue *sq;

	if (!inst || !inst->core || !in_pkt) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	sq = &inst->session_queue;
	spin_lock(&sq->lock);
	if (sq->state != QUEUE_ACTIVE) {
		spin_unlock(&sq->lock);
		dprintk(CVP_ERR, "%s: invalid queue state\n", __func__);
		return -EINVAL;
	}
	spin_unlock(&sq->lock);

	hdev = inst->core->device;

	pkt_idx = get_pkt_index((struct cvp_hal_session_cmd_pkt *)in_pkt);
	if (pkt_idx < 0) {
		dprintk(CVP_ERR, "%s incorrect packet %d, %x\n", __func__,
				in_pkt->pkt_data[0],
				in_pkt->pkt_data[1]);
		offset = in_offset;
		buf_num = in_buf_num;
		signal = HAL_NO_RESP;
	} else {
		offset = cvp_hfi_defs[pkt_idx].buf_offset;
		buf_num = cvp_hfi_defs[pkt_idx].buf_num;
		signal = cvp_hfi_defs[pkt_idx].resp;
	}

	if (in_offset && in_buf_num) {
		offset = in_offset;
		buf_num = in_buf_num;
	}

	rc = msm_cvp_map_buf(inst, in_pkt, offset, buf_num);
	if (rc)
		return rc;

	rc = call_hfi_op(hdev, session_send,
			(void *)inst->session, in_pkt);
	if (rc) {
		dprintk(CVP_ERR,
			"%s: Failed in call_hfi_op %d, %x\n",
			__func__, in_pkt->pkt_data[0], in_pkt->pkt_data[1]);
	}

	if (signal != HAL_NO_RESP) {
		rc = wait_for_sess_signal_receipt(inst, signal);
		if (rc)
			dprintk(CVP_ERR,
				"%s: wait for signal failed, rc %d %d, %x %d\n",
				__func__, rc,
				in_pkt->pkt_data[0],
				in_pkt->pkt_data[1],
				signal);

	}

	return rc;
}

static int msm_cvp_thread_fence_run(void *data)
{
	int i, rc = 0;
	unsigned long timeout_ms = 1000;
	int synx_obj;
	struct cvp_hfi_device *hdev;
	struct msm_cvp_fence_thread_data *fence_thread_data;
	struct cvp_kmd_hfi_fence_packet *in_fence_pkt;
	struct cvp_kmd_hfi_packet *in_pkt;
	struct msm_cvp_inst *inst;
	int *fence;
	int ica_enabled = 0;
	int pkt_idx;

	if (!data) {
		dprintk(CVP_ERR, "%s Wrong input data %pK\n", __func__, data);
		do_exit(-EINVAL);
	}

	fence_thread_data = data;
	inst = cvp_get_inst(get_cvp_core(fence_thread_data->device_id),
				(void *)fence_thread_data->inst);
	if (!inst) {
		dprintk(CVP_ERR, "%s Wrong inst %pK\n", __func__, inst);
		do_exit(-EINVAL);
	}
	in_fence_pkt = (struct cvp_kmd_hfi_fence_packet *)
					&fence_thread_data->in_fence_pkt;
	in_pkt = (struct cvp_kmd_hfi_packet *)(in_fence_pkt);
	pkt_idx = get_pkt_index((struct cvp_hal_session_cmd_pkt *)in_pkt);

	if (pkt_idx < 0) {
		dprintk(CVP_ERR, "%s incorrect packet %d, %x\n", __func__,
			in_pkt->pkt_data[0],
			in_pkt->pkt_data[1]);
		rc = pkt_idx;
		goto exit;
	}

	fence = (int *)(in_fence_pkt->fence_data);
	hdev = inst->core->device;

	//wait on synx before signaling HFI
	switch (cvp_hfi_defs[pkt_idx].type) {
	case HFI_CMD_SESSION_CVP_DME_FRAME:
	{
		for (i = 0; i < HFI_DME_BUF_NUM-1; i++) {
			if (fence[(i<<1)]) {
				rc = synx_import(fence[(i<<1)],
					fence[((i<<1)+1)], &synx_obj);
				if (rc) {
					dprintk(CVP_ERR,
						"%s: synx_import failed\n",
						__func__);
					goto exit;
				}
				rc = synx_wait(synx_obj, timeout_ms);
				if (rc) {
					dprintk(CVP_ERR,
						"%s: synx_wait failed\n",
						__func__);
					goto exit;
				}
				rc = synx_release(synx_obj);
				if (rc) {
					dprintk(CVP_ERR,
						"%s: synx_release failed\n",
						__func__);
					goto exit;
				}
				if (i == 0) {
					ica_enabled = 1;
					/*
					 * Increase loop count to skip fence
					 * waiting on downscale image.
					 */
					i = i+1;
				}
			}
		}

		rc = call_hfi_op(hdev, session_send,
				(void *)inst->session, in_pkt);
		if (rc) {
			dprintk(CVP_ERR,
				"%s: Failed in call_hfi_op %d, %x\n",
				__func__, in_pkt->pkt_data[0],
				in_pkt->pkt_data[1]);
			goto exit;
		}

		rc = wait_for_sess_signal_receipt(inst,
				HAL_SESSION_DME_FRAME_CMD_DONE);
		if (rc)	{
			dprintk(CVP_ERR, "%s: wait for signal failed, rc %d\n",
			__func__, rc);
			goto exit;
		}

		if (ica_enabled) {
			rc = synx_import(fence[2], fence[3], &synx_obj);
			if (rc) {
				dprintk(CVP_ERR, "%s: synx_import failed\n",
					__func__);
				goto exit;
			}
			rc = synx_signal(synx_obj,
					SYNX_STATE_SIGNALED_SUCCESS);
			if (rc) {
				dprintk(CVP_ERR, "%s: synx_signal failed\n",
					__func__);
				goto exit;
			}
			if (synx_get_status(synx_obj) !=
				SYNX_STATE_SIGNALED_SUCCESS) {
				dprintk(CVP_ERR,
					"%s: synx_get_status failed\n",
					__func__);
				goto exit;
			}
			rc = synx_release(synx_obj);
			if (rc) {
				dprintk(CVP_ERR, "%s: synx_release failed\n",
					__func__);
				goto exit;
			}
		}

		rc = synx_import(fence[((HFI_DME_BUF_NUM-1)<<1)],
				fence[((HFI_DME_BUF_NUM-1)<<1)+1],
				&synx_obj);
		if (rc) {
			dprintk(CVP_ERR, "%s: synx_import failed\n", __func__);
			goto exit;
		}
		rc = synx_signal(synx_obj, SYNX_STATE_SIGNALED_SUCCESS);
		if (rc) {
			dprintk(CVP_ERR, "%s: synx_signal failed\n", __func__);
			goto exit;
		}
		if (synx_get_status(synx_obj) != SYNX_STATE_SIGNALED_SUCCESS) {
			dprintk(CVP_ERR, "%s: synx_get_status failed\n",
					__func__);
			goto exit;
		}
		rc = synx_release(synx_obj);
		if (rc) {
			dprintk(CVP_ERR, "%s: synx_release failed\n",
				__func__);
			goto exit;
		}
		break;
	}
	case HFI_CMD_SESSION_CVP_ICA_FRAME:
	{
		for (i = 0; i < cvp_hfi_defs[pkt_idx].buf_num-1; i++) {
			if (fence[(i<<1)]) {
				rc = synx_import(fence[(i<<1)],
					fence[((i<<1)+1)], &synx_obj);
				if (rc) {
					dprintk(CVP_ERR,
						"%s: synx_import failed\n",
						__func__);
					goto exit;
				}
				rc = synx_wait(synx_obj, timeout_ms);
				if (rc) {
					dprintk(CVP_ERR,
						"%s: synx_wait failed\n",
						__func__);
					goto exit;
				}
				rc = synx_release(synx_obj);
				if (rc) {
					dprintk(CVP_ERR,
						"%s: synx_release failed\n",
						__func__);
					goto exit;
				}
				if (i == 0) {
					/*
					 * Increase loop count to skip fence
					 * waiting on output corrected image.
					 */
					i = i+1;
				}
			}
		}

		rc = call_hfi_op(hdev, session_send,
				(void *)inst->session, in_pkt);
		if (rc) {
			dprintk(CVP_ERR,
				"%s: Failed in call_hfi_op %d, %x\n",
				__func__, in_pkt->pkt_data[0],
				in_pkt->pkt_data[1]);
			goto exit;
		}

		rc = wait_for_sess_signal_receipt(inst,
				HAL_SESSION_ICA_FRAME_CMD_DONE);
		if (rc)	{
			dprintk(CVP_ERR, "%s: wait for signal failed, rc %d\n",
			__func__, rc);
			goto exit;
		}

		rc = synx_import(fence[2], fence[3], &synx_obj);
		if (rc) {
			dprintk(CVP_ERR, "%s: synx_import failed\n", __func__);
			goto exit;
		}
		rc = synx_signal(synx_obj, SYNX_STATE_SIGNALED_SUCCESS);
		if (rc) {
			dprintk(CVP_ERR, "%s: synx_signal failed\n", __func__);
			goto exit;
		}
		if (synx_get_status(synx_obj) != SYNX_STATE_SIGNALED_SUCCESS) {
			dprintk(CVP_ERR, "%s: synx_get_status failed\n",
					__func__);
			goto exit;
		}
		rc = synx_release(synx_obj);
		if (rc) {
			dprintk(CVP_ERR, "%s: synx_release failed\n", __func__);
			goto exit;
		}
		break;
	}
	default:
		dprintk(CVP_ERR, "%s: unknown hfi cmd type 0x%x\n",
			__func__, fence_thread_data->arg_type);
		rc = -EINVAL;
		goto exit;
		break;
	}

exit:
	cvp_put_inst(inst);
	do_exit(rc);
}

static int msm_cvp_session_process_hfi_fence(
	struct msm_cvp_inst *inst,
	struct cvp_kmd_arg *arg)
{
	static int thread_num;
	struct task_struct *thread;
	int rc = 0;
	char thread_fence_name[32];
	int pkt_idx;
	struct cvp_kmd_hfi_packet *in_pkt;
	unsigned int signal, offset, buf_num, in_offset, in_buf_num;

	dprintk(CVP_DBG, "%s: Enter inst = %d", __func__, inst);

	if (!inst || !inst->core || !arg) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	in_offset = arg->buf_offset;
	in_buf_num = arg->buf_num;
	in_pkt = (struct cvp_kmd_hfi_packet *)&arg->data.hfi_pkt;
	pkt_idx = get_pkt_index((struct cvp_hal_session_cmd_pkt *)in_pkt);
	if (pkt_idx < 0) {
		dprintk(CVP_ERR, "%s incorrect packet %d, %x\n", __func__,
				in_pkt->pkt_data[0],
				in_pkt->pkt_data[1]);
		offset = in_offset;
		buf_num = in_buf_num;
		signal = HAL_NO_RESP;
	} else {
		offset = cvp_hfi_defs[pkt_idx].buf_offset;
		buf_num = cvp_hfi_defs[pkt_idx].buf_num;
		signal = cvp_hfi_defs[pkt_idx].resp;
	}

	if (in_offset && in_buf_num) {
		offset = in_offset;
		buf_num = in_buf_num;
	}

	rc = msm_cvp_map_buf(inst, in_pkt, offset, buf_num);
	if (rc)
		return rc;

	thread_num = thread_num + 1;
	fence_thread_data.inst = inst;
	fence_thread_data.device_id = (unsigned int)inst->core->id;
	memcpy(&fence_thread_data.in_fence_pkt, &arg->data.hfi_fence_pkt,
				sizeof(struct cvp_kmd_hfi_fence_packet));
	fence_thread_data.arg_type = arg->type;
	snprintf(thread_fence_name, sizeof(thread_fence_name),
				"thread_fence_%d", thread_num);
	thread = kthread_run(msm_cvp_thread_fence_run,
			&fence_thread_data, thread_fence_name);

	return rc;
}

static int msm_cvp_session_cvp_dfs_frame_response(
	struct msm_cvp_inst *inst,
	struct cvp_kmd_hfi_packet *dfs_frame)
{
	int rc = 0;

	dprintk(CVP_DBG, "%s: Enter inst = %pK\n", __func__, inst);

	if (!inst || !inst->core || !dfs_frame) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}
	rc = wait_for_sess_signal_receipt(inst,
			HAL_SESSION_DFS_FRAME_CMD_DONE);
	if (rc)
		dprintk(CVP_ERR,
			"%s: wait for signal failed, rc %d\n",
			__func__, rc);
	return rc;
}

static int msm_cvp_session_cvp_dme_frame_response(
	struct msm_cvp_inst *inst,
	struct cvp_kmd_hfi_packet *dme_frame)
{
	int rc = 0;

	dprintk(CVP_DBG, "%s: Enter inst = %d", __func__, inst);

	if (!inst || !inst->core || !dme_frame) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}
	rc = wait_for_sess_signal_receipt(inst,
			HAL_SESSION_DME_FRAME_CMD_DONE);
	if (rc)
		dprintk(CVP_ERR,
			"%s: wait for signal failed, rc %d\n",
			__func__, rc);
	return rc;
}

static int msm_cvp_session_cvp_persist_response(
	struct msm_cvp_inst *inst,
	struct cvp_kmd_hfi_packet *pbuf_cmd)
{
	int rc = 0;

	dprintk(CVP_DBG, "%s: Enter inst = %d", __func__, inst);

	if (!inst || !inst->core || !pbuf_cmd) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}
	rc = wait_for_sess_signal_receipt(inst,
			HAL_SESSION_PERSIST_CMD_DONE);
	if (rc)
		dprintk(CVP_ERR,
			"%s: wait for signal failed, rc %d\n",
			__func__, rc);
	return rc;
}



static int msm_cvp_send_cmd(struct msm_cvp_inst *inst,
		struct cvp_kmd_send_cmd *send_cmd)
{
	dprintk(CVP_ERR, "%s: UMD gave a deprecated cmd", __func__);

	return 0;
}

#define C2C_FREQ_RATIO	(1.5)

static void adjust_bw_freqs(struct allowed_clock_rates_table *tbl,
		unsigned int tbl_size, unsigned int max_bw,
		unsigned int *freq, unsigned long *ab, unsigned long *ib)
{
	struct msm_cvp_core *core;
	struct msm_cvp_inst *inst;
	unsigned long clk_core_sum = 0, clk_ctlr_sum = 0, bw_sum = 0;
	int i;

	core = list_first_entry(&cvp_driver->cores, struct msm_cvp_core, list);

	list_for_each_entry(inst, &core->instances, list) {
		clk_core_sum += inst->power.clock_cycles_a;
		clk_ctlr_sum += inst->power.clock_cycles_b;
		bw_sum += inst->power.ddr_bw;
	}

	if (clk_core_sum * C2C_FREQ_RATIO < clk_ctlr_sum)
		clk_core_sum = clk_ctlr_sum/C2C_FREQ_RATIO;

	for (i = 1; i < tbl_size; i++) {
		if (clk_core_sum < tbl[i].clock_rate)
			break;
	}

	if (i == tbl_size)
		clk_core_sum = tbl[tbl_size - 1].clock_rate;
	else
		clk_core_sum = tbl[i].clock_rate;

	*freq = clk_core_sum;

	if (bw_sum > max_bw)
		bw_sum = max_bw;

	*ab = bw_sum;
	*ib = 0;
}

/**
 * clock_cycles_a: CVP core clock freq (lower)
 * clock_cycles_b: CVP controller clock freq
 */
static int msm_cvp_request_power(struct msm_cvp_inst *inst,
		struct cvp_kmd_request_power *power)
{
	int rc = 0;
	unsigned int freq;
	unsigned long ab, ib;
	struct msm_cvp_core *core;
	struct bus_info *bus;
	struct allowed_clock_rates_table *clks_tbl = NULL;
	unsigned int clks_tbl_size;
	unsigned int min_rate, max_rate;


	if (!inst || !power) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	core = list_first_entry(&cvp_driver->cores, struct msm_cvp_core, list);

	mutex_lock(&core->lock);

	clks_tbl = core->resources.allowed_clks_tbl;
	clks_tbl_size = core->resources.allowed_clks_tbl_size;
	min_rate = clks_tbl[0].clock_rate;
	max_rate = clks_tbl[clks_tbl_size - 1].clock_rate;
	bus = &core->resources.bus_set.bus_tbl[1];

	memcpy(&inst->power, power, sizeof(*power));

	if (inst->power.clock_cycles_a < min_rate ||
			inst->power.clock_cycles_a > max_rate)
		inst->power.clock_cycles_a = min_rate;

	if (inst->power.clock_cycles_b < (min_rate * C2C_FREQ_RATIO) ||
		inst->power.clock_cycles_b > (max_rate * C2C_FREQ_RATIO))
		inst->power.clock_cycles_b = min_rate * C2C_FREQ_RATIO;

	/* Convert bps to KBps */
	inst->power.ddr_bw = inst->power.ddr_bw >> 10;

	if (inst->power.ddr_bw > bus->range[1])
		inst->power.ddr_bw = bus->range[1] >> 1;

	dprintk(CVP_DBG,
		"%s: cycles_a %d, cycles_b %d, ddr_bw %d sys_cache_bw %d\n",
		__func__, power->clock_cycles_a, power->clock_cycles_b,
		power->ddr_bw, power->sys_cache_bw);

	adjust_bw_freqs(clks_tbl, clks_tbl_size, bus->range[1],
			&freq, &ab, &ib);
	dprintk(CVP_DBG, "%s %x %llx %llx\n", __func__, freq, ab, ib);
	mutex_unlock(&core->lock);

	return rc;
}

static int msm_cvp_register_buffer(struct msm_cvp_inst *inst,
		struct cvp_kmd_buffer *buf)
{
	struct cvp_hfi_device *hdev;
	struct cvp_hal_session *session;

	if (!inst || !inst->core || !buf) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	session = (struct cvp_hal_session *)inst->session;
	if (!session) {
		dprintk(CVP_ERR, "%s: invalid session\n", __func__);
		return -EINVAL;
	}
	hdev = inst->core->device;
	print_client_buffer(CVP_DBG, "register", inst, buf);

	if (!buf->index)
		return 0;

	return msm_cvp_map_buf_dsp(inst, buf);
}

static int msm_cvp_unregister_buffer(struct msm_cvp_inst *inst,
		struct cvp_kmd_buffer *buf)
{
	if (!inst || !inst->core || !buf) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	print_client_buffer(CVP_DBG, "unregister", inst, buf);

	if (!buf->index) {
		dprintk(CVP_INFO,
			"%s CPU path unregister buffer is deprecated!\n",
			__func__);
		return 0;
	}
	return msm_cvp_unmap_buf_dsp(inst, buf);
}

static int msm_cvp_session_start(struct msm_cvp_inst *inst,
		struct cvp_kmd_arg *arg)
{
	struct cvp_session_queue *sq;

	sq = &inst->session_queue;
	spin_lock(&sq->lock);
	if (sq->msg_count) {
		dprintk(CVP_ERR, "session start failed queue not empty%d\n",
			sq->msg_count);
		spin_unlock(&sq->lock);
		return -EINVAL;
	}
	sq->state = QUEUE_ACTIVE;
	spin_unlock(&sq->lock);
	return 0;
}

static int msm_cvp_session_stop(struct msm_cvp_inst *inst,
		struct cvp_kmd_arg *arg)
{
	struct cvp_session_queue *sq;
	struct cvp_kmd_session_control *sc = &arg->data.session_ctrl;

	sq = &inst->session_queue;

	spin_lock(&sq->lock);
	if (sq->msg_count) {
		dprintk(CVP_ERR, "session stop incorrect: queue not empty%d\n",
			sq->msg_count);
		sc->ctrl_data[0] = sq->msg_count;
		spin_unlock(&sq->lock);
		return -EUCLEAN;
	}
	sq->state = QUEUE_STOP;

	spin_unlock(&sq->lock);

	wake_up_all(&inst->session_queue.wq);

	return 0;
}

static int msm_cvp_session_ctrl(struct msm_cvp_inst *inst,
		struct cvp_kmd_arg *arg)
{
	struct cvp_kmd_session_control *ctrl = &arg->data.session_ctrl;
	int rc = 0;



	if (!inst) {
		dprintk(CVP_ERR, "%s invalid session\n", __func__);
		return -EINVAL;
	}


	switch (ctrl->ctrl_type) {
	case SESSION_STOP:
		rc = msm_cvp_session_stop(inst, arg);
		break;
	case SESSION_START:
		rc = msm_cvp_session_start(inst, arg);
		break;
	case SESSION_CREATE:
	case SESSION_DELETE:
	case SESSION_INFO:
	default:
		dprintk(CVP_ERR, "%s Unsupported session ctrl%d\n",
			__func__, ctrl->ctrl_type);
		rc = -EINVAL;
	}
	return rc;
}

static int msm_cvp_get_sysprop(struct msm_cvp_inst *inst,
		struct cvp_kmd_arg *arg)
{
	struct cvp_kmd_sys_properties *props = &arg->data.sys_properties;
	struct cvp_hfi_device *hdev;
	struct iris_hfi_device *hfi;
	int rc = 0;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	hdev = inst->core->device;
	hfi = hdev->hfi_device_data;

	switch (props->prop_data.prop_type) {
	case CVP_HFI_VERSION:
	{
		props->prop_data.data = hfi->version;
		break;
	}
	default:
		dprintk(CVP_ERR, "unrecognized sys property %d\n",
			props->prop_data.prop_type);
		rc = -EFAULT;
	}
	return rc;
}

int msm_cvp_handle_syscall(struct msm_cvp_inst *inst, struct cvp_kmd_arg *arg)
{
	int rc = 0;

	if (!inst || !arg) {
		dprintk(CVP_ERR, "%s: invalid args\n", __func__);
		return -EINVAL;
	}
	dprintk(CVP_DBG, "%s: arg->type = %x", __func__, arg->type);

	switch (arg->type) {
	case CVP_KMD_GET_SESSION_INFO:
	{
		struct cvp_kmd_session_info *session =
			(struct cvp_kmd_session_info *)&arg->data.session;

		rc = msm_cvp_get_session_info(inst, session);
		break;
	}
	case CVP_KMD_REQUEST_POWER:
	{
		struct cvp_kmd_request_power *power =
			(struct cvp_kmd_request_power *)&arg->data.req_power;

		rc = msm_cvp_request_power(inst, power);
		break;
	}
	case CVP_KMD_REGISTER_BUFFER:
	{
		struct cvp_kmd_buffer *buf =
			(struct cvp_kmd_buffer *)&arg->data.regbuf;

		rc = msm_cvp_register_buffer(inst, buf);
		break;
	}
	case CVP_KMD_UNREGISTER_BUFFER:
	{
		struct cvp_kmd_buffer *buf =
			(struct cvp_kmd_buffer *)&arg->data.unregbuf;

		rc = msm_cvp_unregister_buffer(inst, buf);
		break;
	}
	case CVP_KMD_HFI_SEND_CMD:
	{
		struct cvp_kmd_send_cmd *send_cmd =
			(struct cvp_kmd_send_cmd *)&arg->data.send_cmd;

		rc = msm_cvp_send_cmd(inst, send_cmd);
		break;
	}
	case CVP_KMD_RECEIVE_MSG_PKT:
	{
		struct cvp_kmd_hfi_packet *out_pkt =
			(struct cvp_kmd_hfi_packet *)&arg->data.hfi_pkt;
		rc = msm_cvp_session_receive_hfi(inst, out_pkt);
		break;
	}
	case CVP_KMD_SEND_CMD_PKT:
	case CVP_KMD_HFI_DFS_CONFIG_CMD:
	case CVP_KMD_HFI_DFS_FRAME_CMD:
	case CVP_KMD_HFI_DME_CONFIG_CMD:
	case CVP_KMD_HFI_DME_FRAME_CMD:
	case CVP_KMD_HFI_PERSIST_CMD:
	{
		struct cvp_kmd_hfi_packet *in_pkt =
			(struct cvp_kmd_hfi_packet *)&arg->data.hfi_pkt;

		rc = msm_cvp_session_process_hfi(inst, in_pkt,
				arg->buf_offset, arg->buf_num);
		break;
	}
	case CVP_KMD_HFI_DFS_FRAME_CMD_RESPONSE:
	{
		struct cvp_kmd_hfi_packet *dfs_frame =
			(struct cvp_kmd_hfi_packet *)&arg->data.hfi_pkt;

		rc = msm_cvp_session_cvp_dfs_frame_response(inst, dfs_frame);
		break;
	}
	case CVP_KMD_HFI_DME_FRAME_CMD_RESPONSE:
	{
		struct cvp_kmd_hfi_packet *dme_frame =
			(struct cvp_kmd_hfi_packet *)&arg->data.hfi_pkt;

		rc = msm_cvp_session_cvp_dme_frame_response(inst, dme_frame);
		break;
	}
	case CVP_KMD_HFI_PERSIST_CMD_RESPONSE:
	{
		struct cvp_kmd_hfi_packet *pbuf_cmd =
			(struct cvp_kmd_hfi_packet *)&arg->data.hfi_pkt;

		rc = msm_cvp_session_cvp_persist_response(inst, pbuf_cmd);
		break;
	}
	case CVP_KMD_HFI_DME_FRAME_FENCE_CMD:
	case CVP_KMD_SEND_FENCE_CMD_PKT:
	{
		rc = msm_cvp_session_process_hfi_fence(inst, arg);
		break;
	}
	case CVP_KMD_SESSION_CONTROL:
		rc = msm_cvp_session_ctrl(inst, arg);
		break;
	case CVP_KMD_GET_SYS_PROPERTY:
		rc = msm_cvp_get_sysprop(inst, arg);
		break;
	default:
		dprintk(CVP_DBG, "%s: unknown arg type %#x\n",
				__func__, arg->type);
		rc = -ENOTSUPP;
		break;
	}

	return rc;
}

int msm_cvp_session_deinit(struct msm_cvp_inst *inst)
{
	int rc = 0;

	if (!inst || !inst->core) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}
	dprintk(CVP_DBG, "%s: inst %pK (%#x)\n", __func__,
		inst, hash32_ptr(inst->session));

	rc = msm_cvp_comm_try_state(inst, MSM_CVP_CLOSE_DONE);
	if (rc)
		dprintk(CVP_ERR, "%s: close failed\n", __func__);

	inst->clk_data.min_freq = 0;
	inst->clk_data.ddr_bw = 0;
	inst->clk_data.sys_cache_bw = 0;
	rc = msm_cvp_scale_clocks_and_bus(inst);
	if (rc)
		dprintk(CVP_ERR, "%s: failed to scale_clocks_and_bus\n",
			__func__);

	return rc;
}

int msm_cvp_session_init(struct msm_cvp_inst *inst)
{
	int rc = 0;

	if (!inst) {
		dprintk(CVP_ERR, "%s: invalid params\n", __func__);
		return -EINVAL;
	}

	dprintk(CVP_DBG, "%s: inst %pK (%#x)\n", __func__,
		inst, hash32_ptr(inst->session));

	/* set default frequency */
	inst->clk_data.core_id = CVP_CORE_ID_2;
	inst->clk_data.min_freq = 1000;
	inst->clk_data.ddr_bw = 1000;
	inst->clk_data.sys_cache_bw = 1000;

	return rc;
}

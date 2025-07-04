// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2025, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/videodev2.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ratelimit.h>

#include "cam_mem_mgr.h"
#include "cam_sync_api.h"
#include "cam_req_mgr_dev.h"
#include "cam_trace.h"
#include "cam_debug_util.h"
#include "cam_packet_util.h"
#include "cam_context_utils.h"
#include "cam_cdm_util.h"
#include "cam_isp_context.h"
#include "cam_common_util.h"
#include "cam_req_mgr_debug.h"
#include "cam_cpas_api.h"
#include "cam_ife_hw_mgr.h"
#include "cam_subdev.h"

static const char isp_dev_name[] = "cam-isp";

static struct cam_isp_ctx_debug isp_ctx_debug;

#define INC_HEAD(head, max_entries, ret) \
	div_u64_rem(atomic64_add_return(1, head),\
	max_entries, (ret))

static int cam_isp_context_dump_requests(void *data,
	void *pf_args);

static int cam_isp_context_hw_recovery(void *priv, void *data);
static int cam_isp_context_handle_message(void *context,
	uint32_t msg_type, void *data);

static int __cam_isp_ctx_start_dev_in_ready(struct cam_context *ctx,
	struct cam_start_stop_dev_cmd *cmd);

static void __cam_isp_ctx_dump_state_monitor_array(
	struct cam_isp_context *ctx_isp);

static const char *__cam_isp_hw_evt_val_to_type(
	uint32_t evt_id);

static const char *__cam_isp_ctx_substate_val_to_type(
	enum cam_isp_ctx_activated_substate type);

static int __cam_isp_ctx_check_deferred_buf_done(
	struct cam_isp_context *ctx_isp,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state);

static const char *__cam_isp_evt_val_to_type(
	uint32_t evt_id)
{
	switch (evt_id) {
	case CAM_ISP_CTX_EVENT_SUBMIT:
		return "SUBMIT";
	case CAM_ISP_CTX_EVENT_APPLY:
		return "APPLY";
	case CAM_ISP_CTX_EVENT_EPOCH:
		return "EPOCH";
	case CAM_ISP_CTX_EVENT_RUP:
		return "RUP";
	case CAM_ISP_CTX_EVENT_BUFDONE:
		return "BUFDONE";
	case CAM_ISP_CTX_EVENT_SHUTTER:
		return "SHUTTER_NOTIFICATION";
	default:
		return "CAM_ISP_EVENT_INVALID";
	}
}

static void __cam_isp_ctx_update_event_record(
	struct cam_isp_context *ctx_isp,
	enum cam_isp_ctx_event  event,
	struct cam_ctx_request *req,
	void *event_data)
{
	int                      iterator = 0;
	ktime_t                  cur_time;
	struct cam_isp_ctx_req  *req_isp;

	if (!ctx_isp) {
		CAM_ERR(CAM_ISP, "Invalid Args");
		return;
	}
	switch (event) {
	case CAM_ISP_CTX_EVENT_EPOCH:
	case CAM_ISP_CTX_EVENT_RUP:
	case CAM_ISP_CTX_EVENT_BUFDONE:
	case CAM_ISP_CTX_EVENT_SHUTTER:
		break;
	case CAM_ISP_CTX_EVENT_SUBMIT:
	case CAM_ISP_CTX_EVENT_APPLY:
		if (!req) {
			CAM_ERR(CAM_ISP, "Invalid arg for event %d", event);
			return;
		}
		break;
	default:
		break;
	}

	INC_HEAD(&ctx_isp->dbg_monitors.event_record_head[event],
		CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES, &iterator);
	cur_time = ktime_get();
	if (req) {
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		ctx_isp->dbg_monitors.event_record[event][iterator].req_id =
			req->request_id;
		req_isp->event_timestamp[event] = cur_time;
	} else {
		ctx_isp->dbg_monitors.event_record[event][iterator].req_id = 0;
	}
	ctx_isp->dbg_monitors.event_record[event][iterator].timestamp  = cur_time;

	if (event_data == NULL)
		return;
	/* Update event specific data */
	ctx_isp->dbg_monitors.event_record[event][iterator].event_type = event;
	if (event == CAM_ISP_CTX_EVENT_SHUTTER) {
		ctx_isp->dbg_monitors.event_record[event][iterator].req_id =
			((struct shutter_event *)event_data)->req_id;
		ctx_isp->dbg_monitors.event_record[event][iterator].event.shutter_event.req_id =
			((struct shutter_event *)event_data)->req_id;
		ctx_isp->dbg_monitors.event_record[event][iterator].event.shutter_event.status =
			((struct shutter_event *)event_data)->status;
		ctx_isp->dbg_monitors.event_record[event][iterator].event.shutter_event.frame_id =
			((struct shutter_event *)event_data)->frame_id;
		ctx_isp->dbg_monitors.event_record[event][iterator].event.shutter_event.boot_ts =
			((struct shutter_event *)event_data)->boot_ts;
		ctx_isp->dbg_monitors.event_record[event][iterator].event.shutter_event.sof_ts =
			((struct shutter_event *)event_data)->sof_ts;
	}
}

static int __cam_isp_ctx_handle_sof_freeze_evt(
	struct cam_context *ctx)
{
	int rc = 0;
	struct cam_isp_context      *ctx_isp;
	struct cam_hw_cmd_args       hw_cmd_args;
	struct cam_isp_hw_cmd_args   isp_hw_cmd_args;

	ctx_isp = (struct cam_isp_context *)ctx->ctx_priv;
	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_SOF_DEBUG;
	isp_hw_cmd_args.u.sof_irq_enable = 1;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;

	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);

	ctx_isp->sof_dbg_irq_en = true;
	return rc;
}

static void *cam_isp_ctx_user_dump_events(
	void *dump_struct, uint8_t *addr_ptr)
{
	uint64_t                             *addr;
	struct cam_isp_context_event_record  *record;
	struct timespec64                     ts;

	record = (struct cam_isp_context_event_record *)dump_struct;

	addr = (uint64_t *)addr_ptr;
	ts = ktime_to_timespec64(record->timestamp);
	*addr++ = record->req_id;
	*addr++ = ts.tv_sec;
	*addr++ = ts.tv_nsec / NSEC_PER_USEC;

	return addr;
}

static int __cam_isp_ctx_print_event_record(struct cam_isp_context *ctx_isp)
{
	int                                  i, j, rc = 0;
	int                                  index;
	uint32_t                             oldest_entry, num_entries;
	uint64_t                             state_head;
	struct cam_isp_context_event_record *record;
	uint32_t                             len;
	uint8_t                              buf[CAM_ISP_CONTEXT_DBG_BUF_LEN];
	struct timespec64                    ts;
	struct cam_context                  *ctx = ctx_isp->base;

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++) {
		state_head = atomic64_read(&ctx_isp->dbg_monitors.event_record_head[i]);

		if (state_head == -1) {
			return 0;
		} else if (state_head < CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES) {
			num_entries = state_head + 1;
			oldest_entry = 0;
		} else {
			num_entries = CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES;
			div_u64_rem(state_head + 1,
				CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES,
				&oldest_entry);
		}

		index = oldest_entry;
		len = 0;
		memset(buf, 0, CAM_ISP_CONTEXT_DBG_BUF_LEN);
		for (j = 0; j < num_entries; j++) {
			record = &ctx_isp->dbg_monitors.event_record[i][index];
			ts = ktime_to_timespec64(record->timestamp);
			if (len >= CAM_ISP_CONTEXT_DBG_BUF_LEN) {
				CAM_WARN(CAM_ISP, "Overshooting buffer length %llu", len);
				break;
			}
			if (record->event_type != CAM_ISP_CTX_EVENT_SHUTTER)
				len += scnprintf(buf + len, CAM_ISP_CONTEXT_DBG_BUF_LEN - len,
					"%llu[%lld:%06lld] ", record->req_id, ts.tv_sec,
					ts.tv_nsec / NSEC_PER_USEC);
			else
				/*
				 * Output format
				 * req Id[timestamp] status frmId softs bootts
				 */
				len += scnprintf(buf + len, (CAM_ISP_CONTEXT_DBG_BUF_LEN) - len,
					"%llu[%lld:%06lld] [%d %lld %llu %llu] | ",
					record->req_id, ts.tv_sec,
					ts.tv_nsec / NSEC_PER_USEC,
					record->event.shutter_event.status,
					record->event.shutter_event.frame_id,
					record->event.shutter_event.sof_ts,
					record->event.shutter_event.boot_ts);
			index = (index + 1) %
				CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES;
		}
		if (len)
			CAM_INFO(CAM_ISP, "Ctx:%d %s: %s",
		ctx->ctx_id, __cam_isp_evt_val_to_type(i), buf);
	}
	return rc;
}

static int __cam_isp_ctx_dump_event_record(
	struct cam_isp_context *ctx_isp,
	struct cam_common_hw_dump_args *dump_args)
{
	int                                  i, j, rc = 0;
	int                                  index;
	size_t                               remain_len;
	uint32_t                             oldest_entry, num_entries;
	uint32_t                             min_len;
	uint64_t                             state_head;
	struct cam_isp_context_event_record *record;

	if (!dump_args || !ctx_isp) {
		CAM_ERR(CAM_ISP, "Invalid args %pK %pK",
			dump_args, ctx_isp);
		return -EINVAL;
	}

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++) {
		state_head = atomic64_read(&ctx_isp->dbg_monitors.event_record_head[i]);

		if (state_head == -1)
			return 0;
		else if (state_head < CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES) {
			num_entries = state_head + 1;
			oldest_entry = 0;
		} else {
			num_entries = CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES;
			div_u64_rem(state_head + 1, CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES,
				&oldest_entry);
		}

		index = oldest_entry;

		if (dump_args->buf_len <= dump_args->offset) {
			CAM_WARN(CAM_ISP, "Dump buffer overshoot len %zu offset %zu",
				dump_args->buf_len, dump_args->offset);
			return -ENOSPC;
		}

		min_len = (sizeof(struct cam_isp_context_dump_header) +
			(CAM_ISP_CTX_DUMP_EVENT_NUM_WORDS * sizeof(uint64_t))) * num_entries;
		remain_len = dump_args->buf_len - dump_args->offset;

		if (remain_len < min_len) {
			CAM_WARN(CAM_ISP,
				"Dump buffer exhaust remain %zu min %u",
				remain_len, min_len);
			return -ENOSPC;
		}
		for (j = 0; j < num_entries; j++) {
			record = &ctx_isp->dbg_monitors.event_record[i][index];

			rc = cam_common_user_dump_helper(dump_args, cam_isp_ctx_user_dump_events,
				record, sizeof(uint64_t), "ISP_EVT_%s:",
				__cam_isp_evt_val_to_type(i));
			if (rc) {
				CAM_ERR(CAM_ISP,
					"CAM_ISP_CONTEXT DUMP_EVENT_RECORD: Dump failed, rc: %d",
					rc);
				return rc;
			}

			index = (index + 1) % CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES;
		}
	}
	return rc;
}

static void __cam_isp_ctx_req_mini_dump(struct cam_ctx_request *req,
	uint8_t *start_addr, uint8_t *end_addr,
	unsigned long *bytes_updated)
{
	struct cam_isp_ctx_req_mini_dump *req_md;
	struct cam_buf_io_cfg            *io_cfg;
	struct cam_isp_ctx_req           *req_isp;
	struct cam_packet                *packet = NULL;
	unsigned long                     bytes_required = 0;

	bytes_required = sizeof(*req_md);
	*bytes_updated = 0;
	if (start_addr + bytes_required > end_addr)
		return;

	req_md = (struct cam_isp_ctx_req_mini_dump *)start_addr;
	req_isp = (struct cam_isp_ctx_req *)req->req_priv;
	req_md->num_acked = req_isp->num_acked;
	req_md->num_deferred_acks = req_isp->num_deferred_acks;
	req_md->bubble_report = req_isp->bubble_report;
	req_md->bubble_detected = req_isp->bubble_detected;
	req_md->reapply_type = req_isp->reapply_type;
	req_md->request_id = req->request_id;
	*bytes_updated += bytes_required;

	if (req_isp->num_fence_map_out) {
		bytes_required = sizeof(struct cam_hw_fence_map_entry) *
			req_isp->num_fence_map_out;
		if (start_addr + *bytes_updated + bytes_required > end_addr)
			return;

		req_md->map_out = (struct cam_hw_fence_map_entry *)
				((uint8_t *)start_addr + *bytes_updated);
		memcpy(req_md->map_out, req_isp->fence_map_out, bytes_required);
		req_md->num_fence_map_out = req_isp->num_fence_map_out;
		*bytes_updated += bytes_required;
	}

	if (req_isp->num_fence_map_in) {
		bytes_required = sizeof(struct cam_hw_fence_map_entry) *
			req_isp->num_fence_map_in;
		if (start_addr + *bytes_updated + bytes_required > end_addr)
			return;

		req_md->map_in = (struct cam_hw_fence_map_entry *)
			((uint8_t *)start_addr + *bytes_updated);
		memcpy(req_md->map_in, req_isp->fence_map_in, bytes_required);
		req_md->num_fence_map_in = req_isp->num_fence_map_in;
		*bytes_updated += bytes_required;
	}

	packet = req_isp->hw_update_data.packet;
	if (packet && packet->num_io_configs) {
		bytes_required = packet->num_io_configs * sizeof(struct cam_buf_io_cfg);
		if (start_addr + *bytes_updated + bytes_required > end_addr)
			return;

		io_cfg = (struct cam_buf_io_cfg *)((uint32_t *)&packet->payload +
			    packet->io_configs_offset / 4);
		req_md->io_cfg = (struct cam_buf_io_cfg *)((uint8_t *)start_addr + *bytes_updated);
		memcpy(req_md->io_cfg, io_cfg, bytes_required);
		*bytes_updated += bytes_required;
		req_md->num_io_cfg = packet->num_io_configs;
	}
}

static int __cam_isp_ctx_minidump_cb(void *priv, void *args)
{
	struct cam_isp_ctx_mini_dump_info *md;
	struct cam_isp_context            *ctx_isp;
	struct cam_context                *ctx;
	struct cam_ctx_request            *req, *req_temp;
	struct cam_hw_mini_dump_args      *dump_args;
	uint8_t                           *start_addr;
	uint8_t                           *end_addr;
	unsigned long                      total_bytes = 0;
	unsigned long                      bytes_updated = 0;
	uint32_t                           i;

	if (!priv || !args) {
		CAM_ERR(CAM_ISP, "invalid params");
		return 0;
	}

	dump_args = (struct cam_hw_mini_dump_args *)args;
	if (dump_args->len < sizeof(*md)) {
		CAM_ERR(CAM_ISP,
			"In sufficient size received %lu required size: %zu",
			dump_args->len, sizeof(*md));
		return 0;
	}

	ctx = (struct cam_context *)priv;
	ctx_isp = (struct cam_isp_context *)ctx->ctx_priv;
	start_addr = (uint8_t *)dump_args->start_addr;
	end_addr = start_addr + dump_args->len;
	md = (struct cam_isp_ctx_mini_dump_info *)dump_args->start_addr;

	md->sof_timestamp_val = ctx_isp->sof_timestamp_val;
	md->boot_timestamp = ctx_isp->boot_timestamp;
	md->last_sof_timestamp = ctx_isp->last_sof_timestamp;
	md->init_timestamp = ctx_isp->init_timestamp;
	md->frame_id = ctx_isp->frame_id;
	md->reported_req_id = ctx_isp->reported_req_id;
	md->last_applied_req_id = ctx_isp->last_applied_req_id;
	md->last_bufdone_err_apply_req_id =
		ctx_isp->last_bufdone_err_apply_req_id;
	md->frame_id_meta = ctx_isp->frame_id_meta;
	md->substate_activated = ctx_isp->substate_activated;
	md->ctx_id = ctx->ctx_id;
	md->subscribe_event = ctx_isp->subscribe_event;
	md->bubble_frame_cnt = ctx_isp->bubble_frame_cnt;
	md->isp_device_type = ctx_isp->isp_device_type;
	md->active_req_cnt = ctx_isp->active_req_cnt;
	md->trigger_id = ctx_isp->trigger_id;
	md->rdi_only_context = ctx_isp->rdi_only_context;
	md->offline_context = ctx_isp->offline_context;
	md->hw_acquired = ctx_isp->hw_acquired;
	md->init_received = ctx_isp->init_received;
	md->split_acquire = ctx_isp->split_acquire;
	md->use_frame_header_ts = ctx_isp->use_frame_header_ts;
	md->support_consumed_addr = ctx_isp->support_consumed_addr;
	md->use_default_apply = ctx_isp->use_default_apply;
	md->apply_in_progress = atomic_read(&ctx_isp->apply_in_progress);
	md->process_bubble = atomic_read(&ctx_isp->process_bubble);
	md->rxd_epoch = atomic_read(&ctx_isp->rxd_epoch);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++) {
		memcpy(md->event_record[i], ctx_isp->dbg_monitors.event_record[i],
			sizeof(struct cam_isp_context_event_record) *
			CAM_ISP_CTX_EVENT_RECORD_MAX_ENTRIES);
	}

	total_bytes += sizeof(*md);
	if (start_addr + total_bytes >= end_addr)
		goto end;

	if (!list_empty(&ctx->active_req_list)) {
		md->active_list = (struct cam_isp_ctx_req_mini_dump *)
			    (start_addr + total_bytes);
		list_for_each_entry_safe(req, req_temp, &ctx->active_req_list, list) {
			bytes_updated = 0;
			 __cam_isp_ctx_req_mini_dump(req,
				(uint8_t *)&md->active_list[md->active_cnt++],
				end_addr, &bytes_updated);
			total_bytes +=  bytes_updated;
			if ((start_addr + total_bytes >= end_addr))
				goto end;
		}
	}

	if (!list_empty(&ctx->wait_req_list)) {
		md->wait_list = (struct cam_isp_ctx_req_mini_dump *)
			(start_addr + total_bytes);
		list_for_each_entry_safe(req, req_temp, &ctx->wait_req_list, list) {
			bytes_updated = 0;
			__cam_isp_ctx_req_mini_dump(req,
				(uint8_t *)&md->wait_list[md->wait_cnt++],
				end_addr, &bytes_updated);
			total_bytes +=  bytes_updated;
			if ((start_addr + total_bytes >= end_addr))
				goto end;
		}
	}

	if (!list_empty(&ctx->pending_req_list)) {
		md->pending_list = (struct cam_isp_ctx_req_mini_dump *)
			(start_addr + total_bytes);
		list_for_each_entry_safe(req, req_temp, &ctx->pending_req_list, list) {
			bytes_updated = 0;
			__cam_isp_ctx_req_mini_dump(req,
				(uint8_t *)&md->pending_list[md->pending_cnt++],
				end_addr, &bytes_updated);
			total_bytes +=  bytes_updated;
			if ((start_addr + total_bytes >= end_addr))
				goto end;
		}
	}
end:
	dump_args->bytes_written = total_bytes;
	return 0;
}

static void __cam_isp_ctx_update_state_monitor_array(
	struct cam_isp_context *ctx_isp,
	enum cam_isp_state_change_trigger trigger_type,
	uint64_t req_id)
{
	int iterator;
	struct cam_context *ctx = ctx_isp->base;

	INC_HEAD(&ctx_isp->dbg_monitors.state_monitor_head,
		CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES, &iterator);

	ctx_isp->dbg_monitors.state_monitor[iterator].curr_state =
		ctx_isp->substate_activated;
	ctx_isp->dbg_monitors.state_monitor[iterator].frame_id =
		ctx_isp->frame_id;
	ctx_isp->dbg_monitors.state_monitor[iterator].trigger =
		trigger_type;
	ctx_isp->dbg_monitors.state_monitor[iterator].req_id =
		req_id;

	if (trigger_type == CAM_ISP_STATE_CHANGE_TRIGGER_CDM_DONE)
		ctx_isp->dbg_monitors.state_monitor[iterator].evt_time_stamp =
			ctx->cdm_done_ts;
	else
		ktime_get_clocktai_ts64(
			&ctx_isp->dbg_monitors.state_monitor[iterator].evt_time_stamp);
}

static int __cam_isp_ctx_update_frame_timing_record(
	enum cam_isp_hw_event_type hw_evt, struct cam_isp_context *ctx_isp)
{
	uint32_t index = 0;

	/* Update event index on IFE SOF - primary event */
	if (hw_evt == CAM_ISP_HW_EVENT_SOF)
		INC_HEAD(&ctx_isp->dbg_monitors.frame_monitor_head,
			CAM_ISP_CTX_MAX_FRAME_RECORDS, &index);
	else
		div_u64_rem(atomic64_read(&ctx_isp->dbg_monitors.frame_monitor_head),
			CAM_ISP_CTX_MAX_FRAME_RECORDS, &index);

	switch (hw_evt) {
	case CAM_ISP_HW_EVENT_SOF:
		CAM_GET_BOOT_TIMESTAMP(ctx_isp->dbg_monitors.frame_monitor[index].sof_ts);
		break;
	case CAM_ISP_HW_EVENT_EOF:
		CAM_GET_BOOT_TIMESTAMP(ctx_isp->dbg_monitors.frame_monitor[index].eof_ts);
		break;
	case CAM_ISP_HW_EVENT_EPOCH:
		CAM_GET_BOOT_TIMESTAMP(ctx_isp->dbg_monitors.frame_monitor[index].epoch_ts);
		break;
	case CAM_ISP_HW_SECONDARY_EVENT:
		CAM_GET_BOOT_TIMESTAMP(ctx_isp->dbg_monitors.frame_monitor[index].secondary_sof_ts);
		break;
	default:
		break;
	}

	return 0;
}

static void __cam_isp_ctx_dump_frame_timing_record(
	struct cam_isp_context *ctx_isp)
{
	int i = 0;
	int64_t state_head = 0;
	uint32_t index, num_entries, oldest_entry;

	state_head = atomic64_read(&ctx_isp->dbg_monitors.frame_monitor_head);

	if (state_head == -1)
		return;

	if (state_head < CAM_ISP_CTX_MAX_FRAME_RECORDS) {
		num_entries = state_head + 1;
		oldest_entry = 0;
	} else {
		num_entries = CAM_ISP_CTX_MAX_FRAME_RECORDS;
		div_u64_rem(state_head + 1, CAM_ISP_CTX_MAX_FRAME_RECORDS,
			&oldest_entry);
	}

	index = oldest_entry;
	for (i = 0; i < num_entries; i++) {
		CAM_INFO(CAM_ISP,
			"Index: %u SOF_TS: %llu.%llu EPOCH_TS: %llu.%llu EOF_TS: %llu.%llu SEC_SOF: %llu.%llu",
			index,
			ctx_isp->dbg_monitors.frame_monitor[index].sof_ts.tv_sec,
			ctx_isp->dbg_monitors.frame_monitor[index].sof_ts.tv_nsec / NSEC_PER_USEC,
			ctx_isp->dbg_monitors.frame_monitor[index].epoch_ts.tv_sec,
			ctx_isp->dbg_monitors.frame_monitor[index].epoch_ts.tv_nsec / NSEC_PER_USEC,
			ctx_isp->dbg_monitors.frame_monitor[index].eof_ts.tv_sec,
			ctx_isp->dbg_monitors.frame_monitor[index].eof_ts.tv_nsec / NSEC_PER_USEC,
			ctx_isp->dbg_monitors.frame_monitor[index].secondary_sof_ts.tv_sec,
			ctx_isp->dbg_monitors.frame_monitor[index].secondary_sof_ts.tv_nsec /
			NSEC_PER_USEC);

		index = (index + 1) % CAM_ISP_CTX_MAX_FRAME_RECORDS;
	}
}

static const char *__cam_isp_ctx_substate_val_to_type(
	enum cam_isp_ctx_activated_substate type)
{
	switch (type) {
	case CAM_ISP_CTX_ACTIVATED_SOF:
		return "SOF";
	case CAM_ISP_CTX_ACTIVATED_APPLIED:
		return "APPLIED";
	case CAM_ISP_CTX_ACTIVATED_EPOCH:
		return "EPOCH";
	case CAM_ISP_CTX_ACTIVATED_BUBBLE:
		return "BUBBLE";
	case CAM_ISP_CTX_ACTIVATED_BUBBLE_APPLIED:
		return "BUBBLE_APPLIED";
	case CAM_ISP_CTX_ACTIVATED_HW_ERROR:
		return "HW_ERROR";
	case CAM_ISP_CTX_ACTIVATED_HALT:
		return "HALT";
	default:
		return "INVALID";
	}
}

static const char *__cam_isp_hw_evt_val_to_type(
	uint32_t evt_id)
{
	switch (evt_id) {
	case CAM_ISP_STATE_CHANGE_TRIGGER_ERROR:
		return "ERROR";
	case CAM_ISP_STATE_CHANGE_TRIGGER_APPLIED:
		return "APPLIED";
	case CAM_ISP_STATE_CHANGE_TRIGGER_SOF:
		return "SOF";
	case CAM_ISP_STATE_CHANGE_TRIGGER_REG_UPDATE:
		return "REG_UPDATE";
	case CAM_ISP_STATE_CHANGE_TRIGGER_EPOCH:
		return "EPOCH";
	case CAM_ISP_STATE_CHANGE_TRIGGER_EOF:
		return "EOF";
	case CAM_ISP_STATE_CHANGE_TRIGGER_CDM_DONE:
		return "CDM_DONE";
	case CAM_ISP_STATE_CHANGE_TRIGGER_DONE:
		return "DONE";
	case CAM_ISP_STATE_CHANGE_TRIGGER_FLUSH:
		return "FLUSH";
	case CAM_ISP_STATE_CHANGE_TRIGGER_SEC_EVT_SOF:
		return "SEC_EVT_SOF";
	case CAM_ISP_STATE_CHANGE_TRIGGER_SEC_EVT_EPOCH:
		return "SEC_EVT_EPOCH";
	case CAM_ISP_STATE_CHANGE_TRIGGER_FRAME_DROP:
		return "OUT_OF_SYNC_FRAME_DROP";
	default:
		return "CAM_ISP_EVENT_INVALID";
	}
}

static void __cam_isp_ctx_dump_state_monitor_array(
	struct cam_isp_context *ctx_isp)
{
	int i = 0;
	int64_t state_head = 0;
	uint32_t index, num_entries, oldest_entry;
	struct tm ts;

	state_head = atomic64_read(&ctx_isp->dbg_monitors.state_monitor_head);

	if (state_head == -1)
		return;

	if (state_head < CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES) {
		num_entries = state_head + 1;
		oldest_entry = 0;
	} else {
		num_entries = CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES;
		div_u64_rem(state_head + 1, CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES,
			&oldest_entry);
	}

	CAM_ERR(CAM_ISP,
		"Dumping state information for preceding requests");

	index = oldest_entry;

	for (i = 0; i < num_entries; i++) {
		time64_to_tm(ctx_isp->dbg_monitors.state_monitor[index].evt_time_stamp.tv_sec,
			0, &ts);
		CAM_ERR(CAM_ISP,
			"Idx[%d] time[%d-%d %d:%d:%d.%lld]:Substate[%s] Frame[%lld] Req[%llu] evt[%s]",
			index, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
			ctx_isp->dbg_monitors.state_monitor[index].evt_time_stamp.tv_nsec / 1000000,
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->dbg_monitors.state_monitor[index].curr_state),
			ctx_isp->dbg_monitors.state_monitor[index].frame_id,
			ctx_isp->dbg_monitors.state_monitor[index].req_id,
			__cam_isp_hw_evt_val_to_type(
				ctx_isp->dbg_monitors.state_monitor[index].trigger));

		index = (index + 1) % CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES;
	}
}

static void *cam_isp_ctx_user_dump_state_monitor_array_info(
	void *dump_struct, uint8_t *addr_ptr)
{
	struct cam_isp_context_state_monitor  *evt = NULL;
	uint64_t                *addr;

	evt = (struct cam_isp_context_state_monitor *)dump_struct;

	addr = (uint64_t *)addr_ptr;

	*addr++ = evt->evt_time_stamp.tv_sec;
	*addr++ = evt->evt_time_stamp.tv_nsec / NSEC_PER_USEC;
	*addr++ = evt->frame_id;
	*addr++ = evt->req_id;
	return addr;
}

static int __cam_isp_ctx_user_dump_state_monitor_array(
	struct cam_isp_context *ctx_isp,
	struct cam_common_hw_dump_args *dump_args)
{
	int                                          i, rc = 0;
	int                                          index;
	uint32_t                                     oldest_entry;
	uint32_t                                     num_entries;
	uint64_t                                     state_head = 0;

	if (!dump_args || !ctx_isp) {
		CAM_ERR(CAM_ISP, "Invalid args %pK %pK",
			dump_args, ctx_isp);
		return -EINVAL;
	}

	state_head = atomic64_read(&ctx_isp->dbg_monitors.state_monitor_head);

	if (state_head == -1) {
		return 0;
	} else if (state_head < CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES) {
		num_entries = state_head;
		oldest_entry = 0;
	} else {
		num_entries = CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES;
		div_u64_rem(state_head + 1,
			CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES, &oldest_entry);
	}
	CAM_ERR(CAM_ISP,
		"Dumping state information for preceding requests");

	index = oldest_entry;
	for (i = 0; i < num_entries; i++) {

		rc = cam_common_user_dump_helper(dump_args,
			cam_isp_ctx_user_dump_state_monitor_array_info,
			&ctx_isp->dbg_monitors.state_monitor[index],
			sizeof(uint64_t), "ISP_STATE_MONITOR.%s.%s:",
			__cam_isp_ctx_substate_val_to_type(
				ctx_isp->dbg_monitors.state_monitor[index].curr_state),
			__cam_isp_hw_evt_val_to_type(
				ctx_isp->dbg_monitors.state_monitor[index].trigger));

		if (rc) {
			CAM_ERR(CAM_ISP, "CAM ISP CONTEXT: Event record dump failed, rc: %d", rc);
			return rc;
		}

		index = (index + 1) % CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES;

	}
	return rc;
}

static int cam_isp_context_info_dump(void *context,
	enum cam_context_dump_id id)
{
	struct cam_context *ctx = (struct cam_context *)context;

	switch (id) {
	case CAM_CTX_DUMP_ACQ_INFO: {
		cam_context_dump_hw_acq_info(ctx);
		break;
	}
	default:
		CAM_DBG(CAM_ISP, "DUMP id not valid %u, ctx_idx: %u, link: 0x%x",
			id, ctx->ctx_id, ctx->link_hdl);
		break;
	}

	return 0;
}

static const char *__cam_isp_ctx_crm_trigger_point_to_string(
	int trigger_point)
{
	switch (trigger_point) {
	case CAM_TRIGGER_POINT_SOF:
		return "SOF";
	case CAM_TRIGGER_POINT_EOF:
		return "EOF";
	default:
		return "Invalid";
	}
}

static int __cam_isp_ctx_notify_trigger_util(
	int trigger_type, struct cam_isp_context *ctx_isp)
{
	int                                rc = -EINVAL;
	struct cam_context                *ctx = ctx_isp->base;
	struct cam_req_mgr_trigger_notify  notify;

	/* Trigger type not supported, return */
	if (!(ctx_isp->subscribe_event & trigger_type)) {
		CAM_DBG(CAM_ISP,
			"%s trigger point not subscribed for in mask: %u in ctx: %u on link: 0x%x last_bufdone: %lld",
			__cam_isp_ctx_crm_trigger_point_to_string(trigger_type),
			ctx_isp->subscribe_event, ctx->ctx_id, ctx->link_hdl,
			ctx_isp->req_info.last_bufdone_req_id);
		return 0;
	}

	/* Skip CRM notify when recovery is in progress */
	if (atomic_read(&ctx_isp->internal_recovery_set)) {
		CAM_DBG(CAM_ISP,
			"Internal recovery in progress skip notifying %s trigger point in ctx: %u on link: 0x%x",
			__cam_isp_ctx_crm_trigger_point_to_string(trigger_type),
			ctx->ctx_id, ctx->link_hdl);
		return 0;
	}

	notify.link_hdl = ctx->link_hdl;
	notify.dev_hdl = ctx->dev_hdl;
	notify.frame_id = ctx_isp->frame_id;
	notify.trigger = trigger_type;
	notify.req_id = ctx_isp->req_info.last_bufdone_req_id;
	notify.sof_timestamp_val = ctx_isp->sof_timestamp_val;
	notify.trigger_id = ctx_isp->trigger_id;

	CAM_DBG(CAM_ISP,
		"Notify CRM %s on frame: %llu ctx: %u link: 0x%x last_buf_done_req: %lld",
		__cam_isp_ctx_crm_trigger_point_to_string(trigger_type),
		ctx_isp->frame_id, ctx->ctx_id, ctx->link_hdl,
		ctx_isp->req_info.last_bufdone_req_id);

	rc = ctx->ctx_crm_intf->notify_trigger(&notify);
	if (rc)
		CAM_ERR_RATE_LIMIT(CAM_ISP,
			"Failed to notify CRM %s on frame: %llu ctx: %u link: 0x%x last_buf_done_req: %lld rc: %d",
			__cam_isp_ctx_crm_trigger_point_to_string(trigger_type),
			ctx_isp->frame_id, ctx->ctx_id, ctx->link_hdl,
			ctx_isp->req_info.last_bufdone_req_id, rc);

	return rc;
}

static int __cam_isp_ctx_notify_v4l2_error_event(
	uint32_t error_type, uint32_t error_code,
	uint64_t error_request_id, struct cam_context *ctx)
{
	int                         rc = 0;
	struct cam_req_mgr_message  req_msg;

	req_msg.session_hdl = ctx->session_hdl;
	req_msg.u.err_msg.device_hdl = ctx->dev_hdl;
	req_msg.u.err_msg.error_type = error_type;
	req_msg.u.err_msg.link_hdl = ctx->link_hdl;
	req_msg.u.err_msg.request_id = error_request_id;
	req_msg.u.err_msg.resource_size = 0x0;
	req_msg.u.err_msg.error_code = error_code;

	CAM_DBG(CAM_ISP,
		"v4l2 error event [type: %u code: %u] for req: %llu in ctx: %u on link: 0x%x notified successfully",
		error_type, error_code, error_request_id, ctx->ctx_id, ctx->link_hdl);

	rc = cam_req_mgr_notify_message(&req_msg,
			V4L_EVENT_CAM_REQ_MGR_ERROR,
			V4L_EVENT_CAM_REQ_MGR_EVENT);
	if (rc)
		CAM_ERR(CAM_ISP,
			"Notifying v4l2 error [type: %u code: %u] failed for req id:%llu in ctx %u on link: 0x%x",
			error_type, error_code, error_request_id, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_notify_error_util(
	uint32_t trigger_type, enum cam_req_mgr_device_error error,
	uint64_t req_id, struct cam_isp_context *ctx_isp)
{
	int                                rc = -EINVAL;
	struct cam_context                *ctx = ctx_isp->base;
	struct cam_req_mgr_error_notify    notify;

	notify.link_hdl = ctx->link_hdl;
	notify.dev_hdl = ctx->dev_hdl;
	notify.req_id = req_id;
	notify.error = error;
	notify.trigger = trigger_type;
	notify.frame_id = ctx_isp->frame_id;
	notify.sof_timestamp_val = ctx_isp->sof_timestamp_val;

	if ((error == CRM_KMD_ERR_BUBBLE) || (error == CRM_KMD_WARN_INTERNAL_RECOVERY))
		CAM_WARN(CAM_ISP,
			"Notify CRM about bubble req: %llu frame: %llu in ctx: %u on link: 0x%x",
			req_id, ctx_isp->frame_id, ctx->ctx_id, ctx->link_hdl);
	else
		CAM_ERR(CAM_ISP,
			"Notify CRM about fatal error: %u req: %llu frame: %llu in ctx: %u on link: 0x%x",
			error, req_id, ctx_isp->frame_id, ctx->ctx_id, ctx->link_hdl);

	rc = ctx->ctx_crm_intf->notify_err(&notify);
	if (rc)
		CAM_ERR(CAM_ISP,
			"Failed to notify error: %u for req: %lu on ctx: %u in link: 0x%x",
			error, req_id, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_trigger_reg_dump(
	enum cam_hw_mgr_command cmd,
	struct cam_context     *ctx)
{
	int rc = 0;
	struct cam_hw_cmd_args hw_cmd_args;

	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
	hw_cmd_args.cmd_type = cmd;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Reg dump on error failed ctx: %u link: 0x%x rc: %d",
			ctx->ctx_id, ctx->link_hdl, rc);
		goto end;
	}

	CAM_DBG(CAM_ISP,
		"Reg dump type: %u successful in ctx: %u on link: 0x%x",
		cmd, ctx->ctx_id, ctx->link_hdl);

end:
	return rc;
}

static int __cam_isp_ctx_pause_crm_timer(
	struct cam_context *ctx)
{
	int rc = -EINVAL;
	struct cam_req_mgr_timer_notify  timer;

	if (!ctx || !ctx->ctx_crm_intf)
		goto end;

	timer.link_hdl = ctx->link_hdl;
	timer.dev_hdl = ctx->dev_hdl;
	timer.state = false;
	rc = ctx->ctx_crm_intf->notify_timer(&timer);
	if (rc) {
		CAM_ERR(CAM_ISP, "Failed to pause sof timer in ctx: %u on link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	CAM_DBG(CAM_ISP, "Notify CRM to pause timer for ctx: %u link: 0x%x success",
		ctx->ctx_id, ctx->link_hdl);

end:
	return rc;
}

static inline void __cam_isp_ctx_update_sof_ts_util(
	struct cam_isp_hw_sof_event_data *sof_event_data,
	struct cam_isp_context *ctx_isp)
{
	/* Delayed update, skip if ts is already updated */
	if (ctx_isp->sof_timestamp_val == sof_event_data->timestamp)
		return;

	ctx_isp->frame_id++;
	ctx_isp->sof_timestamp_val = sof_event_data->timestamp;
	ctx_isp->boot_timestamp = sof_event_data->boot_time;
}

static int cam_isp_ctx_dump_req(
	struct cam_isp_ctx_req  *req_isp,
	uintptr_t                cpu_addr,
	size_t                   buf_len,
	size_t                  *offset,
	bool                     dump_to_buff)
{
	int i, rc = 0;
	size_t len = 0;
	uint32_t *buf_addr;
	uint32_t *buf_start, *buf_end;
	size_t remain_len = 0;
	struct cam_cdm_cmd_buf_dump_info dump_info;

	for (i = 0; i < req_isp->num_cfg; i++) {
		rc = cam_packet_util_get_cmd_mem_addr(
			req_isp->cfg[i].handle, &buf_addr, &len);
		if (rc) {
			CAM_ERR_RATE_LIMIT(CAM_ISP,
				"Failed to get_cmd_mem_addr, rc=%d",
				rc);
		} else {
			if (req_isp->cfg[i].offset >= ((uint32_t)len)) {
				CAM_ERR(CAM_ISP,
					"Invalid offset exp %u actual %u",
					req_isp->cfg[i].offset, (uint32_t)len);
				cam_mem_put_cpu_buf(req_isp->cfg[i].handle);
				return -EINVAL;
			}
			remain_len = len - req_isp->cfg[i].offset;

			if (req_isp->cfg[i].len >
				((uint32_t)remain_len)) {
				CAM_ERR(CAM_ISP,
					"Invalid len exp %u remain_len %u",
					req_isp->cfg[i].len,
					(uint32_t)remain_len);
				cam_mem_put_cpu_buf(req_isp->cfg[i].handle);
				return -EINVAL;
			}

			buf_start = (uint32_t *)((uint8_t *) buf_addr +
				req_isp->cfg[i].offset);
			buf_end = (uint32_t *)((uint8_t *) buf_start +
				req_isp->cfg[i].len - 1);

			if (dump_to_buff) {
				if (!cpu_addr || !offset || !buf_len) {
					CAM_ERR(CAM_ISP, "Invalid args");
					cam_mem_put_cpu_buf(req_isp->cfg[i].handle);
					break;
				}
				dump_info.src_start = buf_start;
				dump_info.src_end =   buf_end;
				dump_info.dst_start = cpu_addr;
				dump_info.dst_offset = *offset;
				dump_info.dst_max_size = buf_len;
				rc = cam_cdm_util_dump_cmd_bufs_v2(
					&dump_info);
				*offset = dump_info.dst_offset;
				if (rc) {
					cam_mem_put_cpu_buf(req_isp->cfg[i].handle);
					return rc;
				}
			} else
				cam_cdm_util_dump_cmd_buf(buf_start, buf_end);
			cam_mem_put_cpu_buf(req_isp->cfg[i].handle);
		}
	}
	return rc;
}

static int __cam_isp_ctx_enqueue_request_in_order(
	struct cam_context *ctx, struct cam_ctx_request *req, bool lock)
{
	struct cam_ctx_request           *req_current;
	struct cam_ctx_request           *req_prev;
	struct list_head                  temp_list;
	struct cam_isp_context           *ctx_isp;

	INIT_LIST_HEAD(&temp_list);
	if (lock)
		spin_lock_bh(&ctx->lock);
	if (list_empty(&ctx->pending_req_list)) {
		list_add_tail(&req->list, &ctx->pending_req_list);
	} else {
		list_for_each_entry_safe_reverse(
			req_current, req_prev, &ctx->pending_req_list, list) {
			if (req->request_id < req_current->request_id) {
				list_del_init(&req_current->list);
				list_add(&req_current->list, &temp_list);
				continue;
			} else if (req->request_id == req_current->request_id) {
				CAM_WARN(CAM_ISP,
					"Received duplicated request %lld, ctx_idx: %u link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
			}
			break;
		}
		list_add_tail(&req->list, &ctx->pending_req_list);

		if (!list_empty(&temp_list)) {
			list_for_each_entry_safe(
				req_current, req_prev, &temp_list, list) {
				list_del_init(&req_current->list);
				list_add_tail(&req_current->list,
					&ctx->pending_req_list);
			}
		}
	}
	ctx_isp = (struct cam_isp_context *) ctx->ctx_priv;
	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_SUBMIT, req, NULL);
	if (lock)
		spin_unlock_bh(&ctx->lock);
	return 0;
}

static inline void __cam_isp_ctx_move_req_to_free_list(
	struct cam_context *ctx, struct cam_ctx_request *req)
{
	struct cam_isp_ctx_req *req_isp = (struct cam_isp_ctx_req *) req->req_priv;
	struct cam_kmd_buf_info *kmd_cmd_buff_info = &(req_isp->hw_update_data.kmd_cmd_buff_info);

	CAM_DBG(CAM_ISP,
		"Free req id: %lld, ctx_idx: %u, link: 0x%x",
		req->request_id, ctx->ctx_id, ctx->link_hdl);
	if (req->packet) {
		cam_mem_put_kref(kmd_cmd_buff_info->handle);
		cam_common_mem_free(req->packet);
		req->packet = NULL;
	}
	list_add_tail(&req->list, &ctx->free_req_list);
}


static int __cam_isp_ctx_enqueue_init_request(
	struct cam_context *ctx, struct cam_ctx_request *req)
{
	int rc = 0;
	struct cam_ctx_request                *req_old;
	struct cam_isp_ctx_req                *req_isp_old;
	struct cam_isp_ctx_req                *req_isp_new;
	struct cam_isp_prepare_hw_update_data *req_update_old;
	struct cam_isp_prepare_hw_update_data *req_update_new;
	struct cam_isp_prepare_hw_update_data *hw_update_data;
	struct cam_isp_fcg_config_info        *fcg_info_old;
	struct cam_isp_fcg_config_info        *fcg_info_new;
	struct cam_kmd_buf_info *kmd_buff_old = NULL;

	spin_lock_bh(&ctx->lock);
	if (list_empty(&ctx->pending_req_list)) {
		list_add_tail(&req->list, &ctx->pending_req_list);
		CAM_DBG(CAM_ISP, "INIT packet added req id= %d, ctx_idx: %u, link: 0x%x",
			req->request_id, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	req_old = list_first_entry(&ctx->pending_req_list,
		struct cam_ctx_request, list);
	req_isp_old = (struct cam_isp_ctx_req *) req_old->req_priv;
	req_isp_new = (struct cam_isp_ctx_req *) req->req_priv;
	if (req_isp_old->hw_update_data.packet_opcode_type ==
		CAM_ISP_PACKET_INIT_DEV) {
		if ((req_isp_old->num_cfg + req_isp_new->num_cfg) >=
			ctx->max_hw_update_entries) {
			CAM_WARN(CAM_ISP,
				"Can not merge INIT pkt num_cfgs = %d, ctx_idx: %u, link: 0x%x",
				(req_isp_old->num_cfg +
					req_isp_new->num_cfg), ctx->ctx_id, ctx->link_hdl);
			rc = -ENOMEM;
		}

		if (req_isp_old->num_fence_map_out != 0 ||
			req_isp_old->num_fence_map_in != 0) {
			CAM_WARN(CAM_ISP, "Invalid INIT pkt sequence, ctx_idx: %u, link: 0x%x",
				ctx->ctx_id, ctx->link_hdl);
			rc = -EINVAL;
		}

		if (!rc) {
			memcpy(req_isp_old->fence_map_out,
				req_isp_new->fence_map_out,
				sizeof(req_isp_new->fence_map_out[0])*
				req_isp_new->num_fence_map_out);
			req_isp_old->num_fence_map_out =
				req_isp_new->num_fence_map_out;

			memcpy(req_isp_old->fence_map_in,
				req_isp_new->fence_map_in,
				sizeof(req_isp_new->fence_map_in[0])*
				req_isp_new->num_fence_map_in);
			req_isp_old->num_fence_map_in =
				req_isp_new->num_fence_map_in;

			/* Copy hw update entries, num_cfg is updated later */
			memcpy(&req_isp_old->cfg[req_isp_old->num_cfg],
				req_isp_new->cfg,
				sizeof(req_isp_new->cfg[0]) *
				req_isp_new->num_cfg);

			if (req_old->packet) {
				kmd_buff_old = &(req_isp_old->hw_update_data.kmd_cmd_buff_info);
				cam_mem_put_kref(kmd_buff_old->handle);
				cam_common_mem_free(req_old->packet);
				req_old->packet = req->packet;
				req->packet = NULL;
			}

			memcpy(&req_old->pf_data, &req->pf_data,
				sizeof(struct cam_hw_mgr_pf_request_info));

			if (req_isp_new->hw_update_data.num_reg_dump_buf) {
				req_update_new = &req_isp_new->hw_update_data;
				req_update_old = &req_isp_old->hw_update_data;
				memcpy(&req_update_old->reg_dump_buf_desc,
					&req_update_new->reg_dump_buf_desc,
					sizeof(struct cam_cmd_buf_desc) *
					req_update_new->num_reg_dump_buf);
				req_update_old->num_reg_dump_buf =
					req_update_new->num_reg_dump_buf;
			}

			/* Update HW update params for ePCR */
			hw_update_data = &req_isp_new->hw_update_data;
			req_isp_old->hw_update_data.frame_header_res_id =
				req_isp_new->hw_update_data.frame_header_res_id;
			req_isp_old->hw_update_data.frame_header_cpu_addr =
				hw_update_data->frame_header_cpu_addr;
			if (req_isp_new->hw_update_data.mup_en) {
				req_isp_old->hw_update_data.mup_en =
					req_isp_new->hw_update_data.mup_en;
				req_isp_old->hw_update_data.mup_val =
					req_isp_new->hw_update_data.mup_val;
				req_isp_old->hw_update_data.num_exp =
					req_isp_new->hw_update_data.num_exp;
			}

			/* Copy FCG HW update params */
			fcg_info_new = &hw_update_data->fcg_info;
			fcg_info_old = &req_isp_old->hw_update_data.fcg_info;
			fcg_info_old->use_current_cfg = true;

			if (fcg_info_new->ife_fcg_online) {
				fcg_info_old->ife_fcg_online = true;
				fcg_info_old->ife_fcg_entry_idx =
					req_isp_old->num_cfg +
					fcg_info_new->ife_fcg_entry_idx;
				memcpy(&fcg_info_old->ife_fcg_config,
					&fcg_info_new->ife_fcg_config,
					sizeof(struct cam_isp_fcg_config_internal));
			}

			if (fcg_info_new->sfe_fcg_online) {
				fcg_info_old->sfe_fcg_online = true;
				fcg_info_old->sfe_fcg_entry_idx =
					req_isp_old->num_cfg +
					fcg_info_new->sfe_fcg_entry_idx;
				memcpy(&fcg_info_old->sfe_fcg_config,
					&fcg_info_new->sfe_fcg_config,
					sizeof(struct cam_isp_fcg_config_internal));
			}
			req_isp_old->num_cfg += req_isp_new->num_cfg;
			req_old->request_id = req->request_id;
			list_splice_init(&req->buf_tracker, &req_old->buf_tracker);

			list_add_tail(&req->list, &ctx->free_req_list);
		}
	} else {
		CAM_WARN(CAM_ISP,
			"Received Update pkt before INIT pkt. req_id= %lld, ctx_idx: %u, link: 0x%x",
			req->request_id, ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
	}
end:
	spin_unlock_bh(&ctx->lock);
	return rc;
}

static char *__cam_isp_ife_sfe_resource_handle_id_to_type(
	uint32_t resource_handle)
{
	switch (resource_handle) {
	/* IFE output ports */
	case CAM_ISP_IFE_OUT_RES_FULL:                  return "IFE_FULL";
	case CAM_ISP_IFE_OUT_RES_DS4:                   return "IFE_DS4";
	case CAM_ISP_IFE_OUT_RES_DS16:                  return "IFE_DS16";
	case CAM_ISP_IFE_OUT_RES_RAW_DUMP:              return "IFE_RAW_DUMP";
	case CAM_ISP_IFE_OUT_RES_FD:                    return "IFE_FD";
	case CAM_ISP_IFE_OUT_RES_PDAF:                  return "IFE_PDAF";
	case CAM_ISP_IFE_OUT_RES_RDI_0:                 return "IFE_RDI_0";
	case CAM_ISP_IFE_OUT_RES_RDI_1:                 return "IFE_RDI_1";
	case CAM_ISP_IFE_OUT_RES_RDI_2:                 return "IFE_RDI_2";
	case CAM_ISP_IFE_OUT_RES_RDI_3:                 return "IFE_RDI_3";
	case CAM_ISP_IFE_OUT_RES_STATS_HDR_BE:          return "IFE_STATS_HDR_BE";
	case CAM_ISP_IFE_OUT_RES_STATS_HDR_BHIST:       return "IFE_STATS_HDR_BHIST";
	case CAM_ISP_IFE_OUT_RES_STATS_TL_BG:           return "IFE_STATS_TL_BG";
	case CAM_ISP_IFE_OUT_RES_STATS_BF:              return "IFE_STATS_BF";
	case CAM_ISP_IFE_OUT_RES_STATS_AWB_BG:          return "IFE_STATS_AWB_BG";
	case CAM_ISP_IFE_OUT_RES_STATS_BHIST:           return "IFE_STATS_BHIST";
	case CAM_ISP_IFE_OUT_RES_STATS_RS:              return "IFE_STATS_RS";
	case CAM_ISP_IFE_OUT_RES_STATS_CS:              return "IFE_STATS_CS";
	case CAM_ISP_IFE_OUT_RES_STATS_IHIST:           return "IFE_STATS_IHIST";
	case CAM_ISP_IFE_OUT_RES_FULL_DISP:             return "IFE_FULL_DISP";
	case CAM_ISP_IFE_OUT_RES_DS4_DISP:              return "IFE_DS4_DISP";
	case CAM_ISP_IFE_OUT_RES_DS16_DISP:             return "IFE_DS16_DISP";
	case CAM_ISP_IFE_OUT_RES_2PD:                   return "IFE_2PD";
	case CAM_ISP_IFE_OUT_RES_LCR:                   return "IFE_LCR";
	case CAM_ISP_IFE_OUT_RES_AWB_BFW:               return "IFE_AWB_BFW";
	case CAM_ISP_IFE_OUT_RES_PREPROCESS_2PD:        return "IFE_PREPROCESS_2PD";
	case CAM_ISP_IFE_OUT_RES_STATS_AEC_BE:          return "IFE_STATS_AEC_BE";
	case CAM_ISP_IFE_OUT_RES_LTM_STATS:             return "IFE_LTM_STATS";
	case CAM_ISP_IFE_OUT_RES_STATS_GTM_BHIST:       return "IFE_STATS_GTM_BHIST";
	case CAM_ISP_IFE_LITE_OUT_RES_STATS_BG:         return "IFE_STATS_BG";
	case CAM_ISP_IFE_LITE_OUT_RES_PREPROCESS_RAW:   return "IFE_PREPROCESS_RAW";
	case CAM_ISP_IFE_OUT_RES_SPARSE_PD:             return "IFE_SPARSE_PD";
	case CAM_ISP_IFE_OUT_RES_STATS_CAF:             return "IFE_STATS_CAF";
	case CAM_ISP_IFE_OUT_RES_STATS_BAYER_RS:        return "IFE_STATS_BAYER_RS";
	case CAM_ISP_IFE_OUT_RES_PDAF_PARSED_DATA:      return "IFE_PDAF_PARSED_DATA";
	case CAM_ISP_IFE_OUT_RES_STATS_ALSC:            return "IFE_STATS_ALSC";
	/* SFE output ports */
	case CAM_ISP_SFE_OUT_RES_RDI_0:                 return "SFE_RDI_0";
	case CAM_ISP_SFE_OUT_RES_RDI_1:                 return "SFE_RDI_1";
	case CAM_ISP_SFE_OUT_RES_RDI_2:                 return "SFE_RDI_2";
	case CAM_ISP_SFE_OUT_RES_RDI_3:                 return "SFE_RDI_3";
	case CAM_ISP_SFE_OUT_RES_RDI_4:                 return "SFE_RDI_4";
	case CAM_ISP_SFE_OUT_BE_STATS_0:                return "SFE_BE_STATS_0";
	case CAM_ISP_SFE_OUT_BE_STATS_1:                return "SFE_BE_STATS_1";
	case CAM_ISP_SFE_OUT_BE_STATS_2:                return "SFE_BE_STATS_2";
	case CAM_ISP_SFE_OUT_BHIST_STATS_0:             return "SFE_BHIST_STATS_0";
	case CAM_ISP_SFE_OUT_BHIST_STATS_1:             return "SFE_BHIST_STATS_1";
	case CAM_ISP_SFE_OUT_BHIST_STATS_2:             return "SFE_BHIST_STATS_2";
	case CAM_ISP_SFE_OUT_RES_LCR:                   return "SFE_LCR";
	case CAM_ISP_SFE_OUT_RES_RAW_DUMP:              return "SFE_PROCESSED_RAW";
	case CAM_ISP_SFE_OUT_RES_IR:                    return "SFE_IR";
	case CAM_ISP_SFE_OUT_BAYER_RS_STATS_0:          return "SFE_RS_STATS_0";
	case CAM_ISP_SFE_OUT_BAYER_RS_STATS_1:          return "SFE_RS_STATS_1";
	case CAM_ISP_SFE_OUT_BAYER_RS_STATS_2:          return "SFE_RS_STATS_2";
	case CAM_ISP_SFE_OUT_HDR_STATS:                 return "HDR_STATS";
	/* SFE input ports */
	case CAM_ISP_SFE_IN_RD_0:                       return "SFE_RD_0";
	case CAM_ISP_SFE_IN_RD_1:                       return "SFE_RD_1";
	case CAM_ISP_SFE_IN_RD_2:                       return "SFE_RD_2";
	/* Handle invalid type */
	default:                                        return "Invalid_Resource_Type";
	}
}

static const char *__cam_isp_tfe_resource_handle_id_to_type(
	uint32_t resource_handle)
{
	switch (resource_handle) {
	/* TFE output ports */
	case CAM_ISP_TFE_OUT_RES_FULL:                  return "TFE_FULL";
	case CAM_ISP_TFE_OUT_RES_RAW_DUMP:              return "TFE_RAW_DUMP";
	case CAM_ISP_TFE_OUT_RES_PDAF:                  return "TFE_PDAF";
	case CAM_ISP_TFE_OUT_RES_RDI_0:                 return "TFE_RDI_0";
	case CAM_ISP_TFE_OUT_RES_RDI_1:                 return "TFE_RDI_1";
	case CAM_ISP_TFE_OUT_RES_RDI_2:                 return "TFE_RDI_2";
	case CAM_ISP_TFE_OUT_RES_STATS_HDR_BE:          return "TFE_STATS_HDR_BE";
	case CAM_ISP_TFE_OUT_RES_STATS_HDR_BHIST:       return "TFE_STATS_HDR_BHIST";
	case CAM_ISP_TFE_OUT_RES_STATS_TL_BG:           return "TFE_STATS_TL_BG";
	case CAM_ISP_TFE_OUT_RES_STATS_BF:              return "TFE_STATS_BF";
	case CAM_ISP_TFE_OUT_RES_STATS_AWB_BG:          return "TFE_STATS_AWB_BG";
	case CAM_ISP_TFE_OUT_RES_STATS_RS:              return "TFE_STATS_RS";
	case CAM_ISP_TFE_OUT_RES_DS4:                   return "TFE_DS_4";
	case CAM_ISP_TFE_OUT_RES_DS16:                  return "TFE_DS_16";
	case CAM_ISP_TFE_OUT_RES_AI:                    return "TFE_AI";
	case CAM_ISP_TFE_OUT_RES_PD_LCR_STATS:          return "TFE_LCR_STATS";
	case CAM_ISP_TFE_OUT_RES_PD_PREPROCESSED:       return "TFE_PD_PREPROCESSED";
	case CAM_ISP_TFE_OUT_RES_PD_PARSED:             return "TFE_PD_PARSED";
	/* Handle invalid type */
	default:                                        return "Invalid_Resource_Type";
	}
}

static const char *__cam_isp_resource_handle_id_to_type(
	uint32_t device_type, uint32_t resource_handle)
{
	switch (device_type) {
	case CAM_IFE_DEVICE_TYPE:
	case CAM_TFE_MC_DEVICE_TYPE:
		return __cam_isp_ife_sfe_resource_handle_id_to_type(resource_handle);
	case CAM_TFE_DEVICE_TYPE:
		return __cam_isp_tfe_resource_handle_id_to_type(resource_handle);
	default:
		return "INVALID_DEV_TYPE";
	}
}

static uint64_t __cam_isp_ctx_get_event_ts(uint32_t evt_id, void *evt_data)
{
	uint64_t ts = 0;

	if (!evt_data)
		return 0;

	switch (evt_id) {
	case CAM_ISP_HW_EVENT_ERROR:
		ts = ((struct cam_isp_hw_error_event_data *)evt_data)->
			timestamp;
		break;
	case CAM_ISP_HW_EVENT_SOF:
		ts = ((struct cam_isp_hw_sof_event_data *)evt_data)->
			timestamp;
		break;
	case CAM_ISP_HW_EVENT_REG_UPDATE:
		ts = ((struct cam_isp_hw_reg_update_event_data *)evt_data)->
			timestamp;
		break;
	case CAM_ISP_HW_EVENT_EPOCH:
		ts = ((struct cam_isp_hw_epoch_event_data *)evt_data)->
			timestamp;
		break;
	case CAM_ISP_HW_EVENT_EOF:
		ts = ((struct cam_isp_hw_eof_event_data *)evt_data)->
			timestamp;
		break;
	case CAM_ISP_HW_EVENT_DONE:
	case CAM_ISP_HW_SECONDARY_EVENT:
		break;
	default:
		CAM_DBG(CAM_ISP, "Invalid Event Type %d", evt_id);
	}

	return ts;
}

static int __cam_isp_ctx_get_hw_timestamp(struct cam_context *ctx, uint64_t *prev_ts,
	uint64_t *curr_ts, uint64_t *boot_ts)
{
	struct cam_hw_cmd_args hw_cmd_args;
	struct cam_isp_hw_cmd_args isp_hw_cmd_args;
	int rc;

	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	hw_cmd_args.u.internal_args = &isp_hw_cmd_args;

	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_GET_SOF_TS;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->ctxt_to_hw_map, &hw_cmd_args);
	if (rc)
		return rc;

	if (isp_hw_cmd_args.u.sof_ts.prev >= isp_hw_cmd_args.u.sof_ts.curr) {
		CAM_ERR(CAM_ISP, "ctx:%u link:0x%x prev timestamp greater than curr timestamp",
			ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	*prev_ts = isp_hw_cmd_args.u.sof_ts.prev;
	*curr_ts = isp_hw_cmd_args.u.sof_ts.curr;
	*boot_ts = isp_hw_cmd_args.u.sof_ts.boot;

	return 0;
}

static int __cam_isp_ctx_get_cdm_done_timestamp(struct cam_context *ctx,
	uint64_t *last_cdm_done_req)
{
	struct cam_hw_cmd_args hw_cmd_args;
	struct cam_isp_hw_cmd_args isp_hw_cmd_args;
	struct tm ts;
	int rc;

	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	hw_cmd_args.u.internal_args = &isp_hw_cmd_args;

	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_GET_LAST_CDM_DONE;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->ctxt_to_hw_map, &hw_cmd_args);
	if (rc)
		return rc;

	*last_cdm_done_req = isp_hw_cmd_args.u.last_cdm_done;
	ctx->cdm_done_ts = isp_hw_cmd_args.cdm_done_ts;
	time64_to_tm(isp_hw_cmd_args.cdm_done_ts.tv_sec, 0, &ts);
	CAM_DBG(CAM_ISP,
		"last_cdm_done req: %llu ctx: %u link: 0x%x time[%d-%d %d:%d:%d.%lld]",
		last_cdm_done_req, ctx->ctx_id, ctx->link_hdl,
		ts.tm_mon + 1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec,
		isp_hw_cmd_args.cdm_done_ts.tv_nsec / 1000000);

	return 0;
}

static int __cam_isp_ctx_recover_sof_timestamp(struct cam_context *ctx, uint64_t request_id)
{
	struct cam_isp_context *ctx_isp = ctx->ctx_priv;
	uint64_t prev_ts, curr_ts, boot_ts;
	uint64_t a, b, c;
	int rc;

	rc = __cam_isp_ctx_get_hw_timestamp(ctx, &prev_ts, &curr_ts, &boot_ts);
	if (rc) {
		CAM_ERR(CAM_ISP, "ctx:%u link: 0x%x Failed to get timestamp from HW",
			ctx->ctx_id, ctx->link_hdl);
		return rc;
	}

	/**
	 * If the last received SOF was for frame A and we have missed the SOF for frame B,
	 * then we need to find out if the hardware is at frame B or C.
	 *   +-----+-----+-----+
	 *   |  A  |  B  |  C  |
	 *   +-----+-----+-----+
	 */
	a = ctx_isp->sof_timestamp_val;
	if (a == prev_ts) {
		/* Hardware is at frame B */
		b = curr_ts;
		CAM_DBG(CAM_ISP, "ctx:%u link:0x%x recover time(last:0x%llx,curr:0x%llx)req:%llu",
			ctx->ctx_id, ctx->link_hdl, a, b, request_id);
	} else if (a < prev_ts) {
		/* Hardware is at frame C */
		b = prev_ts;
		c = curr_ts;

		CAM_DBG(CAM_ISP,
			"ctx:%u link:0x%x recover time(last:0x%llx,prev:0x%llx,curr:0x%llx)req:%llu",
			ctx->ctx_id, ctx->link_hdl, a, b, c, request_id);
	} else {
		/* Hardware is at frame A (which we supposedly missed) */
		CAM_ERR_RATE_LIMIT(CAM_ISP,
			"ctx:%u link: 0x%x erroneous call to SOF recovery(last:0x%llx, prev:0x%llx, curr:0x%llx) req: %llu",
			ctx->ctx_id, ctx->link_hdl, a, prev_ts, curr_ts, request_id);
		return 0;
	}

	ctx_isp->boot_timestamp = boot_ts + (b - curr_ts);
	ctx_isp->sof_timestamp_val = b;
	ctx_isp->frame_id++;
	return 0;
}

static void __cam_isp_ctx_send_sof_boot_timestamp(
	struct cam_isp_context *ctx_isp, uint64_t request_id,
	uint32_t sof_event_status, struct shutter_event *shutter_event)
{
	struct cam_req_mgr_message   req_msg;

	req_msg.session_hdl = ctx_isp->base->session_hdl;
	req_msg.u.frame_msg.frame_id = ctx_isp->frame_id;
	req_msg.u.frame_msg.request_id = request_id;
	req_msg.u.frame_msg.timestamp = ctx_isp->boot_timestamp;
	req_msg.u.frame_msg.link_hdl = ctx_isp->base->link_hdl;
	req_msg.u.frame_msg.sof_status = sof_event_status;
	req_msg.u.frame_msg.frame_id_meta = ctx_isp->frame_id_meta;

	CAM_DBG(CAM_ISP,
		"request id:%lld frame number:%lld boot time stamp:0x%llx status:%u",
		 request_id, ctx_isp->frame_id,
		 ctx_isp->boot_timestamp, sof_event_status);
	shutter_event->frame_id = ctx_isp->frame_id;
	shutter_event->req_id   = request_id;
	shutter_event->boot_ts  = ctx_isp->boot_timestamp;
	shutter_event->sof_ts   = ctx_isp->sof_timestamp_val;

	if (cam_req_mgr_notify_message(&req_msg,
		V4L_EVENT_CAM_REQ_MGR_SOF_BOOT_TS,
		V4L_EVENT_CAM_REQ_MGR_EVENT))
		CAM_ERR(CAM_ISP,
			"Error in notifying the boot time for req id:%lld",
			request_id);

	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_SHUTTER, NULL, shutter_event);
}

static void __cam_isp_ctx_send_unified_timestamp(
	struct cam_isp_context *ctx_isp, uint64_t request_id,
	struct shutter_event *shutter_event)
{
	struct cam_req_mgr_message   req_msg;

	req_msg.session_hdl = ctx_isp->base->session_hdl;
	req_msg.u.frame_msg_v2.frame_id = ctx_isp->frame_id;
	req_msg.u.frame_msg_v2.request_id = request_id;
	req_msg.u.frame_msg_v2.timestamps[CAM_REQ_SOF_QTIMER_TIMESTAMP] =
		(request_id == 0) ? 0 : ctx_isp->sof_timestamp_val;
	req_msg.u.frame_msg_v2.timestamps[CAM_REQ_BOOT_TIMESTAMP] = ctx_isp->boot_timestamp;
	req_msg.u.frame_msg_v2.link_hdl = ctx_isp->base->link_hdl;
	req_msg.u.frame_msg_v2.frame_id_meta = ctx_isp->frame_id_meta;

	CAM_DBG(CAM_ISP,
		"link hdl 0x%x request id:%lld frame number:%lld SOF time stamp:0x%llx ctx %d\
		boot time stamp:0x%llx", ctx_isp->base->link_hdl, request_id,
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val,ctx_isp->base->ctx_id,
		ctx_isp->boot_timestamp);
	shutter_event->frame_id = ctx_isp->frame_id;
	shutter_event->req_id   = request_id;
	shutter_event->boot_ts  = ctx_isp->boot_timestamp;
	shutter_event->sof_ts   = ctx_isp->sof_timestamp_val;

	if (cam_req_mgr_notify_message(&req_msg,
		V4L_EVENT_CAM_REQ_MGR_SOF_UNIFIED_TS, V4L_EVENT_CAM_REQ_MGR_EVENT))
		CAM_ERR(CAM_ISP,
			"Error in notifying the sof and boot time for req id:%lld",
			request_id);
	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_SHUTTER, NULL, shutter_event);
}

static void __cam_isp_ctx_send_sof_timestamp_frame_header(
	struct cam_isp_context *ctx_isp, uint32_t *frame_header_cpu_addr,
	uint64_t request_id, uint32_t sof_event_status)
{
	uint32_t *time32 = NULL;
	uint64_t timestamp = 0;
	struct cam_req_mgr_message   req_msg;

	time32 = frame_header_cpu_addr;
	timestamp = (uint64_t) time32[1];
	timestamp = timestamp << 24;
	timestamp |= (uint64_t)(time32[0] >> 8);
	timestamp = mul_u64_u32_div(timestamp,
			CAM_IFE_QTIMER_MUL_FACTOR,
			CAM_IFE_QTIMER_DIV_FACTOR);

	ctx_isp->sof_timestamp_val = timestamp;
	req_msg.session_hdl = ctx_isp->base->session_hdl;
	req_msg.u.frame_msg.frame_id = ctx_isp->frame_id;
	req_msg.u.frame_msg.request_id = request_id;
	req_msg.u.frame_msg.timestamp = ctx_isp->sof_timestamp_val;
	req_msg.u.frame_msg.link_hdl = ctx_isp->base->link_hdl;
	req_msg.u.frame_msg.sof_status = sof_event_status;

	CAM_DBG(CAM_ISP,
		"request id:%lld frame number:%lld SOF time stamp:0x%llx status:%u",
		 request_id, ctx_isp->frame_id,
		ctx_isp->sof_timestamp_val, sof_event_status);

	if (cam_req_mgr_notify_message(&req_msg,
		V4L_EVENT_CAM_REQ_MGR_SOF, V4L_EVENT_CAM_REQ_MGR_EVENT))
		CAM_ERR(CAM_ISP,
			"Error in notifying the sof time for req id:%lld",
			request_id);
}

static void __cam_isp_ctx_send_sof_timestamp(
	struct cam_isp_context *ctx_isp, uint64_t request_id,
	uint32_t sof_event_status)
{
	struct cam_req_mgr_message   req_msg;
	struct cam_context           *ctx = ctx_isp->base;
	struct shutter_event         shutter_event = {0};

	if (ctx_isp->reported_frame_id == ctx_isp->frame_id) {
		if (__cam_isp_ctx_recover_sof_timestamp(ctx_isp->base, request_id))
			CAM_WARN(CAM_ISP, "Missed SOF.No SOF timestamp recovery,ctx:%u,link:0x%x",
				ctx->ctx_id, ctx->link_hdl);
	}

	if (request_id == 0 && (ctx_isp->reported_frame_id == ctx_isp->frame_id)) {
		CAM_WARN_RATE_LIMIT(CAM_ISP,
			"Missed SOF Recovery for invalid req, Skip notificaiton to userspace Ctx: %u link: 0x%x frame_id %u",
			ctx->ctx_id, ctx->link_hdl, ctx_isp->frame_id);
		return;
	}

	ctx_isp->reported_frame_id = ctx_isp->frame_id;
	shutter_event.status = sof_event_status;

	if ((ctx_isp->v4l2_event_sub_ids & (1 << V4L_EVENT_CAM_REQ_MGR_SOF_UNIFIED_TS))
		&& !ctx_isp->use_frame_header_ts) {
		__cam_isp_ctx_send_unified_timestamp(ctx_isp, request_id, &shutter_event);
		return;
	}

	if ((ctx_isp->use_frame_header_ts) || (request_id == 0))
		goto end;

	req_msg.session_hdl = ctx_isp->base->session_hdl;
	req_msg.u.frame_msg.frame_id = ctx_isp->frame_id;
	req_msg.u.frame_msg.request_id = request_id;
	req_msg.u.frame_msg.timestamp = ctx_isp->sof_timestamp_val;
	req_msg.u.frame_msg.link_hdl = ctx_isp->base->link_hdl;
	req_msg.u.frame_msg.sof_status = sof_event_status;
	req_msg.u.frame_msg.frame_id_meta = ctx_isp->frame_id_meta;

	CAM_DBG(CAM_ISP,
		"request id:%lld frame number:%lld SOF time stamp:0x%llx status:%u ctx_idx: %u, link: 0x%x",
		request_id, ctx_isp->frame_id,
		ctx_isp->sof_timestamp_val, sof_event_status, ctx->ctx_id, ctx->link_hdl);

	if (cam_req_mgr_notify_message(&req_msg,
		V4L_EVENT_CAM_REQ_MGR_SOF, V4L_EVENT_CAM_REQ_MGR_EVENT))
		CAM_ERR(CAM_ISP,
			"Error in notifying the sof time for req id:%lld, ctx_idx: %u, link: 0x%x",
			request_id, ctx->ctx_id, ctx->link_hdl);

end:
	__cam_isp_ctx_send_sof_boot_timestamp(ctx_isp,
		request_id, sof_event_status, &shutter_event);
}

static void __cam_isp_ctx_handle_buf_done_fail_log(
	struct cam_isp_context *ctx_isp, uint64_t request_id,
	struct cam_isp_ctx_req *req_isp)
{
	int i;
	struct cam_context *ctx = ctx_isp->base;
	const char *handle_type;

	if (req_isp->num_fence_map_out >= CAM_ISP_CTX_RES_MAX) {
		CAM_ERR(CAM_ISP,
			"Num Resources exceed mMAX %d >= %d ",
			req_isp->num_fence_map_out, CAM_ISP_CTX_RES_MAX);
		return;
	}

	CAM_WARN_RATE_LIMIT(CAM_ISP,
		"Prev Req[%lld] : num_out=%d, num_acked=%d, bubble : report=%d, detected=%d",
		request_id, req_isp->num_fence_map_out, req_isp->num_acked,
		req_isp->bubble_report, req_isp->bubble_detected);
	CAM_WARN_RATE_LIMIT(CAM_ISP,
		"Resource Handles that fail to generate buf_done in prev frame");
	for (i = 0; i < req_isp->num_fence_map_out; i++) {
		if (req_isp->fence_map_out[i].sync_id != -1) {
			handle_type = __cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type,
				req_isp->fence_map_out[i].resource_handle);

			trace_cam_log_event("Buf_done Congestion",
				handle_type, request_id, req_isp->fence_map_out[i].sync_id);

			CAM_WARN_RATE_LIMIT(CAM_ISP,
				"Resource_Handle: [%s][0x%x] Sync_ID: [0x%x]",
				handle_type,
				req_isp->fence_map_out[i].resource_handle,
				req_isp->fence_map_out[i].sync_id);
		}
	}

	ctx_isp->congestion_cnt++;

	/* Trigger SOF freeze debug dump on 3 or greater instances of congestion */
	if ((ctx_isp->congestion_cnt >= CAM_ISP_CONTEXT_CONGESTION_CNT_MAX) &&
		(!ctx_isp->sof_dbg_irq_en))
		__cam_isp_ctx_handle_sof_freeze_evt(ctx);
}

static void __cam_isp_context_reset_internal_recovery_params(
	struct cam_isp_context    *ctx_isp)
{
	atomic_set(&ctx_isp->internal_recovery_set, 0);
	atomic_set(&ctx_isp->process_bubble, 0);
	ctx_isp->aeb_error_cnt = 0;
	ctx_isp->bubble_frame_cnt = 0;
	ctx_isp->congestion_cnt = 0;
	ctx_isp->sof_dbg_irq_en = false;
}

static int __cam_isp_context_try_internal_recovery(
	struct cam_isp_context    *ctx_isp)
{
	int rc = 0;
	struct cam_context        *ctx = ctx_isp->base;
	struct cam_ctx_request    *req;
	struct cam_isp_ctx_req    *req_isp;

	/*
	 * Start with wait list, if recovery is stil set
	 * errored request has not been moved to pending yet.
	 * Buf done for errored request has not occurred recover
	 * from here
	 */
	if (!list_empty(&ctx->wait_req_list)) {
		req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request, list);
		req_isp = (struct cam_isp_ctx_req *)req->req_priv;

		if (req->request_id == ctx_isp->recovery_req_id) {
			rc = __cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF,
				CRM_KMD_WARN_INTERNAL_RECOVERY, ctx_isp->recovery_req_id, ctx_isp);
			if (rc) {
				/* Unable to do bubble recovery reset back to normal */
				CAM_WARN(CAM_ISP,
					"Unable to perform internal recovery [bubble reporting failed] for req: %llu in ctx: %u on link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				__cam_isp_context_reset_internal_recovery_params(ctx_isp);
				req_isp->bubble_detected = false;
				goto end;
			}

			list_del_init(&req->list);
			list_add(&req->list, &ctx->pending_req_list);
			ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;
			CAM_INFO(CAM_ISP,
				"Internal recovery for req: %llu in ctx: %u on link: 0x%x triggered",
				ctx_isp->recovery_req_id, ctx->ctx_id, ctx->link_hdl);
			goto end;
		}
	}

	/*
	 * If not in wait list only other possibility is request is in pending list
	 * on error detection, bubble detect is set assuming new frame after detection
	 * comes in, there is an rup it's moved to active list and it finishes with
	 * it's buf done's
	 */
	if (!list_empty(&ctx->pending_req_list)) {
		req = list_first_entry(&ctx->pending_req_list, struct cam_ctx_request, list);
		req_isp = (struct cam_isp_ctx_req *)req->req_priv;

		if (req->request_id == ctx_isp->recovery_req_id) {
			rc = __cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF,
				CRM_KMD_WARN_INTERNAL_RECOVERY, ctx_isp->recovery_req_id, ctx_isp);
			if (rc) {
				/* Unable to do bubble recovery reset back to normal */
				CAM_WARN(CAM_ISP,
					"Unable to perform internal recovery [bubble reporting failed] for req: %llu in ctx: %u on link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				__cam_isp_context_reset_internal_recovery_params(ctx_isp);
				req_isp->bubble_detected = false;
				goto end;
			}
			ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;
			CAM_INFO(CAM_ISP,
				"Internal recovery for req: %llu in ctx: %u on link: 0x%x triggered",
				ctx_isp->recovery_req_id, ctx->ctx_id, ctx->link_hdl);
			goto end;
		}
	}

	/* If request is not found in either of the lists skip recovery */
	__cam_isp_context_reset_internal_recovery_params(ctx_isp);

end:
	return rc;
}

static int __cam_isp_ctx_handle_buf_done_for_req_list(
	struct cam_isp_context *ctx_isp,
	struct cam_ctx_request *req)
{
	int rc = 0, i;
	uint64_t buf_done_req_id;
	struct cam_isp_ctx_req *req_isp;
	struct cam_context *ctx = ctx_isp->base;

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;
	ctx_isp->active_req_cnt--;
	buf_done_req_id = req->request_id;

	if (req_isp->bubble_detected && req_isp->bubble_report) {
		req_isp->num_acked = 0;
		req_isp->num_deferred_acks = 0;
		req_isp->bubble_detected = false;
		list_del_init(&req->list);
		atomic_set(&ctx_isp->process_bubble, 0);
		req_isp->cdm_reset_before_apply = false;
		ctx_isp->bubble_frame_cnt = 0;

		if (buf_done_req_id <= ctx->last_flush_req) {
			cam_smmu_buffer_tracker_putref(&req->buf_tracker);
			for (i = 0; i < req_isp->num_fence_map_out; i++)
				rc = cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_ERROR,
					CAM_SYNC_ISP_EVENT_BUBBLE);
			__cam_isp_ctx_move_req_to_free_list(ctx, req);
			CAM_DBG(CAM_REQ,
				"Move active request %lld to free list(cnt = %d) [flushed], ctx %u, link: 0x%x",
				buf_done_req_id, ctx_isp->active_req_cnt,
				ctx->ctx_id, ctx->link_hdl);
			ctx_isp->last_bufdone_err_apply_req_id = 0;
		} else {
			list_add(&req->list, &ctx->pending_req_list);
			CAM_DBG(CAM_REQ,
				"Move active request %lld to pending list(cnt = %d) [bubble recovery], ctx %u, link: 0x%x",
				req->request_id, ctx_isp->active_req_cnt,
				ctx->ctx_id, ctx->link_hdl);
		}
	} else {
		if (!ctx_isp->use_frame_header_ts) {
			if (ctx_isp->reported_req_id < buf_done_req_id) {
				ctx_isp->reported_req_id = buf_done_req_id;
				__cam_isp_ctx_send_sof_timestamp(ctx_isp,
					buf_done_req_id,
					CAM_REQ_MGR_SOF_EVENT_SUCCESS);
			}
		}
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
		req_isp->reapply_type = CAM_CONFIG_REAPPLY_NONE;
		req_isp->cdm_reset_before_apply = false;
		req_isp->num_acked = 0;
		req_isp->num_deferred_acks = 0;
		/*
		 * Only update the process_bubble and bubble_frame_cnt
		 * when bubble is detected on this req, in case the other
		 * request is processing bubble.
		 */
		if (req_isp->bubble_detected) {
			atomic_set(&ctx_isp->process_bubble, 0);
			ctx_isp->bubble_frame_cnt = 0;
			req_isp->bubble_detected = false;
		}
		CAM_DBG(CAM_REQ,
			"Move active request %lld to free list(cnt = %d) [all fences done], ctx %u link: 0x%x",
			buf_done_req_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
		ctx_isp->req_info.last_bufdone_req_id = req->request_id;
		ctx_isp->last_bufdone_err_apply_req_id = 0;
	}

	if (atomic_read(&ctx_isp->internal_recovery_set) && !ctx_isp->active_req_cnt)
		__cam_isp_context_try_internal_recovery(ctx_isp);

	cam_cpas_notify_event("IFE BufDone", buf_done_req_id);

	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_DONE, buf_done_req_id);

	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_BUFDONE, req, NULL);
	return rc;
}

static int __cam_isp_ctx_handle_buf_done_for_request(
	struct cam_isp_context *ctx_isp,
	struct cam_ctx_request  *req,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state,
	struct cam_isp_hw_done_event_data *done_next_req)
{
	int rc = 0;
	int i, j, k;
	bool not_found = false;
	struct cam_isp_ctx_req *req_isp;
	struct cam_context *ctx = ctx_isp->base;
	const char *handle_type;
	struct cam_isp_context_comp_record *comp_grp = NULL;

	trace_cam_buf_done("ISP", ctx, req);

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	CAM_DBG(CAM_ISP, "Enter with bubble_state %d, req_bubble_detected %d, ctx %u link: 0x%x",
		bubble_state, req_isp->bubble_detected, ctx->ctx_id, ctx->link_hdl);

	done_next_req->resource_handle = 0;
	done_next_req->timestamp = done->timestamp;

	for (i = 0; i < req_isp->num_fence_map_out; i++) {
		if (done->resource_handle ==
			req_isp->fence_map_out[i].resource_handle)
			break;
	}

	if (done->hw_type == CAM_ISP_HW_TYPE_SFE)
		comp_grp = &ctx_isp->sfe_bus_comp_grp[done->comp_group_id];
	else
		comp_grp = &ctx_isp->vfe_bus_comp_grp[done->comp_group_id];

	if (!comp_grp) {
		CAM_ERR(CAM_ISP, "comp_grp is NULL");
		rc = -EINVAL;
		return rc;
	}

	if (i == req_isp->num_fence_map_out) {
		for (j = 0; j < comp_grp->num_res; j++) {
			not_found = false;
			if (comp_grp->res_id[j] == done->resource_handle)
				continue;

			for (k = 0; k < req_isp->num_fence_map_out; k++)
				if (comp_grp->res_id[j] ==
					req_isp->fence_map_out[k].resource_handle)
					break;

			if ((k == req_isp->num_fence_map_out) && (j != comp_grp->num_res - 1))
				continue;
			else if (k != req_isp->num_fence_map_out)
				break;
			else
				not_found = true;
		}
	}

	if (not_found) {
		/*
		 * If not found in current request, it could be
		 * belonging to next request, this can happen if
		 * IRQ delay happens. It is only valid when the
		 * platform doesn't have last consumed address.
		 */
		CAM_WARN(CAM_ISP,
			"BUF_DONE for res %s not found in Req %lld ",
			__cam_isp_resource_handle_id_to_type(
			ctx_isp->isp_device_type,
			done->resource_handle),
			req->request_id);

		done_next_req->hw_type = done->hw_type;
		done_next_req->resource_handle = done->resource_handle;
		done_next_req->comp_group_id = done->comp_group_id;
		goto check_deferred;
	}

	for (i = 0; i < comp_grp->num_res; i++) {
		for (j = 0; j < req_isp->num_fence_map_out; j++) {
			if (comp_grp->res_id[i] ==
				req_isp->fence_map_out[j].resource_handle)
				break;
		}

		if (j == req_isp->num_fence_map_out) {
			/*
			 * If not found in current request, it could be
			 * belonging to an active port with no valid fence
			 * bound to it, we needn't process it.
			 */
			CAM_DBG(CAM_ISP,
				"BUF_DONE for res %s not active in Req %lld ctx %u link: 0x%x",
				__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type,
				comp_grp->res_id[i]),
				req->request_id, ctx->ctx_id, ctx->link_hdl);
			continue;
		}

		if (req_isp->fence_map_out[j].sync_id == -1) {
			handle_type =
				__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type,
				req_isp->fence_map_out[j].resource_handle);

			CAM_WARN(CAM_ISP,
				"Duplicate BUF_DONE for req %lld : i=%d, j=%d, res=%s, ctx %u link: 0x%x",
				req->request_id, i, j, handle_type, ctx->ctx_id, ctx->link_hdl);

			trace_cam_log_event("Duplicate BufDone",
				handle_type, req->request_id, ctx->ctx_id);
			continue;
		}

		/* Get buf handles from packet and retrieve them from presil framework */
		if (cam_presil_mode_enabled()) {
			rc = cam_presil_retrieve_buffers_from_packet(req_isp->hw_update_data.packet,
				ctx->img_iommu_hdl, req_isp->fence_map_out[j].resource_handle);
			if (rc) {
				CAM_ERR(CAM_ISP,
					"Failed to retrieve image buffers req_id:%d ctx_id:%u link: 0x%x bubble detected:%d rc:%d",
					req->request_id, ctx->ctx_id, ctx->link_hdl,
					req_isp->bubble_detected, rc);
				return rc;
			}
		}

		if (!req_isp->bubble_detected) {
			handle_type = __cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type,
				req_isp->fence_map_out[j].resource_handle);
			CAM_DBG(CAM_ISP,
				"Sync with success: req %lld res 0x%x fd 0x%x, ctx %u link: 0x%x port %s",
				req->request_id,
				req_isp->fence_map_out[j].resource_handle,
				req_isp->fence_map_out[j].sync_id,
				ctx->ctx_id, ctx->link_hdl, handle_type);

			cam_smmu_buffer_tracker_buffer_putref(
				req_isp->fence_map_out[j].buffer_tracker);

			rc = cam_sync_signal(req_isp->fence_map_out[j].sync_id,
				CAM_SYNC_STATE_SIGNALED_SUCCESS,
				CAM_SYNC_COMMON_EVENT_SUCCESS);
			if (rc)
				CAM_DBG(CAM_ISP, "Sync failed with rc = %d, ctx %u link: 0x%x",
					 rc, ctx->ctx_id, ctx->link_hdl);
		} else if (!req_isp->bubble_report) {
			CAM_DBG(CAM_ISP,
				"Sync with failure: req %lld res 0x%x fd 0x%x, ctx %u link: 0x%x",
				req->request_id,
				req_isp->fence_map_out[j].resource_handle,
				req_isp->fence_map_out[j].sync_id,
				ctx->ctx_id, ctx->link_hdl);

			cam_smmu_buffer_tracker_buffer_putref(
				req_isp->fence_map_out[j].buffer_tracker);

			rc = cam_sync_signal(req_isp->fence_map_out[j].sync_id,
				CAM_SYNC_STATE_SIGNALED_ERROR,
				CAM_SYNC_ISP_EVENT_BUBBLE);
			if (rc)
				CAM_ERR(CAM_ISP, "Sync failed with rc = %d, ctx %u link: 0x%x",
					rc, ctx->ctx_id, ctx->link_hdl);
		} else {
			/*
			 * Ignore the buffer done if bubble detect is on
			 * Increment the ack number here, and queue the
			 * request back to pending list whenever all the
			 * buffers are done.
			 */
			req_isp->num_acked++;
			CAM_DBG(CAM_ISP,
				"buf done with bubble state %d recovery %d for req %lld, ctx %u link: 0x%x",
				bubble_state,
				req_isp->bubble_report,
				req->request_id,
				ctx->ctx_id, ctx->link_hdl);
			continue;
		}

		CAM_DBG(CAM_ISP, "req %lld, reset sync id 0x%x ctx %u link: 0x%x",
			req->request_id,
			req_isp->fence_map_out[j].sync_id, ctx->ctx_id, ctx->link_hdl);
		if (!rc) {
			req_isp->num_acked++;
			req_isp->fence_map_out[j].sync_id = -1;
		}

		if ((ctx_isp->use_frame_header_ts) &&
			(req_isp->hw_update_data.frame_header_res_id ==
			req_isp->fence_map_out[j].resource_handle))
			__cam_isp_ctx_send_sof_timestamp_frame_header(
				ctx_isp,
				req_isp->hw_update_data.frame_header_cpu_addr,
				req->request_id, CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	}

check_deferred:
	if (req_isp->num_acked > req_isp->num_fence_map_out) {
		/* Should not happen */
		CAM_ERR(CAM_ISP,
			"WARNING: req_id %lld num_acked %d > map_out %d, ctx %u link: 0x%x",
			req->request_id, req_isp->num_acked,
			req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
		WARN_ON(req_isp->num_acked > req_isp->num_fence_map_out);
	}

	if (req_isp->num_acked != req_isp->num_fence_map_out)
		return rc;

	rc = __cam_isp_ctx_handle_buf_done_for_req_list(ctx_isp, req);
	return rc;
}

static int __cam_isp_handle_deferred_buf_done(
	struct cam_isp_context *ctx_isp,
	struct cam_ctx_request  *req,
	bool bubble_handling,
	uint32_t status, uint32_t event_cause)
{
	int i, j;
	int rc = 0;
	struct cam_isp_ctx_req *req_isp =
		(struct cam_isp_ctx_req *) req->req_priv;
	struct cam_context *ctx = ctx_isp->base;

	CAM_DBG(CAM_ISP,
		"ctx[%u] link[0x%x] : Req %llu : Handling %d deferred buf_dones num_acked=%d, bubble_handling=%d",
		ctx->ctx_id, ctx->link_hdl, req->request_id, req_isp->num_deferred_acks,
		req_isp->num_acked, bubble_handling);

	for (i = 0; i < req_isp->num_deferred_acks; i++) {
		j = req_isp->deferred_fence_map_index[i];

		CAM_DBG(CAM_ISP,
			"ctx[%u] link[0x%x] : Sync with status=%d, event_cause=%d: req %lld res 0x%x sync_id 0x%x",
			ctx->ctx_id, ctx->link_hdl, status, event_cause,
			req->request_id,
			req_isp->fence_map_out[j].resource_handle,
			req_isp->fence_map_out[j].sync_id);

		if (req_isp->fence_map_out[j].sync_id == -1) {
			CAM_WARN(CAM_ISP,
				"ctx[%u] link[0x%x] :  Deferred buf_done already signalled, req_id=%llu, j=%d, res=0x%x",
				ctx->ctx_id, ctx->link_hdl, req->request_id, j,
				req_isp->fence_map_out[j].resource_handle);
			continue;
		}

		if (!bubble_handling) {
#ifdef OPLUS_FEATURE_CAMERA_COMMON
                        CAM_WARN_RATE_LIMIT(CAM_ISP,
                                "Unexpected Buf done for res=0x%x on ctx[%u] link[0x%x] for Req %llu, status=%d, possible bh delays",
                                req_isp->fence_map_out[j].resource_handle, ctx->ctx_id,
                                ctx->link_hdl, req->request_id, status);
#else
			CAM_WARN(CAM_ISP,
				"Unexpected Buf done for res=0x%x on ctx[%u] link[0x%x] for Req %llu, status=%d, possible bh delays",
				req_isp->fence_map_out[j].resource_handle, ctx->ctx_id,
				ctx->link_hdl, req->request_id, status);
#endif

			rc = cam_sync_signal(req_isp->fence_map_out[j].sync_id,
				status, event_cause);
			if (rc) {
				CAM_ERR(CAM_ISP,
					"ctx[%u] link[0x%x] : Sync signal for Req %llu, sync_id %d status=%d failed with rc = %d",
					ctx->ctx_id, ctx->link_hdl, req->request_id,
					req_isp->fence_map_out[j].sync_id,
					status, rc);
			} else {
				req_isp->num_acked++;
				req_isp->fence_map_out[j].sync_id = -1;
			}
		} else {
			req_isp->num_acked++;
		}
	}

	CAM_DBG(CAM_ISP,
		"ctx[%u] link[0x%x] : Req %llu : Handled %d deferred buf_dones num_acked=%d, num_fence_map_out=%d",
		ctx->ctx_id, ctx->link_hdl, req->request_id, req_isp->num_deferred_acks,
		req_isp->num_acked, req_isp->num_fence_map_out);

	req_isp->num_deferred_acks = 0;

	return rc;
}

static int __cam_isp_ctx_handle_deferred_buf_done_in_bubble(
	struct cam_isp_context *ctx_isp,
	struct cam_ctx_request  *req)
{
	int                     rc = 0;
	struct cam_context     *ctx = ctx_isp->base;
	struct cam_isp_ctx_req *req_isp;

	req_isp = (struct cam_isp_ctx_req *)req->req_priv;

	if (req_isp->num_deferred_acks)
		rc = __cam_isp_handle_deferred_buf_done(ctx_isp, req,
			req_isp->bubble_report,
			CAM_SYNC_STATE_SIGNALED_ERROR,
			CAM_SYNC_ISP_EVENT_BUBBLE);

	if (req_isp->num_acked > req_isp->num_fence_map_out) {
		/* Should not happen */
		CAM_ERR(CAM_ISP,
			"WARNING: req_id %lld num_acked %d > map_out %d, ctx %u, link[0x%x]",
			req->request_id, req_isp->num_acked,
			req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
		WARN_ON(req_isp->num_acked > req_isp->num_fence_map_out);
	}

	if (req_isp->num_acked == req_isp->num_fence_map_out)
		rc = __cam_isp_ctx_handle_buf_done_for_req_list(ctx_isp, req);

	return rc;
}

static int __cam_isp_ctx_handle_buf_done_for_request_verify_addr(
	struct cam_isp_context *ctx_isp,
	struct cam_ctx_request  *req,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state,
	bool verify_consumed_addr,
	bool defer_buf_done)
{
	int rc = 0;
	int i, j, k, def_idx;
	bool not_found = false;
	bool duplicate_defer_buf_done = false;
	struct cam_isp_ctx_req *req_isp;
	struct cam_context *ctx = ctx_isp->base;
	const char *handle_type;
	uint32_t cmp_addr = 0;
	struct cam_isp_hw_done_event_data   unhandled_done = {0};
	struct cam_isp_context_comp_record *comp_grp = NULL;
	struct cam_hw_cmd_args   hw_cmd_args;
	struct cam_isp_hw_cmd_args  isp_hw_cmd_args;

	trace_cam_buf_done("ISP", ctx, req);

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	CAM_DBG(CAM_ISP, "Enter with bubble_state %d, req_bubble_detected %d, ctx %u, link[0x%x]",
		bubble_state, req_isp->bubble_detected, ctx->ctx_id, ctx->link_hdl);

	unhandled_done.timestamp = done->timestamp;

	for (i = 0; i < req_isp->num_fence_map_out; i++) {
		if (done->resource_handle ==
			req_isp->fence_map_out[i].resource_handle) {
			cmp_addr = cam_smmu_is_expanded_memory() ? CAM_36BIT_INTF_GET_IOVA_BASE(
				req_isp->fence_map_out[i].image_buf_addr[0]) :
				req_isp->fence_map_out[i].image_buf_addr[0];
			if (!verify_consumed_addr ||
				(verify_consumed_addr && (done->last_consumed_addr == cmp_addr))) {
				break;
			}
		}
	}
	CAM_DBG(CAM_ISP, "finish the addr validation");

	if (done->hw_type == CAM_ISP_HW_TYPE_SFE)
		comp_grp = &ctx_isp->sfe_bus_comp_grp[done->comp_group_id];
	else
		comp_grp = &ctx_isp->vfe_bus_comp_grp[done->comp_group_id];

	if (!comp_grp) {
		CAM_ERR(CAM_ISP, "comp_grp is NULL for hw_type: %d", done->hw_type);
		rc = -EINVAL;
		return rc;
	}

	if (i == req_isp->num_fence_map_out) {
		not_found = true;
		for (j = 0; j < comp_grp->num_res; j++) {
			/* If the res is same with original res, we don't need to read again  */
			if (comp_grp->res_id[j] == done->resource_handle)
				continue;

			/* Check if the res in the requested list */
			for (k = 0; k < req_isp->num_fence_map_out; k++)
				if (comp_grp->res_id[j] ==
					req_isp->fence_map_out[k].resource_handle)
					break;

			/* If res_id[j] isn't in requested list, then try next res in the group */
			if (k == req_isp->num_fence_map_out) {
				if (j != comp_grp->num_res - 1)
					continue;
				else
					break;
			}

			if (!verify_consumed_addr) {
				not_found = false;
				break;
			}

			/*
			 * Find out the res from the requested list,
			 * then we can get last consumed address from this port.
			 */
			done->resource_handle = comp_grp->res_id[j];
			done->last_consumed_addr = 0;

			hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
			hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
			isp_hw_cmd_args.cmd_type =
				CAM_ISP_HW_MGR_GET_LAST_CONSUMED_ADDR;
			isp_hw_cmd_args.cmd_data = done;
			hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
			rc = ctx->hw_mgr_intf->hw_cmd(
				ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
			if (rc) {
				CAM_ERR(CAM_ISP, "HW command failed, ctx %u, link: 0x%x",
					ctx->ctx_id, ctx->link_hdl);
				return rc;
			}

			cmp_addr = cam_smmu_is_expanded_memory() ?
				CAM_36BIT_INTF_GET_IOVA_BASE(
				req_isp->fence_map_out[k].image_buf_addr[0]) :
				req_isp->fence_map_out[k].image_buf_addr[0];
			CAM_DBG(CAM_ISP,
				"Get res %s comp_grp_rec_idx:%d fence_map_idx:%d last_consumed_addr:0x%x cmp_addr:0x%x",
				__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type, done->resource_handle), j, k,
				done->last_consumed_addr, cmp_addr);
			if (done->last_consumed_addr == cmp_addr) {
				CAM_DBG(CAM_ISP, "Consumed addr compare success for res:%s ",
					__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type, done->resource_handle));
				not_found = false;
				break;
			}
		}
	}

	if (not_found) {
		/*
		 * If not found in current request, it could be
		 * belonging to next request, this can happen if
		 * IRQ delay happens. It is only valid when the
		 * platform doesn't have last consumed address.
		 */
		CAM_WARN(CAM_ISP,
			"BUF_DONE for res %s last_consumed_addr:0x%x not found in Req %lld ",
			__cam_isp_resource_handle_id_to_type(
			ctx_isp->isp_device_type, done->resource_handle),
			done->last_consumed_addr,
			req->request_id);

		unhandled_done.hw_type = done->hw_type;
		unhandled_done.resource_handle = done->resource_handle;
		unhandled_done.comp_group_id = done->comp_group_id;
		unhandled_done.last_consumed_addr = done->last_consumed_addr;
		goto check_deferred;
	}

	if (done->hw_type == CAM_ISP_HW_TYPE_SFE)
		comp_grp = &ctx_isp->sfe_bus_comp_grp[done->comp_group_id];
	else
		comp_grp = &ctx_isp->vfe_bus_comp_grp[done->comp_group_id];

	if (!comp_grp) {
		CAM_ERR(CAM_ISP, "comp_grp is NULL");
		rc = -EINVAL;
		return rc;
	}
	CAM_DBG(CAM_ISP, "selected the compare group");

	for (i = 0; i < comp_grp->num_res; i++) {
		for (j = 0; j < req_isp->num_fence_map_out; j++) {
			if (comp_grp->res_id[i] ==
				req_isp->fence_map_out[j].resource_handle)
				break;
		}

		if (j == req_isp->num_fence_map_out) {
			/*
			 * If not found in current request, it could be
			 * belonging to an active port with no valid fence
			 * bound to it, we needn't process it.
			 */
			CAM_DBG(CAM_ISP,
				"BUF_DONE for res %s not active in Req %lld ctx %u, link[0x%x]",
				__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type, comp_grp->res_id[i]),
				req->request_id, ctx->ctx_id, ctx->link_hdl);
			continue;
		}

		if (req_isp->fence_map_out[j].sync_id == -1) {
			handle_type = __cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type,
				req_isp->fence_map_out[j].resource_handle);

#ifdef OPLUS_FEATURE_CAMERA_COMMON
			CAM_DBG(CAM_ISP,
				"Duplicate BUF_DONE for req %lld : i=%d, j=%d, res=%s, ctx %u, link[0x%x]",
				req->request_id, i, j, handle_type, ctx->ctx_id, ctx->link_hdl);
#else
			CAM_WARN(CAM_ISP,
				"Duplicate BUF_DONE for req %lld : i=%d, j=%d, res=%s, ctx %u, link[0x%x]",
				req->request_id, i, j, handle_type, ctx->ctx_id, ctx->link_hdl);
#endif

			trace_cam_log_event("Duplicate BufDone",
				handle_type, req->request_id, ctx->ctx_id);
			continue;
		}

		/* Get buf handles from packet and retrieve them from presil framework */
		if (cam_presil_mode_enabled()) {
			rc = cam_presil_retrieve_buffers_from_packet(req_isp->hw_update_data.packet,
				ctx->img_iommu_hdl, req_isp->fence_map_out[j].resource_handle);
			if (rc) {
				CAM_ERR(CAM_ISP,
					"Failed to retrieve image buffers req_id:%d ctx_id:%u link[0x%x] bubble detected:%d rc:%d",
					req->request_id, ctx->ctx_id, ctx->link_hdl,
					req_isp->bubble_detected, rc);
				return rc;
			}
		}

		if (defer_buf_done) {
			uint32_t deferred_indx = req_isp->num_deferred_acks;
			duplicate_defer_buf_done = false;

			CAM_DBG(CAM_ISP,
				"ctx[%u] link[0x%x]:Deferred info:num_acks=%d,fence_map_index=%d,resource_handle=0x%x,sync_id=%d,num_fence_map_out=%d,req=%lld",
				ctx->ctx_id, ctx->link_hdl, req_isp->num_deferred_acks, j,
				req_isp->fence_map_out[j].resource_handle,
				req_isp->fence_map_out[j].sync_id,
				req_isp->num_fence_map_out,
				req->request_id);

			if( req_isp->num_deferred_acks >= CAM_ISP_CTX_RES_MAX)
			{
				CAM_DBG(CAM_ISP, "number of defferred acks exceeds the max hw resource ctx[%u] link[0x%x] req %lld :num_acks %d sync_id %d",
					ctx->ctx_id, ctx->link_hdl, req->request_id,
					req_isp->num_deferred_acks, req_isp->fence_map_out[j].sync_id);
				rc = -EINVAL;
				return rc;
			}

			for (k = 0; k < req_isp->num_deferred_acks; k++) {
				def_idx = req_isp->deferred_fence_map_index[k];
				if (def_idx == j) {
					CAM_WARN(CAM_ISP,
						"duplicate deferred ack for ctx[%u] link[0x%x] req %lld res 0x%x sync_id 0x%x",
						ctx->ctx_id, ctx->link_hdl,
						req->request_id,
						req_isp->fence_map_out[j].resource_handle,
						req_isp->fence_map_out[j].sync_id);
					duplicate_defer_buf_done = true;
					break;
				}
			}

			if (duplicate_defer_buf_done)
				continue;

			if (req_isp->num_deferred_acks == req_isp->num_fence_map_out) {
				CAM_WARN(CAM_ISP,
					"WARNING: req_id %lld num_deferred_acks %d > map_out %d, ctx_idx:%u link[0x%x]",
					req->request_id, req_isp->num_deferred_acks,
					req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
				continue;
			}

			/*
			 * If we are handling this BUF_DONE event for a request
			 * that is still in wait_list, do not signal now,
			 * instead mark it as done and handle it later -
			 * if this request is going into BUBBLE state later
			 * it will automatically be re-applied. If this is not
			 * going into BUBBLE, signal fences later.
			 * Note - we will come here only if the last consumed
			 * address matches with this ports buffer.
			 */
			req_isp->deferred_fence_map_index[deferred_indx] = j;
			req_isp->num_deferred_acks++;
			CAM_DBG(CAM_ISP,
				"ctx[%u] link[0x%x]:Deferred buf done for %llu with bubble state %d recovery %d",
				ctx->ctx_id, ctx->link_hdl, req->request_id, bubble_state,
				req_isp->bubble_report);
			CAM_DBG(CAM_ISP,
				"ctx[%u] link[0x%x]:Deferred info:num_acks=%d,fence_map_index=%d,resource_handle=0x%x,sync_id=%d",
				ctx->ctx_id, ctx->link_hdl, req_isp->num_deferred_acks, j,
				req_isp->fence_map_out[j].resource_handle,
				req_isp->fence_map_out[j].sync_id);
			continue;
		} else if (!req_isp->bubble_detected) {
			CAM_DBG(CAM_ISP,
				"Sync with success: req %lld res 0x%x fd 0x%x, ctx %u res %s",
				req->request_id,
				req_isp->fence_map_out[j].resource_handle,
				req_isp->fence_map_out[j].sync_id,
				ctx->ctx_id,
				__cam_isp_resource_handle_id_to_type(ctx_isp->isp_device_type,
				req_isp->fence_map_out[j].resource_handle));

			cam_smmu_buffer_tracker_buffer_putref(
				req_isp->fence_map_out[j].buffer_tracker);

			rc = cam_sync_signal(req_isp->fence_map_out[j].sync_id,
				CAM_SYNC_STATE_SIGNALED_SUCCESS,
				CAM_SYNC_COMMON_EVENT_SUCCESS);
			if (rc) {
				CAM_ERR(CAM_ISP,
					"Sync=%u for req=%llu failed with rc=%d ctx:%u link[0x%x]",
					req_isp->fence_map_out[j].sync_id, req->request_id,
					rc, ctx->ctx_id, ctx->link_hdl);
			} else if (req_isp->num_deferred_acks) {
				/* Process deferred buf_done acks */
				__cam_isp_handle_deferred_buf_done(ctx_isp,
					req, false,
					CAM_SYNC_STATE_SIGNALED_SUCCESS,
					CAM_SYNC_COMMON_EVENT_SUCCESS);
			}
			/* Reset fence */
			req_isp->fence_map_out[j].sync_id = -1;
		} else if (!req_isp->bubble_report) {

			CAM_DBG(CAM_ISP,
				"Sync with failure: req %lld res 0x%x fd 0x%x, ctx:%u link[0x%x]",
				req->request_id,
				req_isp->fence_map_out[j].resource_handle,
				req_isp->fence_map_out[j].sync_id,
				ctx->ctx_id, ctx->link_hdl);

			cam_smmu_buffer_tracker_buffer_putref(
				req_isp->fence_map_out[j].buffer_tracker);

			rc = cam_sync_signal(req_isp->fence_map_out[j].sync_id,
				CAM_SYNC_STATE_SIGNALED_ERROR,
				CAM_SYNC_ISP_EVENT_BUBBLE);
			if (rc) {
				CAM_ERR(CAM_ISP,
					"Sync:%u for req:%llu failed with rc:%d,ctx:%u,link[0x%x]",
					req_isp->fence_map_out[j].sync_id, req->request_id,
					rc, ctx->ctx_id, ctx->link_hdl);
			} else if (req_isp->num_deferred_acks) {
				/* Process deferred buf_done acks */
				__cam_isp_handle_deferred_buf_done(ctx_isp, req,
					false,
					CAM_SYNC_STATE_SIGNALED_ERROR,
					CAM_SYNC_ISP_EVENT_BUBBLE);
			}
			/* Reset fence */
			req_isp->fence_map_out[j].sync_id = -1;
		} else {
			/*
			 * Ignore the buffer done if bubble detect is on
			 * Increment the ack number here, and queue the
			 * request back to pending list whenever all the
			 * buffers are done.
			 */
			req_isp->num_acked++;
			CAM_DBG(CAM_ISP,
				"buf done with bubble state %d recovery %d for req %lld, ctx_idx:%u link[0x%x]",
				bubble_state,
				req_isp->bubble_report,
				req->request_id,
				ctx->ctx_id, ctx->link_hdl);

			/* Process deferred buf_done acks */
			if (req_isp->num_deferred_acks)
				__cam_isp_handle_deferred_buf_done(ctx_isp, req,
					true,
					CAM_SYNC_STATE_SIGNALED_ERROR,
					CAM_SYNC_ISP_EVENT_BUBBLE);

			if (req_isp->num_acked == req_isp->num_fence_map_out) {
				rc = __cam_isp_ctx_handle_buf_done_for_req_list(ctx_isp, req);
				if (rc)
					CAM_ERR(CAM_ISP,
						"Error in buf done for req = %llu with rc = %d, ctx_idx:%u link[0x%x]",
						req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
				return rc;
			}
			continue;
		}

		CAM_DBG(CAM_ISP, "req %lld, reset sync id 0x%x ctx_idx:%u link[0x%x]",
			req->request_id,
			req_isp->fence_map_out[j].sync_id, ctx->ctx_id, ctx->link_hdl);
		if (!rc) {
			req_isp->num_acked++;
		}

		if ((ctx_isp->use_frame_header_ts) &&
			(req_isp->hw_update_data.frame_header_res_id ==
			req_isp->fence_map_out[j].resource_handle))
			__cam_isp_ctx_send_sof_timestamp_frame_header(
				ctx_isp,
				req_isp->hw_update_data.frame_header_cpu_addr,
				req->request_id, CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	}

check_deferred:
	CAM_DBG(CAM_ISP, "start check_deferred from here");

	if ((unhandled_done.resource_handle > 0) && (!defer_buf_done))
		__cam_isp_ctx_check_deferred_buf_done(
			ctx_isp, &unhandled_done, bubble_state);

	if (req_isp->num_acked > req_isp->num_fence_map_out) {
		/* Should not happen */
		CAM_ERR(CAM_ISP,
			"WARNING: req_id %lld num_acked %d > map_out %d, ctx_idx:%u link[0x%x]",
			req->request_id, req_isp->num_acked,
			req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
	}
	CAM_DBG(CAM_ISP, "finish check_deferred");

	if (req_isp->num_acked != req_isp->num_fence_map_out)
		return rc;

	rc = __cam_isp_ctx_handle_buf_done_for_req_list(ctx_isp, req);
	CAM_DBG(CAM_ISP, "handled the buf done for req list");

	return rc;
}

static int __cam_isp_ctx_handle_buf_done(
	struct cam_isp_context *ctx_isp,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state)
{
	int rc = 0;
	struct cam_ctx_request *req;
	struct cam_context *ctx = ctx_isp->base;
	struct cam_isp_hw_done_event_data done_next_req = {0};

	if (list_empty(&ctx->active_req_list)) {
		CAM_WARN(CAM_ISP, "Buf done with no active request, ctx_idx:%u link[0x%x]",
			ctx->ctx_id, ctx->link_hdl);
		return 0;
	}

	req = list_first_entry(&ctx->active_req_list,
			struct cam_ctx_request, list);

	rc = __cam_isp_ctx_handle_buf_done_for_request(ctx_isp, req, done,
		bubble_state, &done_next_req);

	if (done_next_req.resource_handle) {
		struct cam_isp_hw_done_event_data unhandled_res = {0};
		struct cam_ctx_request  *next_req = list_last_entry(
			&ctx->active_req_list, struct cam_ctx_request, list);

		if (next_req->request_id != req->request_id) {
			/*
			 * Few resource handles are already signalled in the
			 * current request, lets check if there is another
			 * request waiting for these resources. This can
			 * happen if handling some of next request's buf done
			 * events are happening first before handling current
			 * request's remaining buf dones due to IRQ scheduling.
			 * Lets check only one more request as we will have
			 * maximum of 2 requests in active_list at any time.
			 */

			CAM_WARN(CAM_ISP,
				"Unhandled bufdone resources for req %lld,trying next request %lld,ctx:%u link[0x%x]",
				req->request_id, next_req->request_id, ctx->ctx_id, ctx->link_hdl);

			__cam_isp_ctx_handle_buf_done_for_request(ctx_isp,
				next_req, &done_next_req,
				bubble_state, &unhandled_res);

			if (unhandled_res.resource_handle == 0)
				CAM_INFO(CAM_ISP,
					"BUF Done event handed for next request %lld, ctx_idx:%u link[0x%x]",
					next_req->request_id, ctx->ctx_id, ctx->link_hdl);
			else
				CAM_ERR(CAM_ISP,
					"BUF Done not handled for next request %lld, ctx_idx:%u link[0x%x]",
					next_req->request_id, ctx->ctx_id, ctx->link_hdl);
		} else {
			CAM_WARN(CAM_ISP,
				"Req %lld only active request, spurious buf_done rxd, ctx_idx:%u link[0x%x]",
				req->request_id, ctx->ctx_id, ctx->link_hdl);
		}
	}

	return rc;
}

static void __cam_isp_ctx_buf_done_match_req(
	struct cam_isp_context *ctx_isp,
	struct cam_ctx_request *req,
	struct cam_isp_hw_done_event_data *done,
	bool *irq_delay_detected)
{
	int rc = 0;
	int i, j, k;
	uint32_t match_count = 0;
	struct cam_isp_ctx_req *req_isp;
	uint32_t cmp_addr = 0;
	struct cam_isp_context_comp_record *comp_grp = NULL;
	struct cam_hw_cmd_args hw_cmd_args;
	struct cam_isp_hw_cmd_args isp_hw_cmd_args;
	struct cam_context *ctx = ctx_isp->base;

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	if (done->hw_type == CAM_ISP_HW_TYPE_SFE)
		comp_grp = &ctx_isp->sfe_bus_comp_grp[done->comp_group_id];
	else
		comp_grp = &ctx_isp->vfe_bus_comp_grp[done->comp_group_id];

	CAM_DBG(CAM_ISP, "Done Comp Group: %d Res %s last_consumed_addr:0x%x",
		done->comp_group_id,
		__cam_isp_resource_handle_id_to_type(
			ctx_isp->isp_device_type, done->resource_handle),
		done->last_consumed_addr);

	for (i = 0; i < req_isp->num_fence_map_out; i++) {
		cmp_addr = cam_smmu_is_expanded_memory() ? CAM_36BIT_INTF_GET_IOVA_BASE(
			req_isp->fence_map_out[i].image_buf_addr[0]) :
			req_isp->fence_map_out[i].image_buf_addr[0];
		if ((done->resource_handle ==
			 req_isp->fence_map_out[i].resource_handle) &&
			(done->last_consumed_addr == cmp_addr)) {
			match_count++;
			CAM_DBG(CAM_ISP, "Consumed addr compare success for res:%s ",
				__cam_isp_resource_handle_id_to_type(
					ctx_isp->isp_device_type, done->resource_handle));
			break;
		}

	}

	if (i == req_isp->num_fence_map_out) {
		for (j = 0; j < comp_grp->num_res; j++) {
			/* If the res is same with original res, we don't need to read again  */
			if (comp_grp->res_id[j] == done->resource_handle)
				continue;

			/* Check if the res in the requested list */
			for (k = 0; k < req_isp->num_fence_map_out; k++)
				if (comp_grp->res_id[j] ==
					req_isp->fence_map_out[k].resource_handle)
					break;

			/* If res_id[j] isn't in requested list, then try next res in the group */
			if (k == req_isp->num_fence_map_out) {
				if (j != comp_grp->num_res - 1)
					continue;
				else {
					CAM_ERR(CAM_ISP,
						"not in this group and exit ctx %u link: 0x%x",
						ctx->ctx_id, ctx->link_hdl);
					break;
				}
			}

			/*
			 * Find out the res from the requested list,
			 * then we can get last consumed address from this port.
			 */
			done->resource_handle = comp_grp->res_id[j];
			done->last_consumed_addr = 0;

			hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
			hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
			isp_hw_cmd_args.cmd_type =
				CAM_ISP_HW_MGR_GET_LAST_CONSUMED_ADDR;
			isp_hw_cmd_args.cmd_data = done;
			hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
			rc = ctx->hw_mgr_intf->hw_cmd(
				ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
			if (rc) {
				CAM_ERR(CAM_ISP, "HW command failed, ctx %u, link: 0x%x",
					ctx->ctx_id, ctx->link_hdl);
			}

			cmp_addr = cam_smmu_is_expanded_memory() ?
				CAM_36BIT_INTF_GET_IOVA_BASE(
				req_isp->fence_map_out[k].image_buf_addr[0]) :
				req_isp->fence_map_out[k].image_buf_addr[0];
			CAM_DBG(CAM_ISP,
				"Get res %s comp_grp_rec_idx:%d fence_map_idx:%d last_consumed_addr:0x%x, cmp_addr:0x%x",
				__cam_isp_resource_handle_id_to_type(
					ctx_isp->isp_device_type, done->resource_handle), j, k,
				done->last_consumed_addr, cmp_addr);
			if (done->last_consumed_addr == cmp_addr) {
				CAM_DBG(CAM_ISP, "Consumed addr compare success for res:%s ",
					__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type, done->resource_handle));
				match_count++;
				break;
			}
		}
	}

	if (match_count > 0)
		*irq_delay_detected = true;
	else
		*irq_delay_detected = false;

	CAM_DBG(CAM_ISP,
		"buf done num handles %d [%s] match count %d for next req: %lld ctx: %u, link: 0x%x",
		done->resource_handle,
		__cam_isp_resource_handle_id_to_type(
				ctx_isp->isp_device_type, done->resource_handle),
		match_count, req->request_id, ctx->ctx_id, ctx->link_hdl);
	CAM_DBG(CAM_ISP,
		"irq_delay_detected %d", *irq_delay_detected);
}

static void __cam_isp_ctx_try_buf_done_process_for_active_request(
	uint32_t deferred_ack_start_idx, struct cam_isp_context *ctx_isp,
	struct cam_ctx_request *deferred_req)
{
	int i, j, deferred_map_idx, rc;
	struct cam_context *ctx = ctx_isp->base;
	struct cam_ctx_request *curr_active_req;
	struct cam_isp_ctx_req *curr_active_isp_req;
	struct cam_isp_ctx_req *deferred_isp_req;

	if (list_empty(&ctx->active_req_list))
		return;

	curr_active_req = list_first_entry(&ctx->active_req_list,
		struct cam_ctx_request, list);
	curr_active_isp_req = (struct cam_isp_ctx_req *)curr_active_req->req_priv;
	deferred_isp_req = (struct cam_isp_ctx_req *)deferred_req->req_priv;

	/* Check from newly updated deferred acks */
	for (i = deferred_ack_start_idx; i < deferred_isp_req->num_deferred_acks; i++) {
		deferred_map_idx = deferred_isp_req->deferred_fence_map_index[i];

		for (j = 0; j < curr_active_isp_req->num_fence_map_out; j++) {
			/* resource needs to match */
			if (curr_active_isp_req->fence_map_out[j].resource_handle !=
				deferred_isp_req->fence_map_out[deferred_map_idx].resource_handle)
				continue;

			/* Check if fence is valid */
			if (curr_active_isp_req->fence_map_out[j].sync_id == -1)
				break;

			CAM_WARN(CAM_ISP,
				"Processing delayed buf done req: %llu bubble_detected: %s res: 0x%x fd: 0x%x, ctx: %u link: 0x%x [deferred req: %llu last applied: %llu]",
				curr_active_req->request_id,
				CAM_BOOL_TO_YESNO(curr_active_isp_req->bubble_detected),
				curr_active_isp_req->fence_map_out[j].resource_handle,
				curr_active_isp_req->fence_map_out[j].sync_id,
				ctx->ctx_id, ctx->link_hdl,
				deferred_req->request_id, ctx_isp->last_applied_req_id);

			/* Signal only if bubble is not detected for this request */
			if (!curr_active_isp_req->bubble_detected) {
				rc = cam_sync_signal(curr_active_isp_req->fence_map_out[j].sync_id,
					CAM_SYNC_STATE_SIGNALED_SUCCESS,
					CAM_SYNC_COMMON_EVENT_SUCCESS);
				if (rc)
					CAM_ERR(CAM_ISP,
						"Sync: %d for req: %llu failed with rc: %d, ctx: %u link: 0x%x",
						curr_active_isp_req->fence_map_out[j].sync_id,
						curr_active_req->request_id, rc,
						ctx->ctx_id, ctx->link_hdl);

				curr_active_isp_req->fence_map_out[j].sync_id = -1;
			}

			curr_active_isp_req->num_acked++;
			break;
		}
	}
}

static int __cam_isp_ctx_check_deferred_buf_done(
	struct cam_isp_context *ctx_isp,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state)
{
	int rc = 0;
	uint32_t curr_num_deferred = 0;
	struct cam_ctx_request *req;
	struct cam_context *ctx = ctx_isp->base;
	struct cam_isp_ctx_req *req_isp;
	bool  req_in_pending_wait_list = false;

	if (!list_empty(&ctx->wait_req_list)) {
		req = list_first_entry(&ctx->wait_req_list,
			struct cam_ctx_request, list);

		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		curr_num_deferred = req_isp->num_deferred_acks;

		req_in_pending_wait_list = true;
		if (ctx_isp->last_applied_req_id !=
			ctx_isp->last_bufdone_err_apply_req_id) {
			CAM_DBG(CAM_ISP,
				"Trying to find buf done with req in wait list, req %llu last apply id:%lld last err id:%lld curr_num_deferred: %u, ctx: %u link: 0x%x",
				req->request_id, ctx_isp->last_applied_req_id,
				ctx_isp->last_bufdone_err_apply_req_id, curr_num_deferred,
				ctx->ctx_id, ctx->link_hdl);
			ctx_isp->last_bufdone_err_apply_req_id =
				ctx_isp->last_applied_req_id;
		}

		/*
		 * Verify consumed address for this request to make sure
		 * we are handling the buf_done for the correct
		 * buffer. Also defer actual buf_done handling, i.e
		 * do not signal the fence as this request may go into
		 * Bubble state eventully.
		 */
		rc = __cam_isp_ctx_handle_buf_done_for_request_verify_addr(
			ctx_isp, req, done, bubble_state, true, true);

		/* Check for active req if any deferred is processed */
		if (req_isp->num_deferred_acks > curr_num_deferred)
			__cam_isp_ctx_try_buf_done_process_for_active_request(
				curr_num_deferred, ctx_isp, req);
	} else if (!list_empty(&ctx->pending_req_list)) {
		/*
		 * We saw the case that the hw config is blocked due to
		 * some reason, the we get the reg upd and buf done before
		 * the req is added to wait req list.
		 */
		req = list_first_entry(&ctx->pending_req_list,
			struct cam_ctx_request, list);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		curr_num_deferred = req_isp->num_deferred_acks;

		req_in_pending_wait_list = true;
		if (ctx_isp->last_applied_req_id !=
			ctx_isp->last_bufdone_err_apply_req_id) {
			CAM_DBG(CAM_ISP,
				"Trying to find buf done with req in pending list, req %llu last apply id:%lld last err id:%lld curr_num_deferred: %u, ctx: %u link: 0x%x",
				req->request_id, ctx_isp->last_applied_req_id,
				ctx_isp->last_bufdone_err_apply_req_id, curr_num_deferred,
				ctx->ctx_id, ctx->link_hdl);
			ctx_isp->last_bufdone_err_apply_req_id =
				ctx_isp->last_applied_req_id;
		}

		/*
		 * Verify consumed address for this request to make sure
		 * we are handling the buf_done for the correct
		 * buffer. Also defer actual buf_done handling, i.e
		 * do not signal the fence as this request may go into
		 * Bubble state eventully.
		 */
		rc = __cam_isp_ctx_handle_buf_done_for_request_verify_addr(
			ctx_isp, req, done, bubble_state, true, true);

		/* Check for active req if any deferred is processed */
		if (req_isp->num_deferred_acks > curr_num_deferred)
			__cam_isp_ctx_try_buf_done_process_for_active_request(
				curr_num_deferred, ctx_isp, req);
	}

	if (!req_in_pending_wait_list  && (ctx_isp->last_applied_req_id !=
		ctx_isp->last_bufdone_err_apply_req_id)) {
		CAM_DBG(CAM_ISP,
			"Bufdone without active request bubble_state=%d last_applied_req_id:%lld,ctx:%u link:0x%x",
			bubble_state, ctx_isp->last_applied_req_id, ctx->ctx_id, ctx->link_hdl);
		ctx_isp->last_bufdone_err_apply_req_id =
				ctx_isp->last_applied_req_id;
	}

	return rc;
}

static int __cam_isp_ctx_handle_buf_done_verify_addr(
	struct cam_isp_context *ctx_isp,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state)
{
	int rc = 0;
	bool irq_delay_detected = false;
	struct cam_ctx_request *req;
	struct cam_ctx_request *next_req = NULL;
	struct cam_context *ctx = ctx_isp->base;

	if (list_empty(&ctx->active_req_list)) {
		return __cam_isp_ctx_check_deferred_buf_done(
			ctx_isp, done, bubble_state);
	}

	req = list_first_entry(&ctx->active_req_list,
			struct cam_ctx_request, list);

	if (ctx_isp->active_req_cnt > 1) {
		next_req = list_last_entry(
			&ctx->active_req_list,
			struct cam_ctx_request, list);

		if (next_req->request_id != req->request_id)
			__cam_isp_ctx_buf_done_match_req(
					ctx_isp, next_req, done, &irq_delay_detected);
		else
			CAM_WARN(CAM_ISP,
				"Req %lld only active request, spurious buf_done rxd, ctx: %u link: 0x%x",
				req->request_id, ctx->ctx_id, ctx->link_hdl);
	}

	/*
	 * If irq delay isn't detected, then we need to verify
	 * the consumed address for current req, otherwise, we
	 * can't verify the consumed address.
	 */
	rc = __cam_isp_ctx_handle_buf_done_for_request_verify_addr(
		ctx_isp, req, done, bubble_state,
		!irq_delay_detected, false);

	/*
	 * Verify the consumed address for next req all the time,
	 * since the reported buf done event may belong to current
	 * req, then we can't signal this event for next req.
	 */
	if (!rc && irq_delay_detected)
		rc = __cam_isp_ctx_handle_buf_done_for_request_verify_addr(
			ctx_isp, next_req, done,
			bubble_state, true, false);

	return rc;
}

static int __cam_isp_ctx_handle_buf_done_in_activated_state(
	struct cam_isp_context *ctx_isp,
	struct cam_isp_hw_done_event_data *done,
	uint32_t bubble_state)
{
	int rc = 0;

	if (ctx_isp->support_consumed_addr)
		rc = __cam_isp_ctx_handle_buf_done_verify_addr(
			ctx_isp, done, bubble_state);
	else
		rc = __cam_isp_ctx_handle_buf_done(
			ctx_isp, done, bubble_state);

	return rc;
}

static int __cam_isp_ctx_apply_pending_req(
	void *priv, void *data)
{
	int rc = 0;
	int64_t prev_applied_req;
	struct cam_context *ctx = NULL;
	struct cam_isp_context *ctx_isp = priv;
	struct cam_ctx_request *req;
	struct cam_isp_ctx_req *req_isp;
	struct cam_hw_config_args cfg = {0};

	if (!ctx_isp) {
		CAM_ERR(CAM_ISP, "Invalid ctx_isp:%pK", ctx);
		rc = -EINVAL;
		goto end;
	}
	ctx = ctx_isp->base;

	if (list_empty(&ctx->pending_req_list)) {
		CAM_DBG(CAM_ISP,
			"No pending requests to apply, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	if (ctx_isp->vfps_aux_context) {
		if (ctx_isp->substate_activated == CAM_ISP_CTX_ACTIVATED_APPLIED)
			goto end;

		if (ctx_isp->active_req_cnt >= 1)
			goto end;
	} else {
		if ((ctx->state != CAM_CTX_ACTIVATED) ||
			(!atomic_read(&ctx_isp->rxd_epoch)) ||
			(ctx_isp->substate_activated == CAM_ISP_CTX_ACTIVATED_APPLIED))
			goto end;

		if (ctx_isp->active_req_cnt >= 2)
			goto end;
	}


	spin_lock_bh(&ctx->lock);
	req = list_first_entry(&ctx->pending_req_list, struct cam_ctx_request,
		list);
	spin_unlock_bh(&ctx->lock);

	CAM_DBG(CAM_REQ, "Apply request %lld in substate %d ctx_idx: %u, link: 0x%x",
		req->request_id, ctx_isp->substate_activated, ctx->ctx_id, ctx->link_hdl);
	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	cfg.ctxt_to_hw_map = ctx_isp->hw_ctx;
	cfg.request_id = req->request_id;
	cfg.hw_update_entries = req_isp->cfg;
	cfg.num_hw_update_entries = req_isp->num_cfg;
	cfg.priv = &req_isp->hw_update_data;

	/*
	 * Offline mode may receive the SOF and REG_UPD earlier than
	 * CDM processing return back, so we set the substate before
	 * apply setting.
	 */
	spin_lock_bh(&ctx->lock);

	atomic_set(&ctx_isp->rxd_epoch, 0);
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_APPLIED;
	prev_applied_req = ctx_isp->last_applied_req_id;
	ctx_isp->last_applied_req_id = req->request_id;
	atomic_set(&ctx_isp->apply_in_progress, 1);

	list_del_init(&req->list);
	list_add_tail(&req->list, &ctx->wait_req_list);

	spin_unlock_bh(&ctx->lock);

	rc = ctx->hw_mgr_intf->hw_config(ctx->hw_mgr_intf->hw_mgr_priv, &cfg);
	if (rc) {
		CAM_ERR_RATE_LIMIT(CAM_ISP,
				"Can not apply the configuration,ctx: %u,link: 0x%x",
				ctx->ctx_id, ctx->link_hdl);
		spin_lock_bh(&ctx->lock);

		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;
		ctx_isp->last_applied_req_id = prev_applied_req;
		atomic_set(&ctx_isp->apply_in_progress, 0);

		list_del_init(&req->list);
		list_add(&req->list, &ctx->pending_req_list);

		spin_unlock_bh(&ctx->lock);
	} else {
		atomic_set(&ctx_isp->apply_in_progress, 0);
		CAM_DBG(CAM_ISP, "New substate state %d, applied req %lld, ctx: %u, link: 0x%x",
			CAM_ISP_CTX_ACTIVATED_APPLIED,
			ctx_isp->last_applied_req_id, ctx->ctx_id, ctx->link_hdl);

		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_APPLIED,
			req->request_id);
	}

end:
	return rc;
}

static int __cam_isp_ctx_schedule_apply_req(
	struct cam_isp_context *ctx_isp)
{
	int rc = 0;
	struct crm_workq_task *task;

	task = cam_req_mgr_workq_get_task(ctx_isp->workq);
	if (!task) {
		CAM_ERR(CAM_ISP, "No task for worker");
		return -ENOMEM;
	}

	task->process_cb = __cam_isp_ctx_apply_pending_req;
	rc = cam_req_mgr_workq_enqueue_task(task, ctx_isp, CRM_TASK_PRIORITY_0);
	if (rc)
		CAM_ERR(CAM_ISP, "Failed to schedule task rc:%d", rc);

	return rc;
}

static int __cam_isp_ctx_offline_epoch_in_activated_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	struct cam_context *ctx = ctx_isp->base;
	struct cam_ctx_request *req, *req_temp;
	uint64_t request_id = 0;

	atomic_set(&ctx_isp->rxd_epoch, 1);

	CAM_DBG(CAM_ISP, "SOF frame %lld ctx %u link: 0x%x", ctx_isp->frame_id,
		ctx->ctx_id, ctx->link_hdl);

	/*
	 * For offline it is not possible for epoch to be generated without
	 * RUP done. IRQ scheduling delays can possibly cause this.
	 */
	if (list_empty(&ctx->active_req_list)) {
		CAM_WARN(CAM_ISP,
			"Active list empty on ctx:%u link:0x%x - EPOCH serviced before RUP",
			ctx->ctx_id, ctx->link_hdl);
	} else {
		list_for_each_entry_safe(req, req_temp, &ctx->active_req_list, list) {
			if (req->request_id > ctx_isp->reported_req_id) {
				request_id = req->request_id;
				ctx_isp->reported_req_id = request_id;
				break;
			}
		}
	}

	__cam_isp_ctx_schedule_apply_req(ctx_isp);

	/*
	 * If no valid request, wait for RUP shutter posted after buf done
	 */
	if (request_id)
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);

	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_EPOCH,
		request_id);

	return 0;
}

static int __cam_isp_ctx_reg_upd_in_epoch_bubble_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	if (ctx_isp->frame_id == 1)
		CAM_DBG(CAM_ISP, "Reg update in Substate[%s] for early PCR",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated));
	else
		CAM_WARN_RATE_LIMIT(CAM_ISP,
			"ctx:%u Unexpected regupdate in activated Substate[%s] for frame_id:%lld",
			ctx_isp->base->ctx_id,
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated),
			ctx_isp->frame_id);
	return 0;
}

static int __cam_isp_ctx_reg_upd_in_applied_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_ctx_request  *req;
	struct cam_context      *ctx = ctx_isp->base;
	struct cam_isp_ctx_req  *req_isp;
	uint64_t                 request_id = 0;

	if (list_empty(&ctx->wait_req_list)) {
		CAM_ERR(CAM_ISP, "Reg upd ack with no waiting request, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	req = list_first_entry(&ctx->wait_req_list,
		struct cam_ctx_request, list);
	list_del_init(&req->list);

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;
	if (req_isp->num_fence_map_out != 0) {
		list_add_tail(&req->list, &ctx->active_req_list);
		ctx_isp->active_req_cnt++;
		request_id = req->request_id;
		CAM_DBG(CAM_REQ,
			"move request %lld to active list(cnt = %d), ctx %u, link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
		__cam_isp_ctx_update_event_record(ctx_isp,
			CAM_ISP_CTX_EVENT_RUP, req, NULL);
	} else {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		/* no io config, so the request is completed. */
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
		CAM_DBG(CAM_ISP,
			"move active request %lld to free list(cnt = %d), ctx %u, link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
	}

	/*
	 * This function only called directly from applied and bubble applied
	 * state so change substate here.
	 */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_EPOCH;
	CAM_DBG(CAM_ISP, "next Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_REG_UPDATE, request_id);

end:
	return rc;
}

static int __cam_isp_ctx_notify_sof_in_activated_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	uint64_t  request_id  = 0;
	struct cam_context *ctx = ctx_isp->base;
	struct cam_ctx_request  *req;
	struct cam_isp_ctx_req  *req_isp;
	struct cam_hw_cmd_args   hw_cmd_args;
	struct cam_isp_hw_cmd_args  isp_hw_cmd_args;
	uint64_t last_cdm_done_req = 0;
	struct cam_isp_hw_epoch_event_data *epoch_done_event_data =
			(struct cam_isp_hw_epoch_event_data *)evt_data;
#ifdef OPLUS_FEATURE_CAMERA_COMMON
	char trace[64] = {0};
#endif

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "invalid event data");
		return -EINVAL;
	}

	ctx_isp->frame_id_meta = epoch_done_event_data->frame_id_meta;

	if (atomic_read(&ctx_isp->process_bubble)) {
		if (list_empty(&ctx->active_req_list)) {
			CAM_ERR(CAM_ISP,
				"No available active req in bubble, ctx %u, link: 0x%x",
				ctx->ctx_id, ctx->link_hdl);
			atomic_set(&ctx_isp->process_bubble, 0);
			ctx_isp->bubble_frame_cnt = 0;
			rc = -EINVAL;
			return rc;
		}

		if (ctx_isp->last_sof_timestamp ==
			ctx_isp->sof_timestamp_val) {
			CAM_DBG(CAM_ISP,
				"Tasklet delay detected! Bubble frame check skipped, sof_timestamp: %lld, ctx %u, link: 0x%x",
				ctx_isp->sof_timestamp_val, ctx->ctx_id, ctx->link_hdl);
			goto notify_only;
		}

		req = list_first_entry(&ctx->active_req_list,
			struct cam_ctx_request, list);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;

		if (ctx_isp->bubble_frame_cnt >= 1 &&
			req_isp->bubble_detected) {
			hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
			hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
			isp_hw_cmd_args.cmd_type =
				CAM_ISP_HW_MGR_GET_LAST_CDM_DONE;
			hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
			rc = ctx->hw_mgr_intf->hw_cmd(
				ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
			if (rc) {
				CAM_ERR(CAM_ISP, "HW command failed, ctx %u, link: 0x%x",
					ctx->ctx_id, ctx->link_hdl);
				return rc;
			}

			last_cdm_done_req = isp_hw_cmd_args.u.last_cdm_done;
			CAM_DBG(CAM_ISP, "last_cdm_done req: %d, ctx %u, link: 0x%x",
				last_cdm_done_req, ctx->ctx_id, ctx->link_hdl);

			if (last_cdm_done_req >= req->request_id) {
				CAM_DBG(CAM_ISP,
					"invalid sof event data CDM cb detected for req: %lld, possible buf_done delay, waiting for buf_done, ctx %u, link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				ctx_isp->bubble_frame_cnt = 0;
			} else {
				CAM_DBG(CAM_ISP,
					"CDM callback not happened for req: %lld, possible CDM stuck or workqueue delay, ctx %u, link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				req_isp->num_acked = 0;
				req_isp->num_deferred_acks = 0;
				ctx_isp->bubble_frame_cnt = 0;
				req_isp->bubble_detected = false;
				req_isp->cdm_reset_before_apply = true;
				list_del_init(&req->list);
				list_add(&req->list, &ctx->pending_req_list);
				atomic_set(&ctx_isp->process_bubble, 0);
				ctx_isp->active_req_cnt--;
				CAM_DBG(CAM_REQ,
					"Move active req: %lld to pending list(cnt = %d) [bubble re-apply], ctx %u link: 0x%x",
					req->request_id,
					ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
			}
		} else if (req_isp->bubble_detected) {
			ctx_isp->bubble_frame_cnt++;
			CAM_DBG(CAM_ISP,
				"Waiting on bufdone for bubble req: %lld, since frame_cnt = %lld, ctx %u link: 0x%x",
				req->request_id,
				ctx_isp->bubble_frame_cnt, ctx->ctx_id, ctx->link_hdl);
		} else {
			CAM_DBG(CAM_ISP, "Delayed bufdone for req: %lld, ctx %u link: 0x%x",
				req->request_id, ctx->ctx_id, ctx->link_hdl);
		}
	}

notify_only:
	/*
	 * notify reqmgr with sof signal. Note, due to scheduling delay
	 * we can run into situation that two active requests has already
	 * be in the active queue while we try to do the notification.
	 * In this case, we need to skip the current notification. This
	 * helps the state machine to catch up the delay.
	 */
	if (ctx_isp->active_req_cnt <= 2) {
		__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);

		list_for_each_entry(req, &ctx->active_req_list, list) {
			req_isp = (struct cam_isp_ctx_req *) req->req_priv;
			if ((!req_isp->bubble_detected) &&
				(req->request_id > ctx_isp->reported_req_id)) {
				request_id = req->request_id;
				__cam_isp_ctx_update_event_record(ctx_isp,
					CAM_ISP_CTX_EVENT_EPOCH, req, NULL);
				break;
			}
		}

#ifdef OPLUS_FEATURE_CAMERA_COMMON
		if (ctx_isp->substate_activated == CAM_ISP_CTX_ACTIVATED_BUBBLE) {
			request_id = 0;
			memset(trace, 0, sizeof(trace));
			snprintf(trace, sizeof(trace), "KMD %d_4 Skip Frame", ctx->link_hdl);
			trace_int(trace, 0);
			trace_begin_end("Skip Frame: Req[%lld] CAM_ISP_CTX_ACTIVATED_BUBBLE", req->request_id);
		}
#else
		if (ctx_isp->substate_activated == CAM_ISP_CTX_ACTIVATED_BUBBLE)
			request_id = 0;
#endif
		if (request_id != 0)
			ctx_isp->reported_req_id = request_id;

		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_EPOCH,
			request_id);
	}

	ctx_isp->last_sof_timestamp = ctx_isp->sof_timestamp_val;
	return 0;
}

static int __cam_isp_ctx_notify_eof_in_activated_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_context *ctx = ctx_isp->base;
	uint64_t last_cdm_done_req = 0;

	/* update last cdm done timestamp */
	rc = __cam_isp_ctx_get_cdm_done_timestamp(ctx, &last_cdm_done_req);
	if (rc)
		CAM_ERR(CAM_ISP, "ctx:%u link: 0x%x Failed to get timestamp from HW",
			ctx->ctx_id, ctx->link_hdl);
	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_CDM_DONE, last_cdm_done_req);

	/* notify reqmgr with eof signal */
	rc = __cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_EOF, ctx_isp);
	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_EOF, 0);

	return rc;
}

static int __cam_isp_ctx_reg_upd_in_hw_error(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;
	return 0;
}

static int __cam_isp_ctx_sof_in_activated_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;
	struct cam_ctx_request *req = NULL;
	struct cam_context *ctx = ctx_isp->base;
	uint64_t request_id = 0;

	ctx_isp->last_sof_jiffies = jiffies;

	/* First check if there is a valid request in active list */
	list_for_each_entry(req, &ctx->active_req_list, list) {
		if (req->request_id > ctx_isp->reported_req_id) {
			request_id = req->request_id;
			break;
		}
	}

	/*
	 * If nothing in active list, current request might have not moved
	 * from wait to active list. This could happen if REG_UPDATE to sw
	 * is coming immediately after SOF
	 */
	if (request_id == 0) {
		req = list_first_entry(&ctx->wait_req_list,
			struct cam_ctx_request, list);
		if (req)
			request_id = req->request_id;
	}

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "in valid sof event data, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);

	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_SOF, request_id);

	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx, ctx %u request %llu, link: 0x%x",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val, ctx->ctx_id, request_id,
		ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_reg_upd_in_sof(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	struct cam_ctx_request *req = NULL;
	struct cam_isp_ctx_req *req_isp;
	struct cam_context *ctx = ctx_isp->base;

	if (ctx->state != CAM_CTX_ACTIVATED && ctx_isp->frame_id > 1) {
		CAM_DBG(CAM_ISP, "invalid RUP");
		goto end;
	}

	/*
	 * This is for the first update. The initial setting will
	 * cause the reg_upd in the first frame.
	 */
	if (!list_empty(&ctx->wait_req_list)) {
		req = list_first_entry(&ctx->wait_req_list,
			struct cam_ctx_request, list);
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		if (req_isp->num_fence_map_out == req_isp->num_acked)
			__cam_isp_ctx_move_req_to_free_list(ctx, req);
		else
			CAM_ERR(CAM_ISP,
				"receive rup in unexpected state, ctx_idx: %u, link: 0x%x",
				 ctx->ctx_id, ctx->link_hdl);
	}
	if (req != NULL) {
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_REG_UPDATE,
			req->request_id);
	}
end:
	return 0;
}

static int __cam_isp_ctx_epoch_in_applied(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	uint64_t request_id = 0;
	uint32_t wait_req_cnt = 0;
	uint32_t sof_event_status = CAM_REQ_MGR_SOF_EVENT_SUCCESS;
	struct cam_ctx_request             *req;
	struct cam_isp_ctx_req             *req_isp;
	struct cam_context                 *ctx = ctx_isp->base;
	struct cam_isp_hw_epoch_event_data *epoch_done_event_data =
		(struct cam_isp_hw_epoch_event_data *)evt_data;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "invalid event data");
		return -EINVAL;
	}

	ctx_isp->frame_id_meta = epoch_done_event_data->frame_id_meta;
	if (list_empty(&ctx->wait_req_list)) {
		/*
		 * If no wait req in epoch, this is an error case.
		 * The recovery is to go back to sof state
		 */
		CAM_ERR(CAM_ISP, "Ctx:%u link: 0x%x No wait request", ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;

		/* Send SOF event as empty frame*/
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
		__cam_isp_ctx_update_event_record(ctx_isp,
			CAM_ISP_CTX_EVENT_EPOCH, NULL, NULL);
		goto end;
	}

	if (ctx_isp->last_applied_jiffies >= ctx_isp->last_sof_jiffies) {
		list_for_each_entry(req, &ctx->wait_req_list, list) {
			wait_req_cnt++;
		}

		/*
		 * The previous req is applied after SOF and there is only
		 * one applied req, we don't need to report bubble for this case.
		 */
		if (wait_req_cnt == 1 && !ctx_isp->is_tfe_shdr) {
			req = list_first_entry(&ctx->wait_req_list,
				struct cam_ctx_request, list);
			request_id = req->request_id;
			CAM_INFO(CAM_ISP,
				"ctx:%d Don't report the bubble for req:%lld",
				ctx->ctx_id, request_id);
			goto end;
		}
	}

	/* Update state prior to notifying CRM */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;

	req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request,
		list);
	req_isp = (struct cam_isp_ctx_req *)req->req_priv;
	req_isp->bubble_detected = true;
	req_isp->reapply_type = CAM_CONFIG_REAPPLY_IO;
	req_isp->cdm_reset_before_apply = false;
	atomic_set(&ctx_isp->process_bubble, 1);

	CAM_INFO_RATE_LIMIT(CAM_ISP, "ctx:%u link: 0x%x Report Bubble flag %d req id:%lld",
		ctx->ctx_id, ctx->link_hdl, req_isp->bubble_report, req->request_id);

	if (req_isp->bubble_report) {
		__cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF, CRM_KMD_ERR_BUBBLE,
			req->request_id, ctx_isp);
		trace_cam_log_event("Bubble", "Rcvd epoch in applied state",
			req->request_id, ctx->ctx_id);
	} else {
		req_isp->bubble_report = 0;
		CAM_DBG(CAM_ISP, "Skip bubble recovery for req %lld ctx %u, link: 0x%x",
			req->request_id, ctx->ctx_id, ctx->link_hdl);

		if (ctx_isp->active_req_cnt <= 1)
			__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);
	}

	/*
	 * Always move the request to active list. Let buf done
	 * function handles the rest.
	 */
	list_del_init(&req->list);
	list_add_tail(&req->list, &ctx->active_req_list);
	ctx_isp->active_req_cnt++;
	CAM_DBG(CAM_REQ, "move request %lld to active list(cnt = %d), ctx %u, link: 0x%x",
		req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);

	/*
	 * Handle the deferred buf done after moving
	 * the bubble req to active req list.
	 */
	__cam_isp_ctx_handle_deferred_buf_done_in_bubble(
		ctx_isp, req);

	/*
	 * Update the record before req pointer to
	 * other invalid req.
	 */
	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_EPOCH, req, NULL);

	/*
	 * Get the req again from active_req_list in case
	 * the active req cnt is 2.
	 */
	list_for_each_entry(req, &ctx->active_req_list, list) {
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		if ((!req_isp->bubble_report) &&
			(req->request_id > ctx_isp->reported_req_id)) {
			request_id = req->request_id;
			ctx_isp->reported_req_id = request_id;
			CAM_DBG(CAM_ISP,
				"ctx %u link: 0x%x reported_req_id update to %lld",
				ctx->ctx_id, ctx->link_hdl, ctx_isp->reported_req_id);
			break;
		}
	}

	if ((request_id != 0) && req_isp->bubble_detected)
		sof_event_status = CAM_REQ_MGR_SOF_EVENT_ERROR;

	__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
		sof_event_status);

	cam_req_mgr_debug_delay_detect();
	trace_cam_delay_detect("ISP",
		"bubble epoch_in_applied", req->request_id,
		ctx->ctx_id, ctx->link_hdl, ctx->session_hdl,
		CAM_DEFAULT_VALUE);
end:
	if (request_id == 0) {
		req = list_last_entry(&ctx->active_req_list,
			struct cam_ctx_request, list);
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_EPOCH, req->request_id);
	} else {
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_EPOCH, request_id);
	}

	CAM_DBG(CAM_ISP, "next Substate[%s], ctx_idx: %u link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	return 0;
}

static int __cam_isp_ctx_buf_done_in_sof(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_done_event_data *done =
		(struct cam_isp_hw_done_event_data *) evt_data;

	rc = __cam_isp_ctx_handle_buf_done_in_activated_state(ctx_isp, done, 0);
	return rc;
}

static int __cam_isp_ctx_buf_done_in_applied(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_done_event_data *done =
		(struct cam_isp_hw_done_event_data *) evt_data;

	rc = __cam_isp_ctx_handle_buf_done_in_activated_state(ctx_isp, done, 0);
	return rc;
}


static int __cam_isp_ctx_sof_in_epoch(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;
	struct cam_context                    *ctx = ctx_isp->base;
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;
	struct cam_ctx_request *req;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "in valid sof event data");
		return -EINVAL;
	}

	ctx_isp->last_sof_jiffies = jiffies;

	if (atomic_read(&ctx_isp->apply_in_progress))
		CAM_INFO(CAM_ISP, "Apply is in progress at the time of SOF, ctx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);

	if (list_empty(&ctx->active_req_list))
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;
	else
		CAM_DBG(CAM_ISP, "Still need to wait for the buf done, ctx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	req = list_last_entry(&ctx->active_req_list,
		struct cam_ctx_request, list);
	if (req)
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_SOF,
			req->request_id);

	if (ctx_isp->frame_id == 1)
		CAM_INFO(CAM_ISP,
			"First SOF in EPCR ctx:%u link: 0x%x frame_id:%lld next substate %s",
			ctx->ctx_id, ctx->link_hdl, ctx_isp->frame_id,
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated));

	CAM_DBG(CAM_ISP, "SOF in epoch ctx:%u link: 0x%x frame_id:%lld next substate:%s",
		ctx->ctx_id, ctx->link_hdl, ctx_isp->frame_id,
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated));

	return rc;
}

static int __cam_isp_ctx_buf_done_in_epoch(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_done_event_data *done =
		(struct cam_isp_hw_done_event_data *) evt_data;

	rc = __cam_isp_ctx_handle_buf_done_in_activated_state(ctx_isp, done, 0);
	return rc;
}

static int __cam_isp_ctx_buf_done_in_bubble(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_done_event_data *done =
		(struct cam_isp_hw_done_event_data *) evt_data;

	rc = __cam_isp_ctx_handle_buf_done_in_activated_state(ctx_isp, done, 1);
	return rc;
}

static int __cam_isp_ctx_epoch_in_bubble_applied(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	uint64_t  request_id = 0;
	struct cam_ctx_request             *req;
	struct cam_isp_ctx_req             *req_isp;
	struct cam_context                 *ctx = ctx_isp->base;
	struct cam_isp_hw_epoch_event_data *epoch_done_event_data =
		(struct cam_isp_hw_epoch_event_data *)evt_data;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "invalid event data, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	ctx_isp->frame_id_meta = epoch_done_event_data->frame_id_meta;

	/*
	 * This means we missed the reg upd ack. So we need to
	 * transition to BUBBLE state again.
	 */

	if (list_empty(&ctx->wait_req_list)) {
		/*
		 * If no pending req in epoch, this is an error case.
		 * Just go back to the bubble state.
		 */
		CAM_ERR(CAM_ISP, "ctx:%u link: 0x%x No pending request.",
			ctx->ctx_id, ctx->link_hdl);
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
		__cam_isp_ctx_update_event_record(ctx_isp,
			CAM_ISP_CTX_EVENT_EPOCH, NULL, NULL);
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;
		goto end;
	}

	req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request,
		list);
	req_isp = (struct cam_isp_ctx_req *)req->req_priv;
	req_isp->bubble_detected = true;
	CAM_INFO_RATE_LIMIT(CAM_ISP, "Ctx:%u link: 0x%x Report Bubble flag %d req id:%lld",
		ctx->ctx_id, ctx->link_hdl, req_isp->bubble_report, req->request_id);
	req_isp->reapply_type = CAM_CONFIG_REAPPLY_IO;
	req_isp->cdm_reset_before_apply = false;

	if (req_isp->bubble_report) {
		__cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF, CRM_KMD_ERR_BUBBLE,
			req->request_id, ctx_isp);
		atomic_set(&ctx_isp->process_bubble, 1);
	} else {
		req_isp->bubble_report = 0;
		CAM_DBG(CAM_ISP, "Skip bubble recovery for req %lld ctx %u link: 0x%x",
			req->request_id, ctx->ctx_id, ctx->link_hdl);
		if (ctx_isp->active_req_cnt <= 1)
			__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);

		atomic_set(&ctx_isp->process_bubble, 1);
	}

	/*
	 * Always move the request to active list. Let buf done
	 * function handles the rest.
	 */
	list_del_init(&req->list);
	list_add_tail(&req->list, &ctx->active_req_list);
	ctx_isp->active_req_cnt++;
	CAM_DBG(CAM_ISP, "move request %lld to active list(cnt = %d) ctx %u, link: 0x%x",
		req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);

	/*
	 * Handle the deferred buf done after moving
	 * the bubble req to active req list.
	 */
	__cam_isp_ctx_handle_deferred_buf_done_in_bubble(
		ctx_isp, req);

	if (!req_isp->bubble_detected) {
		req = list_first_entry(&ctx->pending_req_list, struct cam_ctx_request,
			list);
		req_isp->bubble_detected = true;
		req_isp->reapply_type = CAM_CONFIG_REAPPLY_IO;
		req_isp->cdm_reset_before_apply = false;
		atomic_set(&ctx_isp->process_bubble, 1);
		list_del_init(&req->list);
		list_add_tail(&req->list, &ctx->active_req_list);
		ctx_isp->active_req_cnt++;
	}

	if (!req_isp->bubble_report) {
		if (req->request_id > ctx_isp->reported_req_id) {
			request_id = req->request_id;
			ctx_isp->reported_req_id = request_id;
			__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
				CAM_REQ_MGR_SOF_EVENT_ERROR);
			__cam_isp_ctx_update_event_record(ctx_isp,
				CAM_ISP_CTX_EVENT_EPOCH, req, NULL);
		} else {
			__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
				CAM_REQ_MGR_SOF_EVENT_SUCCESS);
			__cam_isp_ctx_update_event_record(ctx_isp,
				CAM_ISP_CTX_EVENT_EPOCH, NULL, NULL);
		}
	} else {
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
		__cam_isp_ctx_update_event_record(ctx_isp,
			CAM_ISP_CTX_EVENT_EPOCH, NULL, NULL);
	}
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;
	CAM_DBG(CAM_ISP, "next Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	cam_req_mgr_debug_delay_detect();
	trace_cam_delay_detect("ISP",
		"bubble epoch_in_bubble_applied",
		req->request_id, ctx->ctx_id, ctx->link_hdl,
		ctx->session_hdl,
		CAM_DEFAULT_VALUE);
end:
	req = list_last_entry(&ctx->active_req_list, struct cam_ctx_request,
		list);
	if (req)
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_EPOCH, req->request_id);
	return 0;
}

static int __cam_isp_ctx_buf_done_in_bubble_applied(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_done_event_data *done =
		(struct cam_isp_hw_done_event_data *) evt_data;

	rc = __cam_isp_ctx_handle_buf_done_in_activated_state(ctx_isp, done, 1);

	return rc;
}

static void __cam_isp_get_notification_evt_params(
	uint32_t hw_error, uint32_t *fence_evt_cause,
	uint32_t *req_mgr_err_code, uint32_t *recovery_type)
{
	uint32_t err_type, err_code = 0, recovery_type_temp;

	err_type = CAM_SYNC_ISP_EVENT_UNKNOWN;
	recovery_type_temp = CAM_REQ_MGR_ERROR_TYPE_RECOVERY;

	if (hw_error & CAM_ISP_HW_ERROR_OVERFLOW) {
		err_code |= CAM_REQ_MGR_ISP_UNREPORTED_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_OVERFLOW;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_OUTPUT_FIFO_OVERFLOW) {
		err_code |= CAM_REQ_MGR_CSID_FIFO_OVERFLOW_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_CSID_OUTPUT_FIFO_OVERFLOW;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_RECOVERY_OVERFLOW) {
		err_code |= CAM_REQ_MGR_CSID_RECOVERY_OVERFLOW_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_RECOVERY_OVERFLOW;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_P2I_ERROR) {
		err_code |= CAM_REQ_MGR_ISP_UNREPORTED_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_P2I_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_VIOLATION) {
		err_code |= CAM_REQ_MGR_ISP_UNREPORTED_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_VIOLATION;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_HWPD_VIOLATION) {
		err_code |= CAM_REQ_MGR_ISP_ERR_HWPD_VIOLATION;
		err_type = CAM_SYNC_ISP_EVENT_VIOLATION;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_BUSIF_OVERFLOW) {
		err_code |= CAM_REQ_MGR_ISP_UNREPORTED_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_BUSIF_OVERFLOW;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_SENSOR_SWITCH_ERROR) {
		err_code |= CAM_REQ_MGR_CSID_ERR_ON_SENSOR_SWITCHING;
		err_type = CAM_SYNC_ISP_EVENT_CSID_SENSOR_SWITCH_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_LANE_FIFO_OVERFLOW) {
		err_code |= CAM_REQ_MGR_CSID_LANE_FIFO_OVERFLOW_ERROR;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_PKT_HDR_CORRUPTED) {
		err_code |= CAM_REQ_MGR_CSID_RX_PKT_HDR_CORRUPTION;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_MISSING_PKT_HDR_DATA) {
		err_code |= CAM_REQ_MGR_CSID_MISSING_PKT_HDR_DATA;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_UNBOUNDED_FRAME) {
		err_code |= CAM_REQ_MGR_CSID_UNBOUNDED_FRAME;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_FRAME_SIZE) {
		err_code |= CAM_REQ_MGR_CSID_PIXEL_COUNT_MISMATCH;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_MISSING_EOT) {
		err_code |= CAM_REQ_MGR_CSID_MISSING_EOT;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}
	if (hw_error & CAM_ISP_HW_ERROR_CSID_PKT_PAYLOAD_CORRUPTED) {
		err_code |= CAM_REQ_MGR_CSID_RX_PKT_PAYLOAD_CORRUPTION;
		err_type = CAM_SYNC_ISP_EVENT_CSID_RX_ERROR;
		recovery_type_temp |= CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;
	}

	if (recovery_type_temp == (CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY |
		CAM_REQ_MGR_ERROR_TYPE_RECOVERY))
		recovery_type_temp = CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY;

	if (!err_code)
		err_code = CAM_REQ_MGR_ISP_UNREPORTED_ERROR;

	*req_mgr_err_code = err_code;
	*fence_evt_cause = err_type;
	*recovery_type = recovery_type_temp;
}

static bool __cam_isp_ctx_request_can_reapply(
	struct cam_isp_ctx_req *req_isp)
{
	int i;

	for (i = 0; i < req_isp->num_fence_map_out; i++)
		if (req_isp->fence_map_out[i].sync_id == -1)
			return false;

	return true;
}

static int __cam_isp_ctx_validate_for_req_reapply_util(
	struct cam_isp_context *ctx_isp)
{
	int rc = 0;
	struct cam_ctx_request *req_temp;
	struct cam_ctx_request *req = NULL;
	struct cam_isp_ctx_req *req_isp = NULL;
	struct cam_context *ctx = ctx_isp->base;

	if (in_task())
		spin_lock_bh(&ctx->lock);

	/* Check for req in active/wait lists */
	if (list_empty(&ctx->active_req_list)) {
		CAM_DBG(CAM_ISP,
			"Active request list empty for ctx: %u on link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

		if (list_empty(&ctx->wait_req_list)) {
			CAM_WARN(CAM_ISP,
				"No active/wait req for ctx: %u on link: 0x%x start from pending",
				ctx->ctx_id, ctx->link_hdl);
			rc = 0;
			goto end;
		}
	}

	/* Validate if all fences for active requests are not signaled */
	if (!list_empty(&ctx->active_req_list)) {
		list_for_each_entry_safe_reverse(req, req_temp,
			&ctx->active_req_list, list) {
			/*
			 * If some fences of the active request are already
			 * signaled, we should not do recovery for the buffer
			 * and timestamp consistency.
			 */
			req_isp = (struct cam_isp_ctx_req *)req->req_priv;
			if (!__cam_isp_ctx_request_can_reapply(req_isp)) {
				CAM_WARN(CAM_ISP,
					"Req: %llu in ctx:%u on link: 0x%x fence has partially signaled, cannot do recovery",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				rc = -EINVAL;
				goto end;
			}
		}
	}

	/* Move active requests to pending list */
	if (!list_empty(&ctx->active_req_list)) {
		list_for_each_entry_safe_reverse(req, req_temp,
			&ctx->active_req_list, list) {
			list_del_init(&req->list);
			__cam_isp_ctx_enqueue_request_in_order(ctx, req, false);
			ctx_isp->active_req_cnt--;
			CAM_DBG(CAM_ISP, "ctx:%u link:0x%x move active req %llu to pending",
				ctx->ctx_id, ctx->link_hdl, req->request_id);
		}
	}

	/* Move wait requests to pending list */
	if (!list_empty(&ctx->wait_req_list)) {
		list_for_each_entry_safe_reverse(req, req_temp, &ctx->wait_req_list, list) {
			list_del_init(&req->list);
			__cam_isp_ctx_enqueue_request_in_order(ctx, req, false);
			CAM_DBG(CAM_ISP, "ctx:%u link:0x%x move wait req %llu to pending",
				ctx->ctx_id, ctx->link_hdl, req->request_id);
		}
	}

end:
	if (in_task())
		spin_unlock_bh(&ctx->lock);
	return rc;
}

static int __cam_isp_ctx_handle_recovery_req_util(
	struct cam_isp_context *ctx_isp)
{
	int rc = 0;
	struct cam_context *ctx = ctx_isp->base;
	struct cam_ctx_request *req_to_reapply = NULL;

	if (list_empty(&ctx->pending_req_list)) {
		CAM_WARN(CAM_ISP,
			"No pending request to recover from on ctx: %u", ctx->ctx_id);
		return -EINVAL;
	}

	req_to_reapply = list_first_entry(&ctx->pending_req_list,
		struct cam_ctx_request, list);
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_EPOCH;
	ctx_isp->recovery_req_id = req_to_reapply->request_id;
	atomic_set(&ctx_isp->internal_recovery_set, 1);

	CAM_INFO(CAM_ISP, "Notify CRM to reapply req:%llu for ctx:%u link:0x%x",
		req_to_reapply->request_id, ctx->ctx_id, ctx->link_hdl);

	rc = __cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF,
		CRM_KMD_WARN_INTERNAL_RECOVERY, req_to_reapply->request_id,
			ctx_isp);
	if (rc) {
		/* Unable to notify CRM to do reapply back to normal */
		CAM_WARN(CAM_ISP,
			"ctx:%u link:0x%x unable to notify CRM for req %llu",
			ctx->ctx_id, ctx->link_hdl, ctx_isp->recovery_req_id);
		ctx_isp->recovery_req_id = 0;
		atomic_set(&ctx_isp->internal_recovery_set, 0);
	}

	return rc;
}

static int __cam_isp_ctx_trigger_error_req_reapply(
	uint32_t err_type, struct cam_isp_context *ctx_isp)
{
	int rc = 0;
	struct cam_context *ctx = ctx_isp->base;

	if ((err_type & CAM_ISP_HW_ERROR_RECOVERY_OVERFLOW) &&
		(isp_ctx_debug.disable_internal_recovery_mask &
		CAM_ISP_CTX_DISABLE_RECOVERY_BUS_OVERFLOW))
		return -EINVAL;

	/*
	 * For errors that can be recoverable within kmd, we
	 * try to do internal hw stop, restart and notify CRM
	 * to do reapply with the help of bubble control flow.
	 */

	rc = __cam_isp_ctx_validate_for_req_reapply_util(ctx_isp);
	if (rc)
		goto end;

	rc = __cam_isp_ctx_handle_recovery_req_util(ctx_isp);
	if (rc)
		goto end;

	CAM_DBG(CAM_ISP, "Triggered internal recovery for req:%llu ctx:%u on link 0x%x",
		ctx_isp->recovery_req_id, ctx->ctx_id, ctx->link_hdl);

end:
	return rc;
}

static int __cam_isp_ctx_handle_error(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int                              rc = 0;
	enum cam_req_mgr_device_error    error;
	uint32_t                         i = 0;
	bool                             found = 0;
	struct cam_ctx_request          *req = NULL;
	struct cam_ctx_request          *req_to_report = NULL;
	struct cam_ctx_request          *req_to_dump = NULL;
	struct cam_ctx_request          *req_temp;
	struct cam_isp_ctx_req          *req_isp = NULL;
	struct cam_isp_ctx_req          *req_isp_to_report = NULL;
	uint64_t                         error_request_id;
	struct cam_hw_fence_map_entry   *fence_map_out = NULL;
	uint32_t                         recovery_type, fence_evt_cause;
	uint32_t                         req_mgr_err_code;

	struct cam_context *ctx = ctx_isp->base;
	struct cam_isp_hw_error_event_data  *error_event_data =
			(struct cam_isp_hw_error_event_data *)evt_data;

	CAM_DBG(CAM_ISP, "Enter HW error_type = %d, ctx:%u on link 0x%x",
		error_event_data->error_type, ctx->ctx_id, ctx->link_hdl);

	if (error_event_data->try_internal_recovery) {
		rc = __cam_isp_ctx_trigger_error_req_reapply(error_event_data->error_type, ctx_isp);
		if (!rc)
			goto exit;
	}

	if (!ctx_isp->offline_context)
		__cam_isp_ctx_pause_crm_timer(ctx);

	__cam_isp_ctx_dump_frame_timing_record(ctx_isp);

	__cam_isp_ctx_trigger_reg_dump(CAM_HW_MGR_CMD_REG_DUMP_ON_ERROR, ctx);

	__cam_isp_get_notification_evt_params(error_event_data->error_type,
		&fence_evt_cause, &req_mgr_err_code, &recovery_type);
	/*
	 * The error is likely caused by first request on the active list.
	 * If active list is empty check wait list (maybe error hit as soon
	 * as RUP and we handle error before RUP.
	 */
	if (list_empty(&ctx->active_req_list)) {
		CAM_DBG(CAM_ISP,
			"handling error with no active request, ctx:%u on link 0x%x",
			 ctx->ctx_id, ctx->link_hdl);
		if (list_empty(&ctx->wait_req_list)) {
			CAM_ERR_RATE_LIMIT(CAM_ISP,
				"Error with no active/wait request, ctx:%u on link 0x%x",
				ctx->ctx_id, ctx->link_hdl);
			goto end;
		} else {
			req_to_dump = list_first_entry(&ctx->wait_req_list,
				struct cam_ctx_request, list);
		}
	} else {
		req_to_dump = list_first_entry(&ctx->active_req_list,
			struct cam_ctx_request, list);
	}

	req_isp = (struct cam_isp_ctx_req *) req_to_dump->req_priv;

	if (error_event_data->enable_req_dump)
		rc = cam_isp_ctx_dump_req(req_isp, 0, 0, NULL, false);

	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_ERROR, req_to_dump->request_id);

	list_for_each_entry_safe(req, req_temp,
		&ctx->active_req_list, list) {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		if (!req_isp->bubble_report) {
			CAM_ERR(CAM_ISP, "signalled error for req %llu, ctx:%u on link 0x%x",
				req->request_id, ctx->ctx_id, ctx->link_hdl);
			for (i = 0; i < req_isp->num_fence_map_out; i++) {
				fence_map_out =
					&req_isp->fence_map_out[i];
				if (req_isp->fence_map_out[i].sync_id != -1) {
					CAM_DBG(CAM_ISP,
						"req %llu, Sync fd 0x%x ctx %u, link 0x%x",
						req->request_id,
						req_isp->fence_map_out[i].sync_id,
						ctx->ctx_id, ctx->link_hdl);
					rc = cam_sync_signal(
						fence_map_out->sync_id,
						CAM_SYNC_STATE_SIGNALED_ERROR,
						fence_evt_cause);
					fence_map_out->sync_id = -1;
				}
			}
			list_del_init(&req->list);
			__cam_isp_ctx_move_req_to_free_list(ctx, req);
			ctx_isp->active_req_cnt--;
		} else {
			found = 1;
			break;
		}
	}

	if (found)
		goto move_to_pending;

	list_for_each_entry_safe(req, req_temp,
		&ctx->wait_req_list, list) {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		if (!req_isp->bubble_report) {
			CAM_ERR(CAM_ISP, "signalled error for req %llu, ctx %u, link 0x%x",
				req->request_id, ctx->ctx_id, ctx->link_hdl);
			for (i = 0; i < req_isp->num_fence_map_out; i++) {
				fence_map_out =
					&req_isp->fence_map_out[i];
				if (req_isp->fence_map_out[i].sync_id != -1) {
					CAM_DBG(CAM_ISP,
						"req %llu, Sync fd 0x%x ctx %u link 0x%x",
						req->request_id,
						req_isp->fence_map_out[i].sync_id,
						ctx->ctx_id, ctx->link_hdl);
					rc = cam_sync_signal(
						fence_map_out->sync_id,
						CAM_SYNC_STATE_SIGNALED_ERROR,
						fence_evt_cause);
					fence_map_out->sync_id = -1;
				}
			}
			list_del_init(&req->list);
			__cam_isp_ctx_move_req_to_free_list(ctx, req);
		} else {
			found = 1;
			break;
		}
	}

move_to_pending:
	/*
	 * If bubble recovery is enabled on any request we need to move that
	 * request and all the subsequent requests to the pending list.
	 * Note:
	 * We need to traverse the active list in reverse order and add
	 * to head of pending list.
	 * e.g. pending current state: 10, 11 | active current state: 8, 9
	 * intermittent for loop iteration- pending: 9, 10, 11 | active: 8
	 * final state - pending: 8, 9, 10, 11 | active: NULL
	 */
	if (found) {
		list_for_each_entry_safe_reverse(req, req_temp,
			&ctx->active_req_list, list) {
			list_del_init(&req->list);
			list_add(&req->list, &ctx->pending_req_list);
			ctx_isp->active_req_cnt--;
		}
		list_for_each_entry_safe_reverse(req, req_temp,
			&ctx->wait_req_list, list) {
			list_del_init(&req->list);
			list_add(&req->list, &ctx->pending_req_list);
		}
	}

end:
	do {
		if (list_empty(&ctx->pending_req_list)) {
			error_request_id = ctx_isp->last_applied_req_id;
			break;
		}
		req = list_first_entry(&ctx->pending_req_list,
			struct cam_ctx_request, list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		error_request_id = ctx_isp->last_applied_req_id;

		if (req_isp->bubble_report) {
			req_to_report = req;
			req_isp_to_report = req_to_report->req_priv;
			break;
		}

		for (i = 0; i < req_isp->num_fence_map_out; i++) {
			if (req_isp->fence_map_out[i].sync_id != -1)
				rc = cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_ERROR,
					fence_evt_cause);
			req_isp->fence_map_out[i].sync_id = -1;
		}
		list_del_init(&req->list);
		__cam_isp_ctx_move_req_to_free_list(ctx, req);

	} while (req->request_id < ctx_isp->last_applied_req_id);

	if (ctx_isp->offline_context)
		goto exit;

	error = CRM_KMD_ERR_FATAL;
	if (req_isp_to_report && req_isp_to_report->bubble_report)
		if (error_event_data->recovery_enabled)
			error = CRM_KMD_ERR_BUBBLE;

	__cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF, error,
		error_request_id, ctx_isp);

	/*
	 * Need to send error occurred in KMD
	 * This will help UMD to take necessary action
	 * and to dump relevant info
	 */
	if (error == CRM_KMD_ERR_FATAL)
		__cam_isp_ctx_notify_v4l2_error_event(recovery_type,
			req_mgr_err_code, error_request_id, ctx);

	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_HW_ERROR;
	CAM_DBG(CAM_ISP, "Handling error done on ctx: %u, link: 0x%x", ctx->ctx_id, ctx->link_hdl);

exit:
	return rc;
}

static int __cam_isp_ctx_fs2_sof_in_sof_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;
	struct cam_ctx_request *req;
	struct cam_context *ctx = ctx_isp->base;
	uint64_t  request_id  = 0;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "in valid sof event data, ctx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);

	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx, ctx: %u, link: 0x%x",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val, ctx->ctx_id, ctx->link_hdl);

	if (!(list_empty(&ctx->wait_req_list)))
		goto end;

	if (ctx_isp->active_req_cnt <= 2) {
		__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);

		list_for_each_entry(req, &ctx->active_req_list, list) {
			if (req->request_id > ctx_isp->reported_req_id) {
				request_id = req->request_id;
				ctx_isp->reported_req_id = request_id;
				break;
			}
		}
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	}

	__cam_isp_ctx_update_state_monitor_array(ctx_isp,
		CAM_ISP_STATE_CHANGE_TRIGGER_SOF, request_id);

end:
	return rc;
}

static int __cam_isp_ctx_fs2_buf_done(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;
	struct cam_isp_hw_done_event_data *done =
		(struct cam_isp_hw_done_event_data *) evt_data;
	struct cam_context *ctx = ctx_isp->base;
	int prev_active_req_cnt = 0;
	int curr_req_id = 0;
	struct cam_ctx_request  *req;

	prev_active_req_cnt = ctx_isp->active_req_cnt;
	req = list_first_entry(&ctx->active_req_list,
		struct cam_ctx_request, list);
	if (req)
		curr_req_id = req->request_id;

	rc = __cam_isp_ctx_handle_buf_done_in_activated_state(ctx_isp, done, 0);

	if (prev_active_req_cnt == ctx_isp->active_req_cnt + 1) {
		if (list_empty(&ctx->wait_req_list) &&
			list_empty(&ctx->active_req_list)) {
			CAM_DBG(CAM_ISP, "No request, move to SOF, ctx_idx: %u, link: 0x%x",
				 ctx->ctx_id, ctx->link_hdl);
			ctx_isp->substate_activated =
				CAM_ISP_CTX_ACTIVATED_SOF;
			if (ctx_isp->reported_req_id < curr_req_id) {
				ctx_isp->reported_req_id = curr_req_id;
				__cam_isp_ctx_send_sof_timestamp(ctx_isp,
					curr_req_id,
					CAM_REQ_MGR_SOF_EVENT_SUCCESS);
			}
		}
	}

	return rc;
}

static int __cam_isp_ctx_fs2_buf_done_in_epoch(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;

	rc =  __cam_isp_ctx_fs2_buf_done(ctx_isp, evt_data);
	return rc;
}

static int __cam_isp_ctx_fs2_buf_done_in_applied(
	struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;

	rc =  __cam_isp_ctx_fs2_buf_done(ctx_isp, evt_data);
	return rc;
}

static int __cam_isp_ctx_fs2_reg_upd_in_sof(struct cam_isp_context *ctx_isp,
	void *evt_data)
{
	int rc = 0;
	struct cam_ctx_request *req = NULL;
	struct cam_isp_ctx_req *req_isp;
	struct cam_context *ctx = ctx_isp->base;

	if (ctx->state != CAM_CTX_ACTIVATED && ctx_isp->frame_id > 1) {
		CAM_DBG(CAM_ISP, "invalid RUP");
		goto end;
	}

	/*
	 * This is for the first update. The initial setting will
	 * cause the reg_upd in the first frame.
	 */
	if (!list_empty(&ctx->wait_req_list)) {
		req = list_first_entry(&ctx->wait_req_list,
			struct cam_ctx_request, list);
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		if (req_isp->num_fence_map_out == req_isp->num_acked)
			__cam_isp_ctx_move_req_to_free_list(ctx, req);
		else
			CAM_ERR(CAM_ISP,
				"receive rup in unexpected state, ctx_idx: %u, link: 0x%x",
				 ctx->ctx_id, ctx->link_hdl);
	}
	if (req != NULL) {
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_REG_UPDATE,
			req->request_id);
	}
end:
	return rc;
}

static int __cam_isp_ctx_fs2_reg_upd_in_applied_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_ctx_request  *req = NULL;
	struct cam_context      *ctx = ctx_isp->base;
	struct cam_isp_ctx_req  *req_isp;
	uint64_t  request_id  = 0;

	if (list_empty(&ctx->wait_req_list)) {
		CAM_ERR(CAM_ISP, "Reg upd ack with no waiting request, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	req = list_first_entry(&ctx->wait_req_list,
			struct cam_ctx_request, list);
	list_del_init(&req->list);

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;
	if (req_isp->num_fence_map_out != 0) {
		list_add_tail(&req->list, &ctx->active_req_list);
		ctx_isp->active_req_cnt++;
		CAM_DBG(CAM_REQ, "move request %lld to active list(cnt = %d), ctx:%u,link:0x%x",
			 req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
	} else {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		/* no io config, so the request is completed. */
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
	}

	/*
	 * This function only called directly from applied and bubble applied
	 * state so change substate here.
	 */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_EPOCH;
	if (req_isp->num_fence_map_out != 1)
		goto end;

	if (ctx_isp->active_req_cnt <= 2) {
		list_for_each_entry(req, &ctx->active_req_list, list) {
			if (req->request_id > ctx_isp->reported_req_id) {
				request_id = req->request_id;
				ctx_isp->reported_req_id = request_id;
				break;
			}
		}

		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);

		__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);
	}

	CAM_DBG(CAM_ISP, "next Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(ctx_isp->substate_activated),
		ctx->ctx_id, ctx->link_hdl);

end:
	if (req != NULL && !rc) {
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_REG_UPDATE,
			req->request_id);
	}
	return rc;
}

static void __cam_isp_ctx_notify_aeb_error_for_sec_event(
	struct cam_isp_context *ctx_isp)
{
	struct cam_context *ctx = ctx_isp->base;

	if ((++ctx_isp->aeb_error_cnt) <= CAM_ISP_CONTEXT_AEB_ERROR_CNT_MAX) {
		CAM_WARN(CAM_ISP,
			"AEB slave RDI's current request's SOF seen after next req is applied for ctx: %u on link: 0x%x last_applied_req: %llu err_cnt: %u",
			ctx->ctx_id, ctx->link_hdl, ctx_isp->last_applied_req_id, ctx_isp->aeb_error_cnt);
		return;
	}

	CAM_ERR(CAM_ISP,
		"Fatal - AEB slave RDI's current request's SOF seen after next req is applied, EPOCH height need to be re-configured for ctx: %u on link: 0x%x err_cnt: %u",
		ctx->ctx_id, ctx->link_hdl, ctx_isp->aeb_error_cnt);

	/* Pause CRM timer */
	if (!ctx_isp->offline_context)
		__cam_isp_ctx_pause_crm_timer(ctx);

	/* Trigger reg dump */
	__cam_isp_ctx_trigger_reg_dump(CAM_HW_MGR_CMD_REG_DUMP_ON_ERROR, ctx);

	/* Notify CRM on fatal error */
	__cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF, CRM_KMD_ERR_FATAL,
		ctx_isp->last_applied_req_id, ctx_isp);

	/* Notify userland on error */
	__cam_isp_ctx_notify_v4l2_error_event(CAM_REQ_MGR_ERROR_TYPE_RECOVERY,
		CAM_REQ_MGR_CSID_ERR_ON_SENSOR_SWITCHING, ctx_isp->last_applied_req_id, ctx);

	/* Change state to HALT, stop further processing of HW events */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_HALT;

	/* Dump AEB debug info */
	__cam_isp_ctx_dump_frame_timing_record(ctx_isp);
}

static int __cam_isp_ctx_trigger_internal_recovery(
	bool sync_frame_drop, struct cam_isp_context *ctx_isp)
{
	int                                 rc = 0;
	bool                                do_recovery = true;
	struct cam_context                 *ctx = ctx_isp->base;
	struct cam_ctx_request             *req = NULL;
	struct cam_isp_ctx_req             *req_isp = NULL;

	if (list_empty(&ctx->wait_req_list)) {
		/*
		 * If the wait list is empty, and we encounter a "silent" frame drop
		 * then the settings applied on the previous frame, did not reflect
		 * at the next frame boundary, it's expected to latch a frame after.
		 * No need to recover. If it's an out of sync drop use pending req
		 */
		if (sync_frame_drop && !list_empty(&ctx->pending_req_list))
			req = list_first_entry(&ctx->pending_req_list,
				struct cam_ctx_request, list);
		else
			do_recovery = false;
	}

	/* If both wait and pending list have no request to recover on */
	if (!do_recovery) {
		CAM_WARN(CAM_ISP,
			"No request to perform recovery - ctx: %u on link: 0x%x last_applied: %lld last_buf_done: %lld",
			ctx->ctx_id, ctx->link_hdl, ctx_isp->last_applied_req_id,
			ctx_isp->req_info.last_bufdone_req_id);
		goto end;
	}

	if (!req) {
		req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request, list);
		if (req->request_id != ctx_isp->last_applied_req_id)
			CAM_WARN(CAM_ISP,
				"Top of wait list req: %llu does not match with last applied: %llu in ctx: %u on link: 0x%x",
				req->request_id, ctx_isp->last_applied_req_id,
				ctx->ctx_id, ctx->link_hdl);
	}

	req_isp = (struct cam_isp_ctx_req *)req->req_priv;
	/*
	 * Treat this as bubble, after recovery re-start from appropriate sub-state
	 * This will block servicing any further apply calls from CRM
	 */
	atomic_set(&ctx_isp->internal_recovery_set, 1);
	atomic_set(&ctx_isp->process_bubble, 1);
	ctx_isp->recovery_req_id = req->request_id;

	/* Wait for active request's to finish before issuing recovery */
	if (ctx_isp->active_req_cnt) {
		req_isp->bubble_detected = true;
		CAM_WARN(CAM_ISP,
			"Active req cnt: %u wait for all buf dones before kicking in recovery on req: %lld ctx: %u on link: 0x%x",
			ctx_isp->active_req_cnt, ctx_isp->recovery_req_id,
			ctx->ctx_id, ctx->link_hdl);
	} else {
		rc = __cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF,
				CRM_KMD_WARN_INTERNAL_RECOVERY, ctx_isp->recovery_req_id, ctx_isp);
		if (rc) {
			/* Unable to do bubble recovery reset back to normal */
			CAM_WARN(CAM_ISP,
				"Unable to perform internal recovery [bubble reporting failed] for req: %llu in ctx: %u on link: 0x%x",
				ctx_isp->recovery_req_id, ctx->ctx_id, ctx->link_hdl);
			__cam_isp_context_reset_internal_recovery_params(ctx_isp);
			goto end;
		}

		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;
		list_del_init(&req->list);
		list_add(&req->list, &ctx->pending_req_list);
	}

end:
	return rc;
}

static int __cam_isp_ctx_handle_secondary_events(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	bool recover = false, sync_frame_drop = false;
	struct cam_context *ctx = ctx_isp->base;
	struct cam_isp_hw_secondary_event_data *sec_evt_data =
		(struct cam_isp_hw_secondary_event_data *)evt_data;

	/* Current scheme to handle only for custom AEB */
	if (!ctx_isp->aeb_enabled) {
		CAM_WARN(CAM_ISP,
			"Recovery not supported for non-AEB ctx: %u on link: 0x%x reject sec evt: %u",
			ctx->ctx_id, ctx->link_hdl, sec_evt_data->evt_type);
		goto end;
	}

	if (atomic_read(&ctx_isp->internal_recovery_set)) {
		CAM_WARN(CAM_ISP,
			"Internal recovery in progress in ctx: %u on link: 0x%x reject sec evt: %u",
			ctx->ctx_id, ctx->link_hdl, sec_evt_data->evt_type);
		goto end;
	}

	/*
	 * In case of custom AEB ensure first exposure frame has
	 * not moved forward with its settings without second/third
	 * expoure frame coming in. Also track for bubble, in case of system
	 * delays it's possible for the IFE settings to be not written to
	 * HW on a given frame. If these scenarios occurs flag as error,
	 * and recover.
	 */
	switch (sec_evt_data->evt_type) {
	case CAM_ISP_HW_SEC_EVENT_SOF:
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_SEC_EVT_SOF,
			ctx_isp->last_applied_req_id);

		__cam_isp_ctx_update_frame_timing_record(CAM_ISP_HW_SECONDARY_EVENT, ctx_isp);

		/* Slave RDI's frame starting post IFE EPOCH - Fatal */
		if ((ctx_isp->substate_activated ==
			CAM_ISP_CTX_ACTIVATED_APPLIED) ||
			(ctx_isp->substate_activated ==
			CAM_ISP_CTX_ACTIVATED_BUBBLE_APPLIED))
			__cam_isp_ctx_notify_aeb_error_for_sec_event(ctx_isp);
		else
			/* Reset error count */
			ctx_isp->aeb_error_cnt = 0;
		break;
	case CAM_ISP_HW_SEC_EVENT_EPOCH:
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_SEC_EVT_EPOCH,
			ctx_isp->last_applied_req_id);
		ctx_isp->out_of_sync_cnt = 0;

		/*
		 * Master RDI frame dropped in CSID, due to programming delay no RUP/AUP
		 * On such occasions use CSID CAMIF EPOCH for bubble detection, flag
		 * on detection and perform necessary bubble recovery
		 */
		if ((ctx_isp->substate_activated ==
			CAM_ISP_CTX_ACTIVATED_APPLIED) ||
			(ctx_isp->substate_activated ==
			CAM_ISP_CTX_ACTIVATED_BUBBLE_APPLIED)) {
			recover = true;
			CAM_WARN(CAM_ISP,
				"Programming delay input frame dropped ctx: %u on link: 0x%x last_applied_req: %llu, kicking in internal recovery....",
				ctx->ctx_id, ctx->link_hdl, ctx_isp->last_applied_req_id);
		}
		break;
	case CAM_ISP_HW_SEC_EVENT_OUT_OF_SYNC_FRAME_DROP:
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_FRAME_DROP,
			ctx_isp->last_applied_req_id);

		/* Avoid recovery loop if frame is dropped at stream on */
		if (!ctx_isp->frame_id) {
			CAM_ERR(CAM_ISP,
				"Sensor sync [vc mismatch] frame dropped at stream on ctx: %u link: 0x%x frame_id: %u last_applied_req: %lld",
				ctx->ctx_id, ctx->link_hdl,
				ctx_isp->frame_id, ctx_isp->last_applied_req_id);
			rc = -EPERM;
			break;
		}

		if (!(ctx_isp->out_of_sync_cnt++) &&
			(ctx_isp->recovery_req_id == ctx_isp->last_applied_req_id)) {
			CAM_WARN(CAM_ISP,
				"Sensor sync [vc mismatch] frame dropped ctx: %u on link: 0x%x last_applied_req: %llu last_recovered_req: %llu out_of_sync_cnt: %u, recovery maybe in progress...",
				ctx->ctx_id, ctx->link_hdl, ctx_isp->last_applied_req_id,
				ctx_isp->recovery_req_id, ctx_isp->out_of_sync_cnt);
			break;
		}

		recover = true;
		sync_frame_drop = true;
		ctx_isp->out_of_sync_cnt = 0;
		CAM_WARN(CAM_ISP,
			"Sensor sync [vc mismatch] frame dropped ctx: %u on link: 0x%x last_applied_req: %llu last_recovered_req: %llu out_of_sync_cnt: %u, kicking in internal recovery....",
			ctx->ctx_id, ctx->link_hdl, ctx_isp->last_applied_req_id,
			ctx_isp->recovery_req_id, ctx_isp->out_of_sync_cnt);
		break;
	default:
		break;
	}

	if (recover &&
		!(isp_ctx_debug.disable_internal_recovery_mask & CAM_ISP_CTX_DISABLE_RECOVERY_AEB))
		rc = __cam_isp_ctx_trigger_internal_recovery(sync_frame_drop, ctx_isp);

end:
	return rc;
}

static struct cam_isp_ctx_irq_ops
	cam_isp_ctx_activated_state_machine_irq[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_sof,
			__cam_isp_ctx_notify_sof_in_activated_state,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_sof,
			__cam_isp_ctx_handle_secondary_events,
		},
	},
	/* APPLIED */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_applied_state,
			__cam_isp_ctx_epoch_in_applied,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_applied,
			__cam_isp_ctx_handle_secondary_events,
		},
	},
	/* EPOCH */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_epoch,
			__cam_isp_ctx_reg_upd_in_epoch_bubble_state,
			__cam_isp_ctx_notify_sof_in_activated_state,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_epoch,
			__cam_isp_ctx_handle_secondary_events,
		},
	},
	/* BUBBLE */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_epoch_bubble_state,
			__cam_isp_ctx_notify_sof_in_activated_state,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_bubble,
			__cam_isp_ctx_handle_secondary_events,
		},
	},
	/* Bubble Applied */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_applied_state,
			__cam_isp_ctx_epoch_in_bubble_applied,
			NULL,
			__cam_isp_ctx_buf_done_in_bubble_applied,
			__cam_isp_ctx_handle_secondary_events,
		},
	},
	/* HW ERROR */
	{
		.irq_ops = {
			NULL,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_hw_error,
			NULL,
			NULL,
			NULL,
		},
	},
	/* HALT */
	{
	},
};

static struct cam_isp_ctx_irq_ops
	cam_isp_ctx_fs2_state_machine_irq[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_fs2_sof_in_sof_state,
			__cam_isp_ctx_fs2_reg_upd_in_sof,
			__cam_isp_ctx_fs2_sof_in_sof_state,
			__cam_isp_ctx_notify_eof_in_activated_state,
			NULL,
		},
	},
	/* APPLIED */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_fs2_reg_upd_in_applied_state,
			__cam_isp_ctx_epoch_in_applied,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_fs2_buf_done_in_applied,
		},
	},
	/* EPOCH */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_epoch,
			__cam_isp_ctx_reg_upd_in_epoch_bubble_state,
			__cam_isp_ctx_notify_sof_in_activated_state,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_fs2_buf_done_in_epoch,
		},
	},
	/* BUBBLE */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_epoch_bubble_state,
			__cam_isp_ctx_notify_sof_in_activated_state,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_bubble,
		},
	},
	/* Bubble Applied */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_applied_state,
			__cam_isp_ctx_epoch_in_bubble_applied,
			NULL,
			__cam_isp_ctx_buf_done_in_bubble_applied,
		},
	},
	/* HW ERROR */
	{
		.irq_ops = {
			NULL,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_hw_error,
			NULL,
			NULL,
			NULL,
		},
	},
	/* HALT */
	{
	},
};

static struct cam_isp_ctx_irq_ops
	cam_isp_ctx_offline_state_machine_irq[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
		},
	},
	/* APPLIED */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_applied_state,
			__cam_isp_ctx_offline_epoch_in_activated_state,
			NULL,
			__cam_isp_ctx_buf_done_in_applied,
		},
	},
	/* EPOCH */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_sof_in_activated_state,
			NULL,
			__cam_isp_ctx_offline_epoch_in_activated_state,
			NULL,
			__cam_isp_ctx_buf_done_in_epoch,
		},
	},
	/* BUBBLE */
	{
	},
	/* Bubble Applied */
	{
	},
	/* HW ERROR */
	{
		.irq_ops = {
			NULL,
			__cam_isp_ctx_sof_in_activated_state,
			__cam_isp_ctx_reg_upd_in_hw_error,
			NULL,
			NULL,
			NULL,
		},
	},
	/* HALT */
	{
	},
};

static inline int cam_isp_context_apply_evt_injection(struct cam_context *ctx)
{
	struct cam_isp_context *ctx_isp = ctx->ctx_priv;
	struct cam_hw_inject_evt_param *evt_inject_params = &ctx_isp->evt_inject_params;
	struct cam_common_evt_inject_data inject_evt = {0};
	int rc;

	inject_evt.evt_params = evt_inject_params;
	rc = cam_context_apply_evt_injection(ctx, &inject_evt);
	if (rc)
		CAM_ERR(CAM_ISP, "Fail to apply event injection ctx_id: %u link: 0x%x req_id: %u",
			ctx->ctx_id, ctx->link_hdl, evt_inject_params->req_id);

	evt_inject_params->is_valid = false;

	return rc;
}

static inline void __cam_isp_ctx_update_fcg_prediction_idx(
	struct cam_context                      *ctx,
	uint64_t                                 request_id,
	struct cam_isp_fcg_prediction_tracker   *fcg_tracker,
	struct cam_isp_fcg_config_info          *fcg_info)
{
	struct cam_isp_context *ctx_isp = ctx->ctx_priv;

	if ((fcg_tracker->sum_skipped == 0) ||
		(fcg_tracker->sum_skipped > CAM_ISP_MAX_FCG_PREDICTIONS)) {
		fcg_info->use_current_cfg = true;
		CAM_DBG(CAM_ISP,
			"Apply req: %llu, Use current FCG value, frame_id: %llu, ctx_id: %u",
			request_id, ctx_isp->frame_id, ctx->ctx_id);
	} else {
		fcg_info->prediction_idx = fcg_tracker->sum_skipped;
		CAM_DBG(CAM_ISP,
			"Apply req: %llu, FCG prediction: %u, frame_id: %llu, ctx_id: %u",
			request_id, fcg_tracker->sum_skipped,
			ctx_isp->frame_id, ctx->ctx_id);
	}
}

static inline void __cam_isp_ctx_print_fcg_tracker(
	struct cam_isp_fcg_prediction_tracker *fcg_tracker)
{
	uint32_t skipped_list[CAM_ISP_AFD_PIPELINE_DELAY];
	struct cam_isp_skip_frame_info *skip_info;
	int i = 0;

	list_for_each_entry(skip_info,
		&fcg_tracker->skipped_list, list) {
		skipped_list[i] = skip_info->num_frame_skipped;
		i += 1;
	}

	CAM_DBG(CAM_ISP,
		"FCG tracker num_skipped: %u, sum_skipped: %u, skipped list: [%u, %u, %u]",
		fcg_tracker->num_skipped, fcg_tracker->sum_skipped,
		skipped_list[0], skipped_list[1], skipped_list[2]);
}

static int __cam_isp_ctx_apply_req_in_activated_state(
	struct cam_context *ctx, struct cam_req_mgr_apply_request *apply,
	enum cam_isp_ctx_activated_substate next_state)
{
	int rc = 0;
	struct cam_ctx_request                  *req;
	struct cam_ctx_request                  *active_req = NULL;
	struct cam_isp_ctx_req                  *req_isp;
	struct cam_isp_ctx_req                  *active_req_isp;
	struct cam_isp_context                  *ctx_isp = NULL;
	struct cam_hw_config_args                cfg = {0};
	struct cam_isp_skip_frame_info          *skip_info;
	struct cam_isp_fcg_prediction_tracker   *fcg_tracker;
	struct cam_isp_fcg_config_info          *fcg_info;

	ctx_isp = (struct cam_isp_context *) ctx->ctx_priv;

	/* Reset mswitch ref cnt */
	atomic_set(&ctx_isp->mswitch_default_apply_delay_ref_cnt,
		ctx_isp->mswitch_default_apply_delay_max_cnt);

	if (apply->re_apply)
		if (apply->request_id <= ctx_isp->last_applied_req_id) {
			CAM_INFO_RATE_LIMIT(CAM_ISP,
				"ctx_id:%u link: 0x%x Trying to reapply the same request %llu again",
				ctx->ctx_id, ctx->link_hdl,
				apply->request_id);
			return 0;
		}

	if (list_empty(&ctx->pending_req_list)) {
		CAM_ERR_RATE_LIMIT(CAM_ISP,
			"ctx_id:%u link: 0x%x No available request for Apply id %lld",
			ctx->ctx_id, ctx->link_hdl,
			apply->request_id);
		rc = -EFAULT;
		goto end;
	}

	/*
	 * When the pipeline has issue, the requests can be queued up in the
	 * pipeline. In this case, we should reject the additional request.
	 * The maximum number of request allowed to be outstanding is 2.
	 *
	 */
	if (atomic_read(&ctx_isp->process_bubble)) {
		CAM_INFO_RATE_LIMIT(CAM_ISP,
			"ctx_id:%u link: 0x%x Processing bubble cannot apply Request Id %llu",
			ctx->ctx_id, ctx->link_hdl,
			apply->request_id);
		rc = -EFAULT;
		goto end;
	}

	/*
	 * When isp processing internal recovery, the crm may still apply
	 * req to isp ctx. In this case, we should reject this req apply.
	 */
	if (atomic_read(&ctx_isp->internal_recovery_set)) {
		CAM_INFO_RATE_LIMIT(CAM_ISP,
			"ctx_id:%u link: 0x%x Processing recovery cannot apply Request Id %lld",
			ctx->ctx_id, ctx->link_hdl,
			apply->request_id);
		rc = -EAGAIN;
		goto end;
	}

	spin_lock_bh(&ctx->lock);
	req = list_first_entry(&ctx->pending_req_list, struct cam_ctx_request,
		list);
	spin_unlock_bh(&ctx->lock);

	/*
	 * Check whether the request id is matching the tip, if not, this means
	 * we are in the middle of the error handling. Need to reject this apply
	 */
	if (req->request_id != apply->request_id) {
		CAM_ERR_RATE_LIMIT(CAM_ISP,
			"ctx_id:%u link: 0x%x Invalid Request Id asking %llu existing %llu",
			ctx->ctx_id, ctx->link_hdl,
			apply->request_id, req->request_id);
		rc = -EFAULT;
		goto end;
	}

	CAM_DBG(CAM_REQ, "Apply request %lld in Substate[%s] ctx %u, link: 0x%x",
		req->request_id,
		__cam_isp_ctx_substate_val_to_type(ctx_isp->substate_activated),
		ctx->ctx_id, ctx->link_hdl);
	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	if (ctx_isp->active_req_cnt >=  2) {
		CAM_WARN_RATE_LIMIT(CAM_ISP,
			"Reject apply request (id %lld) due to congestion(cnt = %d) ctx %u, link: 0x%x",
			req->request_id,
			ctx_isp->active_req_cnt,
			ctx->ctx_id, ctx->link_hdl);

		spin_lock_bh(&ctx->lock);
		if (!list_empty(&ctx->active_req_list))
			active_req = list_first_entry(&ctx->active_req_list,
				struct cam_ctx_request, list);
		else
			CAM_ERR_RATE_LIMIT(CAM_ISP,
				"WARNING: should not happen (cnt = %d) but active_list empty, ctx %u, link: 0x%x",
				ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
		spin_unlock_bh(&ctx->lock);

		if (active_req) {
			active_req_isp =
				(struct cam_isp_ctx_req *) active_req->req_priv;
			__cam_isp_ctx_handle_buf_done_fail_log(ctx_isp,
				active_req->request_id, active_req_isp);
		}

		rc = -EFAULT;
		goto end;
	}

	/* Reset congestion counter */
	ctx_isp->congestion_cnt = 0;

	req_isp->bubble_report = apply->report_if_bubble;

	/*
	 * Reset all buf done/bubble flags for the req being applied
	 * If internal recovery has led to re-apply of same
	 * request, clear all stale entities
	 */
	req_isp->num_acked = 0;
	req_isp->num_deferred_acks = 0;
	req_isp->cdm_reset_before_apply = false;
	req_isp->bubble_detected = false;

	cfg.ctxt_to_hw_map = ctx_isp->hw_ctx;
	cfg.request_id = req->request_id;
	cfg.hw_update_entries = req_isp->cfg;
	cfg.num_hw_update_entries = req_isp->num_cfg;
	cfg.priv  = &req_isp->hw_update_data;
	cfg.init_packet = 0;
	cfg.reapply_type = req_isp->reapply_type;
	cfg.cdm_reset_before_apply = req_isp->cdm_reset_before_apply;

	if ((ctx_isp->evt_inject_params.is_valid) &&
		(req->request_id == ctx_isp->evt_inject_params.req_id)) {
		rc = cam_isp_context_apply_evt_injection(ctx_isp->base);
		if (!rc)
			goto end;
	}

	/* Decide the exact FCG prediction */
	fcg_tracker = &ctx_isp->fcg_tracker;
	fcg_info = &req_isp->hw_update_data.fcg_info;
	if (!list_empty(&fcg_tracker->skipped_list)) {
		__cam_isp_ctx_print_fcg_tracker(fcg_tracker);
		skip_info = list_first_entry(&fcg_tracker->skipped_list,
			struct cam_isp_skip_frame_info, list);
		fcg_tracker->sum_skipped -= skip_info->num_frame_skipped;
		if (unlikely((uint32_t)UINT_MAX - fcg_tracker->sum_skipped <
			fcg_tracker->num_skipped))
			fcg_tracker->num_skipped =
				(uint32_t)UINT_MAX - fcg_tracker->sum_skipped;
		fcg_tracker->sum_skipped += fcg_tracker->num_skipped;
		skip_info->num_frame_skipped = fcg_tracker->num_skipped;
		fcg_tracker->num_skipped = 0;
		list_rotate_left(&fcg_tracker->skipped_list);

		__cam_isp_ctx_print_fcg_tracker(fcg_tracker);
		__cam_isp_ctx_update_fcg_prediction_idx(ctx,
			apply->request_id, fcg_tracker, fcg_info);
	}

	atomic_set(&ctx_isp->apply_in_progress, 1);

	rc = ctx->hw_mgr_intf->hw_config(ctx->hw_mgr_intf->hw_mgr_priv, &cfg);
	if (!rc) {
		spin_lock_bh(&ctx->lock);
		ctx_isp->substate_activated = next_state;
		ctx_isp->last_applied_req_id = apply->request_id;
		ctx_isp->last_applied_jiffies = jiffies;

		if (ctx_isp->is_tfe_shdr) {
			if (ctx_isp->is_shdr_master && req_isp->hw_update_data.mup_en)
				apply->dual_trigger_status = req_isp->hw_update_data.num_exp;
			else
				apply->dual_trigger_status = CAM_REQ_DUAL_TRIGGER_NONE;
		} else {
			apply->dual_trigger_status = CAM_REQ_DUAL_TRIGGER_NONE;
		}

		list_del_init(&req->list);
		if (atomic_read(&ctx_isp->internal_recovery_set))
			__cam_isp_ctx_enqueue_request_in_order(ctx, req, false);
		else
			list_add_tail(&req->list, &ctx->wait_req_list);
		CAM_DBG(CAM_ISP, "new Substate[%s], applied req %lld, ctx_idx: %u, link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(next_state),
			ctx_isp->last_applied_req_id, ctx->ctx_id, ctx->link_hdl);
		spin_unlock_bh(&ctx->lock);

		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_APPLIED,
			req->request_id);
		__cam_isp_ctx_update_event_record(ctx_isp,
			CAM_ISP_CTX_EVENT_APPLY, req, NULL);
	} else if (rc == -EALREADY) {
		spin_lock_bh(&ctx->lock);
		req_isp->bubble_detected = true;
		req_isp->cdm_reset_before_apply = false;
		atomic_set(&ctx_isp->process_bubble, 1);
		list_del_init(&req->list);
		list_add(&req->list, &ctx->active_req_list);
		ctx_isp->active_req_cnt++;
		spin_unlock_bh(&ctx->lock);
		CAM_DBG(CAM_REQ,
			"move request %lld to active list(cnt = %d), ctx %u, link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
	} else {
		CAM_ERR_RATE_LIMIT(CAM_ISP,
			"ctx_id:%u link: 0x%x,Can not apply (req %lld) the configuration, rc %d",
			ctx->ctx_id, ctx->link_hdl, apply->request_id, rc);
	}

	atomic_set(&ctx_isp->apply_in_progress, 0);
end:
	return rc;
}

static int __cam_isp_ctx_apply_req_in_sof(
	struct cam_context *ctx, struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	CAM_DBG(CAM_ISP, "current Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	rc = __cam_isp_ctx_apply_req_in_activated_state(ctx, apply,
		CAM_ISP_CTX_ACTIVATED_APPLIED);
	CAM_DBG(CAM_ISP, "new Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	if (rc)
		CAM_DBG(CAM_ISP, "Apply failed in Substate[%s], rc %d, ctx %u, link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), rc, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_apply_req_in_epoch(
	struct cam_context *ctx, struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	CAM_DBG(CAM_ISP, "current Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	rc = __cam_isp_ctx_apply_req_in_activated_state(ctx, apply,
		CAM_ISP_CTX_ACTIVATED_APPLIED);
	CAM_DBG(CAM_ISP, "new Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	if (rc)
		CAM_DBG(CAM_ISP, "Apply failed in Substate[%s], rc %d, ctx %u, link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), rc, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_apply_req_in_bubble(
	struct cam_context *ctx, struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	CAM_DBG(CAM_ISP, "current Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	rc = __cam_isp_ctx_apply_req_in_activated_state(ctx, apply,
		CAM_ISP_CTX_ACTIVATED_BUBBLE_APPLIED);
	CAM_DBG(CAM_ISP, "new Substate[%s], ctx %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	if (rc)
		CAM_DBG(CAM_ISP, "Apply failed in Substate[%s], rc %d, ctx %u, link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), rc, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static void __cam_isp_ctx_find_mup_for_default_settings(
	int64_t req_id, struct cam_context *ctx,
	struct cam_ctx_request **switch_req)
{
	struct cam_ctx_request *req, *temp_req;

	if (list_empty(&ctx->pending_req_list)) {
		CAM_DBG(CAM_ISP,
			"Pending list empty, unable to find mup for req: %lld ctx: %u",
			req_id, ctx->ctx_id);
		return;
	}

	list_for_each_entry_safe(
		req, temp_req, &ctx->pending_req_list, list) {
		if (req->request_id == req_id) {
			struct cam_isp_ctx_req *req_isp = (struct cam_isp_ctx_req *)req->req_priv;

			if (req_isp->hw_update_data.mup_en) {
				*switch_req = req;
				CAM_DBG(CAM_ISP,
					"Found mup for last applied max pd req: %lld in ctx: %u",
					req_id, ctx->ctx_id);
			}
		}
	}
}

static int __cam_isp_ctx_apply_default_req_settings(
	struct cam_context *ctx, struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	bool skip_rup_aup = false;
	struct cam_ctx_request *req = NULL;
	struct cam_isp_ctx_req *req_isp = NULL;
	struct cam_isp_context *isp_ctx =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_hw_cmd_args hw_cmd_args;
	struct cam_isp_hw_cmd_args isp_hw_cmd_args;
	struct cam_hw_config_args cfg = {0};

	if (isp_ctx->mode_switch_en && isp_ctx->handle_mswitch) {
		if ((apply->last_applied_max_pd_req > 0) &&
			(atomic_dec_and_test(&isp_ctx->mswitch_default_apply_delay_ref_cnt))) {
			__cam_isp_ctx_find_mup_for_default_settings(
				apply->last_applied_max_pd_req, ctx, &req);
		}

		if (req) {
			req_isp = (struct cam_isp_ctx_req *)req->req_priv;

			CAM_DBG(CAM_ISP,
				"Applying IQ for mode switch req: %lld ctx: %u",
				req->request_id, ctx->ctx_id);
			cfg.ctxt_to_hw_map = isp_ctx->hw_ctx;
			cfg.request_id = req->request_id;
			cfg.hw_update_entries = req_isp->cfg;
			cfg.num_hw_update_entries = req_isp->num_cfg;
			cfg.priv  = &req_isp->hw_update_data;
			cfg.init_packet = 0;
			cfg.reapply_type = CAM_CONFIG_REAPPLY_IQ;
			cfg.cdm_reset_before_apply = req_isp->cdm_reset_before_apply;

			rc = ctx->hw_mgr_intf->hw_config(ctx->hw_mgr_intf->hw_mgr_priv, &cfg);
			if (rc) {
				CAM_ERR(CAM_ISP,
					"Failed to apply req: %lld IQ settings in ctx: %u",
					req->request_id, ctx->ctx_id);
				goto end;
			}
			skip_rup_aup = true;
		}
	}

	if (isp_ctx->use_default_apply) {
		hw_cmd_args.ctxt_to_hw_map = isp_ctx->hw_ctx;
		hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
		isp_hw_cmd_args.cmd_type =
			CAM_ISP_HW_MGR_CMD_PROG_DEFAULT_CFG;

		isp_hw_cmd_args.cmd_data = (void *)&skip_rup_aup;
		hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;

		rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
			&hw_cmd_args);
		if (rc)
			CAM_ERR(CAM_ISP,
				"Failed to apply default settings rc %d ctx %u, link: 0x%x",
				rc, ctx->ctx_id, ctx->link_hdl);
		else
			CAM_DBG(CAM_ISP, "Applied default settings rc %d ctx: %u link: 0x%x",
				rc, ctx->ctx_id, ctx->link_hdl);
	}

end:
	return rc;
}

static void *cam_isp_ctx_user_dump_req_list(
	void *dump_struct, uint8_t *addr_ptr)
{
	struct list_head        *head = NULL;
	uint64_t                *addr;
	struct cam_ctx_request  *req, *req_temp;

	head = (struct list_head *)dump_struct;

	addr = (uint64_t *)addr_ptr;

	if (!list_empty(head)) {
		list_for_each_entry_safe(req, req_temp, head, list) {
			*addr++ = req->request_id;
		}
	}

	return addr;
}

static void *cam_isp_ctx_user_dump_active_requests(
	void *dump_struct, uint8_t *addr_ptr)
{
	uint64_t                *addr;
	struct cam_ctx_request  *req;

	req = (struct cam_ctx_request *)dump_struct;

	addr = (uint64_t *)addr_ptr;
	*addr++ = req->request_id;
	return addr;
}

static int __cam_isp_ctx_dump_req_info(
	struct cam_context     *ctx,
	struct cam_ctx_request *req,
	struct cam_common_hw_dump_args *dump_args)
{
	int                                 i, rc = 0;
	uint32_t                            min_len;
	size_t                              remain_len;
	struct cam_isp_ctx_req             *req_isp;
	struct cam_ctx_request             *req_temp;

	if (!req || !ctx || !dump_args) {
		CAM_ERR(CAM_ISP, "Invalid parameters %pK %pK %pK",
			req, ctx, dump_args);
		return -EINVAL;
	}
	req_isp = (struct cam_isp_ctx_req *)req->req_priv;

	if (dump_args->buf_len <= dump_args->offset) {
		CAM_WARN(CAM_ISP,
			"Dump buffer overshoot len %zu offset %zu, ctx_idx: %u, link: 0x%x",
			dump_args->buf_len, dump_args->offset, ctx->ctx_id, ctx->link_hdl);
		return -ENOSPC;
	}

	remain_len = dump_args->buf_len - dump_args->offset;
	min_len = sizeof(struct cam_isp_context_dump_header) +
		(CAM_ISP_CTX_DUMP_REQUEST_NUM_WORDS *
			req_isp->num_fence_map_out *
			sizeof(uint64_t));

	if (remain_len < min_len) {
		CAM_WARN(CAM_ISP, "Dump buffer exhaust remain %zu min %u, ctx_idx: %u, link: 0x%x",
			remain_len, min_len, ctx->ctx_id, ctx->link_hdl);
		return -ENOSPC;
	}

	/* Dump pending request list */
	rc = cam_common_user_dump_helper(dump_args, cam_isp_ctx_user_dump_req_list,
		&ctx->pending_req_list, sizeof(uint64_t), "ISP_OUT_FENCE_PENDING_REQUESTS:");
	if (rc) {
		CAM_ERR(CAM_ISP,
			"CAM_ISP_CONTEXT:Pending request dump failed, rc:%d, ctx:%u, link:0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
		return rc;
	}

	/* Dump applied request list */
	rc = cam_common_user_dump_helper(dump_args, cam_isp_ctx_user_dump_req_list,
		&ctx->wait_req_list, sizeof(uint64_t), "ISP_OUT_FENCE_APPLIED_REQUESTS:");
	if (rc) {
		CAM_ERR(CAM_ISP,
			"CAM_ISP_CONTEXT: Applied request dump failed, rc:%d, ctx:%u, link:0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
		return rc;
	}

	/* Dump active request list */
	rc = cam_common_user_dump_helper(dump_args, cam_isp_ctx_user_dump_req_list,
		&ctx->active_req_list, sizeof(uint64_t), "ISP_OUT_FENCE_ACTIVE_REQUESTS:");
	if (rc) {
		CAM_ERR(CAM_ISP,
			"CAM_ISP_CONTEXT: Active request dump failed, rc:%d, ctx:%u, link:0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
		return rc;
	}

	/* Dump active request fences */
	if (!list_empty(&ctx->active_req_list)) {
		list_for_each_entry_safe(req, req_temp, &ctx->active_req_list, list) {
			req_isp = (struct cam_isp_ctx_req *)req->req_priv;
			for (i = 0; i < req_isp->num_fence_map_out; i++) {
				rc = cam_common_user_dump_helper(dump_args,
					cam_isp_ctx_user_dump_active_requests,
					req, sizeof(uint64_t),
					"ISP_OUT_FENCE_REQUEST_ACTIVE.%s.%u.%d:",
					__cam_isp_ife_sfe_resource_handle_id_to_type(
						req_isp->fence_map_out[i].resource_handle),
					req_isp->fence_map_out[i].image_buf_addr[0],
					req_isp->fence_map_out[i].sync_id);

				if (rc) {
					CAM_ERR(CAM_ISP,
						"CAM_ISP_CONTEXT DUMP_REQ_INFO: Dump failed, rc: %d, ctx_idx: %u, link: 0x%x",
						rc, ctx->ctx_id, ctx->link_hdl);
					return rc;
				}
			}
		}
	}

	return rc;
}

static void *cam_isp_ctx_user_dump_timer(
	void *dump_struct, uint8_t *addr_ptr)
{
	struct cam_ctx_request  *req = NULL;
	struct cam_isp_ctx_req  *req_isp = NULL;
	uint64_t                *addr;
	ktime_t                  cur_time;

	req = (struct cam_ctx_request *)dump_struct;
	req_isp = (struct cam_isp_ctx_req *)req->req_priv;
	cur_time = ktime_get();

	addr = (uint64_t *)addr_ptr;

	*addr++ = req->request_id;
	*addr++ = ktime_to_timespec64(
		req_isp->event_timestamp[CAM_ISP_CTX_EVENT_APPLY]).tv_sec;
	*addr++ = ktime_to_timespec64(
		req_isp->event_timestamp[CAM_ISP_CTX_EVENT_APPLY]).tv_nsec / NSEC_PER_USEC;
	*addr++ = ktime_to_timespec64(cur_time).tv_sec;
	*addr++ = ktime_to_timespec64(cur_time).tv_nsec / NSEC_PER_USEC;
	return addr;
}

static void *cam_isp_ctx_user_dump_stream_info(
	void *dump_struct, uint8_t *addr_ptr)
{
	struct cam_context           *ctx = NULL;
	int32_t                      *addr;

	ctx = (struct cam_context *)dump_struct;

	addr = (int32_t *)addr_ptr;

	*addr++ = ctx->ctx_id;
	*addr++ = ctx->dev_hdl;
	*addr++ = ctx->link_hdl;

	return addr;
}

static int __cam_isp_ctx_dump_in_top_state(
	struct cam_context           *ctx,
	struct cam_req_mgr_dump_info *dump_info)
{
	int                                 rc = 0;
	bool                                dump_only_event_record = false;
	size_t                              buf_len;
	size_t                              remain_len;
	ktime_t                             cur_time;
	uint32_t                            min_len;
	uint64_t                            diff;
	uintptr_t                           cpu_addr;
	uint8_t                             req_type;
	struct cam_isp_context             *ctx_isp;
	struct cam_ctx_request             *req = NULL;
	struct cam_isp_ctx_req             *req_isp;
	struct cam_ctx_request             *req_temp;
	struct cam_hw_dump_args             ife_dump_args;
	struct cam_common_hw_dump_args      dump_args;
	struct cam_hw_cmd_args              hw_cmd_args;
	struct cam_isp_hw_cmd_args          isp_hw_cmd_args;

	rc  = cam_mem_get_cpu_buf(dump_info->buf_handle,
		&cpu_addr, &buf_len);
	if (rc) {
		CAM_ERR(CAM_ISP, "Invalid handle %u rc %d, ctx_idx: %u, link: 0x%x",
			dump_info->buf_handle, rc, ctx->ctx_id, ctx->link_hdl);
		return rc;
	}

	spin_lock_bh(&ctx->lock);
	list_for_each_entry_safe(req, req_temp,
		&ctx->active_req_list, list) {
		if (req->request_id == dump_info->req_id) {
			CAM_INFO(CAM_ISP, "isp dump active list req: %lld, ctx_idx: %u, link: 0x%x",
			    dump_info->req_id, ctx->ctx_id, ctx->link_hdl);
			req_type = 'a';
			goto hw_dump;
		}
	}
	list_for_each_entry_safe(req, req_temp,
		&ctx->wait_req_list, list) {
		if (req->request_id == dump_info->req_id) {
			CAM_INFO(CAM_ISP, "isp dump wait list req: %lld, ctx_idx: %u, link: 0x%x",
			    dump_info->req_id, ctx->ctx_id, ctx->link_hdl);
			req_type = 'w';
			goto hw_dump;
		}
	}
	list_for_each_entry_safe(req, req_temp,
		&ctx->pending_req_list, list) {
		if (req->request_id == dump_info->req_id) {
			CAM_INFO(CAM_ISP,
			    "isp dump pending list req: %lld, ctx_idx: %u, link: 0x%x",
			    dump_info->req_id, ctx->ctx_id, ctx->link_hdl);
			req_type = 'p';
			goto hw_dump;
		}
	}
	goto end;
hw_dump:
	if (buf_len <= dump_info->offset) {
		spin_unlock_bh(&ctx->lock);
		CAM_WARN(CAM_ISP,
		    "Dump buffer overshoot len %zu offset %zu, ctx_idx: %u, link: 0x%x",
		    buf_len, dump_info->offset, ctx->ctx_id, ctx->link_hdl);
		cam_mem_put_cpu_buf(dump_info->buf_handle);
		return -ENOSPC;
	}

	remain_len = buf_len - dump_info->offset;
	min_len = sizeof(struct cam_isp_context_dump_header) +
		(CAM_ISP_CTX_DUMP_NUM_WORDS * sizeof(uint64_t));

	if (remain_len < min_len) {
		CAM_WARN(CAM_ISP,
		    "Dump buffer exhaust remain %zu min %u, ctx_idx: %u, link: 0x%x",
		    remain_len, min_len, ctx->ctx_id, ctx->link_hdl);
		spin_unlock_bh(&ctx->lock);
		cam_mem_put_cpu_buf(dump_info->buf_handle);
		return -ENOSPC;
	}

	ctx_isp = (struct cam_isp_context *) ctx->ctx_priv;
	req_isp = (struct cam_isp_ctx_req *) req->req_priv;
	cur_time = ktime_get();
	diff = ktime_us_delta(
		req_isp->event_timestamp[CAM_ISP_CTX_EVENT_APPLY],
		cur_time);
	__cam_isp_ctx_print_event_record(ctx_isp);
	if (diff < CAM_ISP_CTX_RESPONSE_TIME_THRESHOLD) {
		CAM_INFO(CAM_ISP, "req %lld found no error, ctx_idx: %u, link: 0x%x",
			req->request_id, ctx->ctx_id, ctx->link_hdl);
		dump_only_event_record = true;
	}

	dump_args.req_id = dump_info->req_id;
	dump_args.cpu_addr = cpu_addr;
	dump_args.buf_len = buf_len;
	dump_args.offset = dump_info->offset;
	dump_args.ctxt_to_hw_map = ctx_isp->hw_ctx;

	/* Dump time info */
	rc = cam_common_user_dump_helper(&dump_args, cam_isp_ctx_user_dump_timer,
		req, sizeof(uint64_t), "ISP_CTX_DUMP.%c:", req_type);
	if (rc) {
		CAM_ERR(CAM_ISP, "Time dump fail %lld, rc: %d, ctx_idx: %u, link: 0x%x",
			req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	dump_info->offset = dump_args.offset;

	min_len = sizeof(struct cam_isp_context_dump_header) +
		(CAM_ISP_CTX_DUMP_NUM_WORDS * sizeof(int32_t));
	remain_len = buf_len - dump_info->offset;
	if (remain_len < min_len) {
		CAM_WARN(CAM_ISP,
		    "Dump buffer exhaust remain %zu min %u, ctx_idx: %u, link: 0x%x",
		    remain_len, min_len, ctx->ctx_id, ctx->link_hdl);
		spin_unlock_bh(&ctx->lock);
		cam_mem_put_cpu_buf(dump_info->buf_handle);
		return -ENOSPC;
	}

	/* Dump stream info */
	ctx->ctxt_to_hw_map = ctx_isp->hw_ctx;
	if (ctx->hw_mgr_intf->hw_dump) {
		/* Dump first part of stream info from isp context */
		rc = cam_common_user_dump_helper(&dump_args,
			cam_isp_ctx_user_dump_stream_info, ctx,
			sizeof(int32_t), "ISP_STREAM_INFO_FROM_CTX:");
		if (rc) {
			CAM_ERR(CAM_ISP,
			    "ISP CTX stream info dump fail %lld, rc: %d, ctx: %u, link: 0x%x",
			    req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
			goto end;
		}

		dump_info->offset = dump_args.offset;
		remain_len = buf_len - dump_info->offset;
		if (remain_len < min_len) {
			CAM_WARN(CAM_ISP,
				"Dump buffer exhaust remain %zu min %u, ctx_idx: %u, link: 0x%x",
				remain_len, min_len, ctx->ctx_id, ctx->link_hdl);
			spin_unlock_bh(&ctx->lock);
			cam_mem_put_cpu_buf(dump_info->buf_handle);
			return -ENOSPC;
		}

		/* Dump second part of stream info from ife hw manager */
		hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
		hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
		isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_DUMP_STREAM_INFO;
		isp_hw_cmd_args.cmd_data = &dump_args;
		hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;

		rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv, &hw_cmd_args);
		if (rc) {
			CAM_ERR(CAM_ISP,
			    "IFE HW MGR stream info dump fail %lld, rc: %d, ctx: %u, link: 0x%x",
			    req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
			goto end;
		}

		dump_info->offset = dump_args.offset;
	}

	/* Dump event record */
	rc = __cam_isp_ctx_dump_event_record(ctx_isp, &dump_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Event record dump fail %lld, rc: %d, ctx_idx: %u, link: 0x%x",
			req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	dump_info->offset = dump_args.offset;
	if (dump_only_event_record) {
		goto end;
	}

	/* Dump state monitor array */
	rc = __cam_isp_ctx_user_dump_state_monitor_array(ctx_isp, &dump_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Dump event fail %lld, rc: %d, ctx_idx: %u, link: 0x%x",
			req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	/* Dump request info */
	rc = __cam_isp_ctx_dump_req_info(ctx, req, &dump_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Dump Req info fail %lld, rc: %d, ctx_idx: %u, link: 0x%x",
			req->request_id, rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	spin_unlock_bh(&ctx->lock);

	/* Dump CSID, VFE, and SFE info */
	dump_info->offset = dump_args.offset;
	if (ctx->hw_mgr_intf->hw_dump) {
		ife_dump_args.offset = dump_args.offset;
		ife_dump_args.request_id = dump_info->req_id;
		ife_dump_args.buf_handle = dump_info->buf_handle;
		ife_dump_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
		rc = ctx->hw_mgr_intf->hw_dump(
			ctx->hw_mgr_intf->hw_mgr_priv,
			&ife_dump_args);
		dump_info->offset = ife_dump_args.offset;
	}
	cam_mem_put_cpu_buf(dump_info->buf_handle);
	return rc;

end:
	spin_unlock_bh(&ctx->lock);
	cam_mem_put_cpu_buf(dump_info->buf_handle);
	return rc;
}

static int __cam_isp_ctx_flush_req_in_flushed_state(
	struct cam_context               *ctx,
	struct cam_req_mgr_flush_request *flush_req)
{
	CAM_INFO(CAM_ISP, "Flush (type %d) in flushed state req id %lld ctx_id:%u link: 0x%x",
		flush_req->type, flush_req->req_id, ctx->ctx_id, ctx->link_hdl);
	if (flush_req->req_id > ctx->last_flush_req)
		ctx->last_flush_req = flush_req->req_id;

	return 0;
}

static int __cam_isp_ctx_flush_req(struct cam_context *ctx,
	struct list_head *req_list, struct cam_req_mgr_flush_request *flush_req)
{
	int i, rc, tmp = 0;
	struct cam_ctx_request           *req;
	struct cam_ctx_request           *req_temp;
	struct cam_isp_ctx_req           *req_isp;
	struct list_head                  flush_list;
	struct cam_isp_context           *ctx_isp = NULL;

	ctx_isp = (struct cam_isp_context *) ctx->ctx_priv;

	INIT_LIST_HEAD(&flush_list);
	if (list_empty(req_list)) {
		CAM_DBG(CAM_ISP, "request list is empty, ctx_id:%u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		if (flush_req->type == CAM_REQ_MGR_FLUSH_TYPE_CANCEL_REQ) {
			CAM_INFO(CAM_ISP,
			    "no request to cancel(lastapplied:%lld cancel:%lld),ctx:%u link:0x%x",
			    ctx_isp->last_applied_req_id, flush_req->req_id,
			    ctx->ctx_id, ctx->link_hdl);
			return -EINVAL;
		} else
			return 0;
	}

	CAM_DBG(CAM_REQ, "Flush [%u] in progress for req_id %llu, ctx_id:%u link: 0x%x",
		flush_req->type, flush_req->req_id, ctx->ctx_id, ctx->link_hdl);
	list_for_each_entry_safe(req, req_temp, req_list, list) {
		if (flush_req->type == CAM_REQ_MGR_FLUSH_TYPE_CANCEL_REQ) {
			if (req->request_id != flush_req->req_id) {
				continue;
			} else {
				list_del_init(&req->list);
				list_add_tail(&req->list, &flush_list);
				__cam_isp_ctx_update_state_monitor_array(
					ctx_isp,
					CAM_ISP_STATE_CHANGE_TRIGGER_FLUSH,
					req->request_id);
				break;
			}
		}
		list_del_init(&req->list);
		list_add_tail(&req->list, &flush_list);
		__cam_isp_ctx_update_state_monitor_array(ctx_isp,
			CAM_ISP_STATE_CHANGE_TRIGGER_FLUSH, req->request_id);
	}

	if (list_empty(&flush_list)) {
		/*
		 * Maybe the req isn't sent to KMD since UMD already skip
		 * req in CSL layer.
		 */
		CAM_INFO(CAM_ISP,
			"flush list is empty, flush type %d for req %llu, ctx_id:%u link: 0x%x",
			flush_req->type, flush_req->req_id, ctx->ctx_id, ctx->link_hdl);
		return 0;
	}

	list_for_each_entry_safe(req, req_temp, &flush_list, list) {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);

		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		for (i = 0; i < req_isp->num_fence_map_out; i++) {
			if (req_isp->fence_map_out[i].sync_id != -1) {
				CAM_DBG(CAM_ISP,
					"Flush req 0x%llx, fence %d, ctx_id:%u link: 0x%x",
					req->request_id,
					req_isp->fence_map_out[i].sync_id,
					ctx->ctx_id, ctx->link_hdl);
				rc = cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_CANCEL,
					CAM_SYNC_ISP_EVENT_FLUSH);
				if (rc) {
					tmp = req_isp->fence_map_out[i].sync_id;
					CAM_ERR_RATE_LIMIT(CAM_ISP,
						"signal fence %d failed, ctx_id:%u link: 0x%x",
						tmp, ctx->ctx_id, ctx->link_hdl);
				}
				req_isp->fence_map_out[i].sync_id = -1;
			}
		}
		req_isp->reapply_type = CAM_CONFIG_REAPPLY_NONE;
		req_isp->cdm_reset_before_apply = false;
		list_del_init(&req->list);
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
	}

	return 0;
}

static inline void __cam_isp_ctx_reset_fcg_tracker(
	struct cam_context *ctx)
{
	struct cam_isp_context         *ctx_isp;
	struct cam_isp_skip_frame_info *skip_info;

	ctx_isp = (struct cam_isp_context *) ctx->ctx_priv;

	/* Reset skipped_list for FCG config */
	ctx_isp->fcg_tracker.sum_skipped = 0;
	ctx_isp->fcg_tracker.num_skipped = 0;
	list_for_each_entry(skip_info, &ctx_isp->fcg_tracker.skipped_list, list)
		skip_info->num_frame_skipped = 0;
	CAM_DBG(CAM_ISP, "Reset FCG skip info on ctx %u link: %x",
		ctx->ctx_id, ctx->link_hdl);
}

static int __cam_isp_ctx_flush_req_in_top_state(
	struct cam_context               *ctx,
	struct cam_req_mgr_flush_request *flush_req)
{
	int                               rc = 0;
	struct cam_isp_context           *ctx_isp;
	struct cam_isp_stop_args          stop_isp;
	struct cam_hw_stop_args           stop_args;
	struct cam_hw_reset_args          reset_args;
	struct cam_req_mgr_timer_notify   timer;

	ctx_isp = (struct cam_isp_context *) ctx->ctx_priv;

	/* Reset skipped_list for FCG config */
	__cam_isp_ctx_reset_fcg_tracker(ctx);

	if (flush_req->type == CAM_REQ_MGR_FLUSH_TYPE_ALL) {
		if (ctx->state <= CAM_CTX_READY) {
			ctx->state = CAM_CTX_ACQUIRED;
			goto end;
		}

		spin_lock_bh(&ctx->lock);
		ctx->state = CAM_CTX_FLUSHED;
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_HALT;
		spin_unlock_bh(&ctx->lock);

		CAM_INFO(CAM_ISP, "Last request id to flush is %lld, ctx_id:%u link: 0x%x",
			flush_req->req_id, ctx->ctx_id, ctx->link_hdl);
		ctx->last_flush_req = flush_req->req_id;

		__cam_isp_ctx_trigger_reg_dump(CAM_HW_MGR_CMD_REG_DUMP_ON_FLUSH, ctx);

		stop_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
		stop_isp.hw_stop_cmd = CAM_ISP_HW_STOP_IMMEDIATELY;
		stop_isp.stop_only = true;
		stop_isp.is_internal_stop = false;
		stop_args.args = (void *)&stop_isp;
		rc = ctx->hw_mgr_intf->hw_stop(ctx->hw_mgr_intf->hw_mgr_priv,
			&stop_args);
		if (rc)
			CAM_ERR(CAM_ISP, "Failed to stop HW in Flush rc: %d, ctx_id:%u link: 0x%x",
				rc, ctx->ctx_id, ctx->link_hdl);

		CAM_INFO(CAM_ISP, "Stop HW complete. Reset HW next.Ctx_id:%u link: 0x%x",
			 ctx->ctx_id, ctx->link_hdl);
		CAM_DBG(CAM_ISP, "Flush wait and active lists, ctx_id:%u link: 0x%x",
			 ctx->ctx_id, ctx->link_hdl);

		if (ctx->ctx_crm_intf && ctx->ctx_crm_intf->notify_timer) {
			timer.link_hdl = ctx->link_hdl;
			timer.dev_hdl = ctx->dev_hdl;
			timer.state = false;
			ctx->ctx_crm_intf->notify_timer(&timer);
		}

		spin_lock_bh(&ctx->lock);
		if (!list_empty(&ctx->wait_req_list))
			__cam_isp_ctx_flush_req(ctx, &ctx->wait_req_list,
				flush_req);

		if (!list_empty(&ctx->active_req_list))
			__cam_isp_ctx_flush_req(ctx, &ctx->active_req_list,
				flush_req);

		ctx_isp->active_req_cnt = 0;
		spin_unlock_bh(&ctx->lock);

		reset_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
		rc = ctx->hw_mgr_intf->hw_reset(ctx->hw_mgr_intf->hw_mgr_priv,
			&reset_args);
		if (rc)
			CAM_ERR(CAM_ISP, "Failed to reset HW rc: %d, ctx_id:%u link: 0x%x",
				 rc, ctx->ctx_id, ctx->link_hdl);

		ctx_isp->init_received = false;
	}

	CAM_DBG(CAM_ISP, "Flush pending list, ctx_idx: %u, link: 0x%x", ctx->ctx_id, ctx->link_hdl);
	/*
	 * On occasions when we are doing a flush all, HW would get reset
	 * shutting down any th/bh in the pipeline. If internal recovery
	 * is triggered prior to flush, by clearing the pending list post
	 * HW reset will ensure no stale request entities are left behind
	 */
	spin_lock_bh(&ctx->lock);
	__cam_isp_ctx_flush_req(ctx, &ctx->pending_req_list, flush_req);
	spin_unlock_bh(&ctx->lock);

end:
	ctx_isp->bubble_frame_cnt = 0;
	ctx_isp->congestion_cnt = 0;
	ctx_isp->sof_dbg_irq_en = false;
	atomic_set(&ctx_isp->process_bubble, 0);
	atomic_set(&ctx_isp->rxd_epoch, 0);
	atomic_set(&ctx_isp->internal_recovery_set, 0);
	return rc;
}

static int __cam_isp_ctx_flush_req_in_ready(
	struct cam_context *ctx,
	struct cam_req_mgr_flush_request *flush_req)
{
	int rc = 0;

	CAM_DBG(CAM_ISP, "try to flush pending list, ctx_id:%u link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);
	spin_lock_bh(&ctx->lock);
	rc = __cam_isp_ctx_flush_req(ctx, &ctx->pending_req_list, flush_req);

	/* if nothing is in pending req list, change state to acquire */
	if (list_empty(&ctx->pending_req_list))
		ctx->state = CAM_CTX_ACQUIRED;
	spin_unlock_bh(&ctx->lock);

	trace_cam_context_state("ISP", ctx);

	CAM_DBG(CAM_ISP, "Flush request in ready state. next state %d, ctx_id:%u link: 0x%x",
		 ctx->state, ctx->ctx_id, ctx->link_hdl);
	return rc;
}

static struct cam_ctx_ops
	cam_isp_ctx_activated_state_machine[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_apply_req_in_sof,
			.notify_frame_skip =
				__cam_isp_ctx_apply_default_req_settings,
		},
		.irq_ops = NULL,
	},
	/* APPLIED */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* EPOCH */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_apply_req_in_epoch,
			.notify_frame_skip =
				__cam_isp_ctx_apply_default_req_settings,
		},
		.irq_ops = NULL,
	},
	/* BUBBLE */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_apply_req_in_bubble,
			.notify_frame_skip =
				__cam_isp_ctx_apply_default_req_settings,
		},
		.irq_ops = NULL,
	},
	/* Bubble Applied */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.notify_frame_skip =
				__cam_isp_ctx_apply_default_req_settings,
		},
		.irq_ops = NULL,
	},
	/* HW ERROR */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* HALT */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
};

static struct cam_ctx_ops
	cam_isp_ctx_fs2_state_machine[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_apply_req_in_sof,
		},
		.irq_ops = NULL,
	},
	/* APPLIED */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* EPOCH */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_apply_req_in_epoch,
		},
		.irq_ops = NULL,
	},
	/* BUBBLE */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_apply_req_in_bubble,
		},
		.irq_ops = NULL,
	},
	/* Bubble Applied */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* HW ERROR */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* HALT */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
};

static int __cam_isp_ctx_rdi_only_sof_in_top_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	int rc = 0;
	struct cam_context                    *ctx = ctx_isp->base;
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;
	uint64_t                               request_id  = 0;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "in valid sof event data, ctx_idx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);

	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx, ctx_idx: %u, link: 0x%x",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val, ctx->ctx_id, ctx->link_hdl);

	/*
	 * notify reqmgr with sof signal. Note, due to scheduling delay
	 * we can run into situation that two active requests has already
	 * be in the active queue while we try to do the notification.
	 * In this case, we need to skip the current notification. This
	 * helps the state machine to catch up the delay.
	 */
	if (ctx_isp->active_req_cnt <= 2) {
		__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);

		/*
		 * It's possible for rup done to be processed before
		 * SOF, check for first active request shutter here
		 */
		if (!list_empty(&ctx->active_req_list)) {
			struct cam_ctx_request  *req = NULL;

			req = list_first_entry(&ctx->active_req_list,
				struct cam_ctx_request, list);
			if (req->request_id > ctx_isp->reported_req_id) {
				request_id = req->request_id;
				ctx_isp->reported_req_id = request_id;
			}
		}
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	} else {
		CAM_ERR_RATE_LIMIT(CAM_ISP, "Can not notify SOF to CRM, ctx_idx: %u, link: 0x%x",
				 ctx->ctx_id, ctx->link_hdl);
	}

	if (list_empty(&ctx->active_req_list))
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;
	else
		CAM_DBG(CAM_ISP, "Still need to wait for the buf done, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	CAM_DBG(CAM_ISP, "next Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	return rc;
}

static int __cam_isp_ctx_rdi_only_sof_in_applied_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "in valid sof event data");
		return -EINVAL;
	}

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);

	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val);

	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE_APPLIED;
	CAM_DBG(CAM_ISP, "next Substate[%s]",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated));

	return 0;
}

static int __cam_isp_ctx_rdi_only_sof_in_bubble_applied(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	struct cam_ctx_request    *req;
	struct cam_isp_ctx_req    *req_isp;
	struct cam_context        *ctx = ctx_isp->base;
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;
	uint64_t  request_id = 0;

	/*
	 * Sof in bubble applied state means, reg update not received.
	 * before increment frame id and override time stamp value, send
	 * the previous sof time stamp that got captured in the
	 * sof in applied state.
	 */
	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx, ctx_idx: %u, link: 0x%x",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val, ctx->ctx_id, ctx->link_hdl);
	__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
		CAM_REQ_MGR_SOF_EVENT_SUCCESS);

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);

	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx, ctx_idx: %u, link: 0x%x",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val, ctx->ctx_id, ctx->link_hdl);

	if (list_empty(&ctx->wait_req_list)) {
		/*
		 * If no pending req in epoch, this is an error case.
		 * The recovery is to go back to sof state
		 */
		CAM_ERR(CAM_ISP, "No wait request, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;

		/* Send SOF event as empty frame*/
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);
		goto end;
	}

	req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request,
		list);
	req_isp = (struct cam_isp_ctx_req *)req->req_priv;
	req_isp->bubble_detected = true;
	CAM_INFO_RATE_LIMIT(CAM_ISP, "Ctx:%u link: 0x%x Report Bubble flag %d req id:%lld",
		ctx->ctx_id, ctx->link_hdl, req_isp->bubble_report, req->request_id);
	req_isp->reapply_type = CAM_CONFIG_REAPPLY_IO;
	req_isp->cdm_reset_before_apply = false;

	if (req_isp->bubble_report) {
		__cam_isp_ctx_notify_error_util(CAM_TRIGGER_POINT_SOF, CRM_KMD_ERR_BUBBLE,
			req->request_id, ctx_isp);
		atomic_set(&ctx_isp->process_bubble, 1);
	} else {
		req_isp->bubble_report = 0;
	}

	/*
	 * Always move the request to active list. Let buf done
	 * function handles the rest.
	 */
	list_del_init(&req->list);
	list_add_tail(&req->list, &ctx->active_req_list);
	ctx_isp->active_req_cnt++;
	CAM_DBG(CAM_ISP, "move request %lld to active list(cnt = %d), ctx_idx: %u, link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);

	if (!req_isp->bubble_report) {
		if (req->request_id > ctx_isp->reported_req_id) {
			request_id = req->request_id;
			ctx_isp->reported_req_id = request_id;
			__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
				CAM_REQ_MGR_SOF_EVENT_ERROR);
		} else
			__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
				CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	} else
		__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
			CAM_REQ_MGR_SOF_EVENT_SUCCESS);

	/* change the state to bubble, as reg update has not come */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_BUBBLE;
	CAM_DBG(CAM_ISP, "next Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
end:
	return 0;
}

static int __cam_isp_ctx_rdi_only_sof_in_bubble_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	uint32_t i;
	struct cam_ctx_request                *req;
	struct cam_context                    *ctx = ctx_isp->base;
	struct cam_isp_hw_sof_event_data      *sof_event_data = evt_data;
	struct cam_isp_ctx_req                *req_isp;
	struct cam_hw_cmd_args                 hw_cmd_args;
	struct cam_isp_hw_cmd_args             isp_hw_cmd_args;
	uint64_t                               request_id  = 0;
	uint64_t                               last_cdm_done_req = 0;
	int                                    rc = 0;

	if (!evt_data) {
		CAM_ERR(CAM_ISP, "in valid sof event data, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);
	CAM_DBG(CAM_ISP, "frame id: %lld time stamp:0x%llx, ctx_idx: %u, link: 0x%x",
		ctx_isp->frame_id, ctx_isp->sof_timestamp_val, ctx->ctx_id, ctx->link_hdl);


	if (atomic_read(&ctx_isp->process_bubble)) {
		if (list_empty(&ctx->active_req_list)) {
			CAM_ERR(CAM_ISP, "No available active req in bubble, ctx: %u, link: 0x%x",
				ctx->ctx_id, ctx->link_hdl);
			atomic_set(&ctx_isp->process_bubble, 0);
			return -EINVAL;
		}

		if (ctx_isp->last_sof_timestamp ==
			ctx_isp->sof_timestamp_val) {
			CAM_DBG(CAM_ISP,
				"Tasklet delay detected! Bubble frame: %lld check skipped, sof_timestamp: %lld, ctx_id: %u, link: 0x%x",
				ctx_isp->frame_id,
				ctx_isp->sof_timestamp_val,
				ctx->ctx_id, ctx->link_hdl);
			goto end;
		}

		req = list_first_entry(&ctx->active_req_list,
				struct cam_ctx_request, list);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;

		if (req_isp->bubble_detected) {
			hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
			hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
			isp_hw_cmd_args.cmd_type =
				CAM_ISP_HW_MGR_GET_LAST_CDM_DONE;
			hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
			rc = ctx->hw_mgr_intf->hw_cmd(
				ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
			if (rc) {
				CAM_ERR(CAM_ISP, "HW command failed, ctx_id: %u, link: 0x%x",
					ctx->ctx_id, ctx->link_hdl);
				return rc;
			}

			last_cdm_done_req = isp_hw_cmd_args.u.last_cdm_done;
			CAM_DBG(CAM_ISP, "last_cdm_done req: %d ctx_id: %u, link: 0x%x",
				last_cdm_done_req, ctx->ctx_id, ctx->link_hdl);

			if (last_cdm_done_req >= req->request_id) {
				CAM_DBG(CAM_ISP,
					"CDM callback detected for req: %lld, possible buf_done delay, waiting for buf_done, ctx_id: %u, link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				if (req_isp->num_fence_map_out ==
					req_isp->num_deferred_acks) {
					__cam_isp_handle_deferred_buf_done(ctx_isp, req,
						true,
						CAM_SYNC_STATE_SIGNALED_ERROR,
						CAM_SYNC_ISP_EVENT_BUBBLE);

					__cam_isp_ctx_handle_buf_done_for_req_list(
						ctx_isp, req);
				}
				goto end;
			} else {
				CAM_WARN(CAM_ISP,
					"CDM callback not happened for req: %lld, possible CDM stuck or workqueue delay, ctx_id: %u, link: 0x%x",
					req->request_id, ctx->ctx_id, ctx->link_hdl);
				req_isp->num_acked = 0;
				req_isp->num_deferred_acks = 0;
				req_isp->bubble_detected = false;
				req_isp->cdm_reset_before_apply = true;
				list_del_init(&req->list);
				list_add(&req->list, &ctx->pending_req_list);
				atomic_set(&ctx_isp->process_bubble, 0);
				ctx_isp->active_req_cnt--;
				CAM_DBG(CAM_REQ,
					"Move active req: %lld to pending list(cnt = %d) [bubble re-apply],ctx %u link: 0x%x",
					req->request_id,
					ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
			}
			goto end;
		}
	}

	/*
	 * Signal all active requests with error and move the  all the active
	 * requests to free list
	 */
	while (!list_empty(&ctx->active_req_list)) {
		req = list_first_entry(&ctx->active_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in active list. fence num %d, ctx %u link: 0x%x",
			req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_ERROR,
					CAM_SYNC_ISP_EVENT_BUBBLE);
			}
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
		ctx_isp->active_req_cnt--;
	}

end:
	/* notify reqmgr with sof signal */
	__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);

	/*
	 * It is idle frame with out any applied request id, send
	 * request id as zero
	 */
	__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
		CAM_REQ_MGR_SOF_EVENT_SUCCESS);

	/*
	 * Can't move the substate to SOF if we are processing bubble,
	 * since the SOF substate can't receive REG_UPD and buf done,
	 * then the processing of bubble req can't be finished
	 */
	if (!atomic_read(&ctx_isp->process_bubble))
		ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;

	CAM_DBG(CAM_ISP, "next Substate[%s], ctx %u link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	ctx_isp->last_sof_timestamp = ctx_isp->sof_timestamp_val;
	return 0;
}


static int __cam_isp_ctx_rdi_only_reg_upd_in_bubble_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	struct cam_ctx_request  *req = NULL;
	struct cam_context      *ctx = ctx_isp->base;

	req = list_first_entry(&ctx->active_req_list,
		struct cam_ctx_request, list);

	CAM_INFO(CAM_ISP, "Received RUP for Bubble Request, ctx %u link: 0x%x",
		req->request_id, ctx->ctx_id, ctx->link_hdl);

	return 0;
}

static int __cam_isp_ctx_rdi_only_reg_upd_in_bubble_applied_state(
	struct cam_isp_context *ctx_isp, void *evt_data)
{
	struct cam_ctx_request  *req = NULL;
	struct cam_context      *ctx = ctx_isp->base;
	struct cam_isp_ctx_req  *req_isp;
	uint64_t  request_id  = 0;

	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_EPOCH;
	/* notify reqmgr with sof signal*/
	if (list_empty(&ctx->wait_req_list)) {
		CAM_ERR(CAM_ISP, "Reg upd ack with no waiting request, ctx %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto error;
	}

	req = list_first_entry(&ctx->wait_req_list,
			struct cam_ctx_request, list);
	list_del_init(&req->list);

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;
	request_id =
		(req_isp->hw_update_data.packet_opcode_type ==
		CAM_ISP_PACKET_INIT_DEV) ? 0 : req->request_id;

	if (req_isp->num_fence_map_out != 0) {
		list_add_tail(&req->list, &ctx->active_req_list);
		ctx_isp->active_req_cnt++;
		CAM_DBG(CAM_ISP,
			"move request %lld to active list(cnt = %d), ctx %u link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
		/* if packet has buffers, set correct request id */
		request_id = req->request_id;
	} else {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		/* no io config, so the request is completed. */
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
		CAM_DBG(CAM_ISP,
			"move active req %lld to free list(cnt=%d), ctx %u link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
	}

	__cam_isp_ctx_notify_trigger_util(CAM_TRIGGER_POINT_SOF, ctx_isp);

	if (request_id)
		ctx_isp->reported_req_id = request_id;

	__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
		CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	CAM_DBG(CAM_ISP, "next Substate[%s], ctx %u link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_RUP, req, NULL);
	return 0;
error:
	/* Send SOF event as idle frame*/
	__cam_isp_ctx_send_sof_timestamp(ctx_isp, request_id,
		CAM_REQ_MGR_SOF_EVENT_SUCCESS);
	__cam_isp_ctx_update_event_record(ctx_isp,
		CAM_ISP_CTX_EVENT_RUP, NULL, NULL);

	/*
	 * There is no request in the pending list, move the sub state machine
	 * to SOF sub state
	 */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;

	return 0;
}

static struct cam_isp_ctx_irq_ops
	cam_isp_ctx_rdi_only_activated_state_machine_irq
			[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.irq_ops = {
			NULL,
			__cam_isp_ctx_rdi_only_sof_in_top_state,
			__cam_isp_ctx_reg_upd_in_sof,
			NULL,
			__cam_isp_ctx_notify_eof_in_activated_state,
			NULL,
		},
	},
	/* APPLIED */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_rdi_only_sof_in_applied_state,
			__cam_isp_ctx_reg_upd_in_applied_state,
			NULL,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_applied,
		},
	},
	/* EPOCH */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_rdi_only_sof_in_top_state,
			NULL,
			NULL,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_epoch,
		},
	},
	/* BUBBLE*/
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_rdi_only_sof_in_bubble_state,
			__cam_isp_ctx_rdi_only_reg_upd_in_bubble_state,
			NULL,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_bubble,
		},
	},
	/* BUBBLE APPLIED ie PRE_BUBBLE */
	{
		.irq_ops = {
			__cam_isp_ctx_handle_error,
			__cam_isp_ctx_rdi_only_sof_in_bubble_applied,
			__cam_isp_ctx_rdi_only_reg_upd_in_bubble_applied_state,
			NULL,
			__cam_isp_ctx_notify_eof_in_activated_state,
			__cam_isp_ctx_buf_done_in_bubble_applied,
		},
	},
	/* HW ERROR */
	{
	},
	/* HALT */
	{
	},
};

static int __cam_isp_ctx_rdi_only_apply_req_top_state(
	struct cam_context *ctx, struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	CAM_DBG(CAM_ISP, "current Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
	rc = __cam_isp_ctx_apply_req_in_activated_state(ctx, apply,
		CAM_ISP_CTX_ACTIVATED_APPLIED);
	CAM_DBG(CAM_ISP, "new Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	if (rc)
		CAM_ERR_RATE_LIMIT(CAM_ISP,
			"ctx_id:%u link: 0x%x Apply failed in Substate[%s], rc %d",
			ctx->ctx_id, ctx->link_hdl,
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), rc);

	return rc;
}

static struct cam_ctx_ops
	cam_isp_ctx_rdi_only_activated_state_machine
		[CAM_ISP_CTX_ACTIVATED_MAX] = {
	/* SOF */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_rdi_only_apply_req_top_state,
		},
		.irq_ops = NULL,
	},
	/* APPLIED */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* EPOCH */
	{
		.ioctl_ops = {},
		.crm_ops = {
			.apply_req = __cam_isp_ctx_rdi_only_apply_req_top_state,
		},
		.irq_ops = NULL,
	},
	/* PRE BUBBLE */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* BUBBLE */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* HW ERROR */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* HALT */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
};

static int __cam_isp_ctx_flush_dev_in_top_state(struct cam_context *ctx,
	struct cam_flush_dev_cmd *cmd)
{
	struct cam_isp_context *ctx_isp = ctx->ctx_priv;
	struct cam_req_mgr_flush_request flush_req;

	if (!ctx_isp->offline_context) {
		CAM_ERR(CAM_ISP, "flush dev only supported in offline context,ctx: %u, link:0x%x",
			ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	flush_req.type = (cmd->flush_type == CAM_FLUSH_TYPE_ALL) ? CAM_REQ_MGR_FLUSH_TYPE_ALL :
			CAM_REQ_MGR_FLUSH_TYPE_CANCEL_REQ;
	flush_req.req_id = cmd->req_id;

	CAM_DBG(CAM_ISP, "offline flush (type:%u, req:%lu), ctx_idx: %u, link: 0x%x",
		flush_req.type, flush_req.req_id, ctx->ctx_id, ctx->link_hdl);

	switch (ctx->state) {
	case CAM_CTX_ACQUIRED:
	case CAM_CTX_ACTIVATED:
		return __cam_isp_ctx_flush_req_in_top_state(ctx, &flush_req);
	case CAM_CTX_READY:
		return __cam_isp_ctx_flush_req_in_ready(ctx, &flush_req);
	default:
		CAM_ERR(CAM_ISP, "flush dev in wrong state: %d, ctx_idx: %u, link: 0x%x",
			ctx->state, ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	if (cmd->flush_type == CAM_FLUSH_TYPE_ALL)
		cam_req_mgr_workq_flush(ctx_isp->workq);
}

static void __cam_isp_ctx_free_mem_hw_entries(struct cam_context *ctx)
{
	int  i;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	if (ctx->out_map_entries) {
		for (i = 0; i < CAM_ISP_CTX_REQ_MAX; i++) {
			kfree(ctx->out_map_entries[i]);
			ctx->out_map_entries[i] = NULL;
		}

		kfree(ctx->out_map_entries);
		ctx->out_map_entries = NULL;
	}

	if (ctx->in_map_entries) {
		for (i = 0; i < CAM_ISP_CTX_REQ_MAX; i++) {
			kfree(ctx->in_map_entries[i]);
			ctx->in_map_entries[i] = NULL;
		}

		kfree(ctx->in_map_entries);
		ctx->in_map_entries = NULL;
	}

	if (ctx->hw_update_entry) {
		for (i = 0; i < CAM_ISP_CTX_REQ_MAX; i++) {
			kfree(ctx->hw_update_entry[i]);
			ctx->hw_update_entry[i] = NULL;
		}

		kfree(ctx->hw_update_entry);
		ctx->hw_update_entry = NULL;
	}

	if (ctx_isp) {
		for (i = 0; i < CAM_ISP_CTX_REQ_MAX; i++) {
			kfree(ctx_isp->req_isp[i].deferred_fence_map_index);
			ctx_isp->req_isp[i].deferred_fence_map_index = NULL;
		}
	}

	ctx->max_out_map_entries = 0;
	ctx->max_in_map_entries = 0;
	ctx->max_hw_update_entries = 0;
}

static int __cam_isp_ctx_release_hw_in_top_state(struct cam_context *ctx,
	void *cmd)
{
	int rc = 0;
	struct cam_hw_release_args       rel_arg;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_req_mgr_flush_request flush_req;
	int i;

	if (ctx_isp->hw_ctx) {
		rel_arg.ctxt_to_hw_map = ctx_isp->hw_ctx;
		ctx->hw_mgr_intf->hw_release(ctx->hw_mgr_intf->hw_mgr_priv,
			&rel_arg);
		ctx_isp->hw_ctx = NULL;
	} else {
		CAM_ERR(CAM_ISP, "No hw resources acquired for ctx[%u], link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
	}

	ctx->last_flush_req = 0;
	ctx_isp->custom_enabled = false;
	ctx_isp->use_frame_header_ts = false;
	ctx_isp->use_default_apply = false;
	ctx_isp->frame_id = 0;
	ctx_isp->active_req_cnt = 0;
	ctx_isp->reported_req_id = 0;
	ctx_isp->reported_frame_id = 0;
	ctx_isp->hw_acquired = false;
	ctx_isp->init_received = false;
	ctx_isp->support_consumed_addr = false;
	ctx_isp->aeb_enabled = false;
	ctx_isp->req_info.last_bufdone_req_id = 0;
	kfree(ctx_isp->vfe_bus_comp_grp);
	kfree(ctx_isp->sfe_bus_comp_grp);
	ctx_isp->vfe_bus_comp_grp = NULL;
	ctx_isp->sfe_bus_comp_grp = NULL;

	atomic64_set(&ctx_isp->dbg_monitors.state_monitor_head, -1);
	atomic64_set(&ctx_isp->dbg_monitors.frame_monitor_head, -1);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->dbg_monitors.event_record_head[i], -1);
	/*
	 * Ideally, we should never have any active request here.
	 * But we still add some sanity check code here to help the debug
	 */
	if (!list_empty(&ctx->active_req_list))
		CAM_WARN(CAM_ISP, "Active list is not empty, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	/* Flush all the pending request list  */
	flush_req.type = CAM_REQ_MGR_FLUSH_TYPE_ALL;
	flush_req.link_hdl = ctx->link_hdl;
	flush_req.dev_hdl = ctx->dev_hdl;
	flush_req.req_id = 0;

	CAM_DBG(CAM_ISP, "try to flush pending list, ctx_idx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);
	spin_lock_bh(&ctx->lock);
	rc = __cam_isp_ctx_flush_req(ctx, &ctx->pending_req_list, &flush_req);
	spin_unlock_bh(&ctx->lock);
	__cam_isp_ctx_free_mem_hw_entries(ctx);
	cam_req_mgr_workq_destroy(&ctx_isp->workq);
	ctx->state = CAM_CTX_ACQUIRED;

	trace_cam_context_state("ISP", ctx);
	CAM_DBG(CAM_ISP, "Release device success[%u] link: 0x%x next state %d",
		ctx->ctx_id, ctx->link_hdl, ctx->state);
	return rc;
}

/* top level state machine */
static int __cam_isp_ctx_release_dev_in_top_state(struct cam_context *ctx,
	struct cam_release_dev_cmd *cmd)
{
	int rc = 0;
	int i;
	struct cam_hw_release_args       rel_arg;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_req_mgr_flush_request flush_req;

	if (cmd && ctx_isp->hw_ctx) {
		CAM_ERR(CAM_ISP, "releasing hw, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		__cam_isp_ctx_release_hw_in_top_state(ctx, NULL);
	}

	if (ctx_isp->hw_ctx) {
		rel_arg.ctxt_to_hw_map = ctx_isp->hw_ctx;
		ctx->hw_mgr_intf->hw_release(ctx->hw_mgr_intf->hw_mgr_priv,
			&rel_arg);
		ctx_isp->hw_ctx = NULL;
	}

	cam_common_release_evt_params(ctx->dev_hdl);
	memset(&ctx_isp->evt_inject_params, 0, sizeof(struct cam_hw_inject_evt_param));

	ctx->session_hdl = -1;
	ctx->dev_hdl = -1;
	ctx->link_hdl = -1;
	ctx->ctx_crm_intf = NULL;
	ctx->last_flush_req = 0;
	ctx_isp->frame_id = 0;
	ctx_isp->active_req_cnt = 0;
	ctx_isp->reported_req_id = 0;
	ctx_isp->reported_frame_id = 0;
	ctx_isp->hw_acquired = false;
	ctx_isp->init_received = false;
	ctx_isp->offline_context = false;
	ctx_isp->vfps_aux_context = false;
	ctx_isp->rdi_only_context = false;
	ctx_isp->req_info.last_bufdone_req_id = 0;
	ctx_isp->v4l2_event_sub_ids = 0;
	ctx_isp->resume_hw_in_flushed = false;

	atomic64_set(&ctx_isp->dbg_monitors.state_monitor_head, -1);
	atomic64_set(&ctx_isp->dbg_monitors.frame_monitor_head, -1);
	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->dbg_monitors.event_record_head[i], -1);
	/*
	 * Ideally, we should never have any active request here.
	 * But we still add some sanity check code here to help the debug
	 */
	if (!list_empty(&ctx->active_req_list))
		CAM_ERR(CAM_ISP, "Active list is not empty, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	/* Flush all the pending request list  */
	flush_req.type = CAM_REQ_MGR_FLUSH_TYPE_ALL;
	flush_req.link_hdl = ctx->link_hdl;
	flush_req.dev_hdl = ctx->dev_hdl;
	flush_req.req_id = 0;

	CAM_DBG(CAM_ISP, "try to flush pending list, ctx_idx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);
	spin_lock_bh(&ctx->lock);
	rc = __cam_isp_ctx_flush_req(ctx, &ctx->pending_req_list, &flush_req);
	spin_unlock_bh(&ctx->lock);
	__cam_isp_ctx_free_mem_hw_entries(ctx);

	ctx->state = CAM_CTX_AVAILABLE;

	trace_cam_context_state("ISP", ctx);
	CAM_DBG(CAM_ISP, "Release device success[%u] link: 0x%x next state %d",
		ctx->ctx_id, ctx->link_hdl, ctx->state);
	return rc;
}

static int __cam_isp_ctx_config_dev_in_top_state(
	struct cam_context *ctx, struct cam_config_dev_cmd *cmd)
{
	int rc = 0, i;
	struct cam_ctx_request           *req = NULL;
	struct cam_isp_ctx_req           *req_isp;
	struct cam_packet                *packet;
	size_t                            remain_len = 0;
	struct cam_hw_prepare_update_args cfg = {0};
	struct cam_isp_prepare_hw_update_data *hw_update_data;
	struct cam_req_mgr_add_request    add_req;
	struct cam_isp_context           *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_hw_cmd_args           hw_cmd_args;
	struct cam_isp_hw_cmd_args       isp_hw_cmd_args;
	uint32_t                         packet_opcode = 0;
	struct cam_kmd_buf_info *kmd_buff = NULL;

	CAM_DBG(CAM_ISP, "get free request object......ctx_idx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);

	/* get free request */
	spin_lock_bh(&ctx->lock);
	if (!list_empty(&ctx->free_req_list)) {
		req = list_first_entry(&ctx->free_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
	}
	spin_unlock_bh(&ctx->lock);

	if (!req) {
		CAM_ERR(CAM_ISP, "No more request obj free, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		return -ENOMEM;
	}

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	remain_len = cam_context_parse_config_cmd(ctx, cmd, &packet);
	if (IS_ERR_OR_NULL(packet)) {
		rc = PTR_ERR(packet);
		goto free_req;
	}

	/* Query the packet opcode */
	hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_GET_PACKET_OPCODE;
	isp_hw_cmd_args.cmd_data = (void *)packet;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "HW command failed, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_packet;
	}

	packet_opcode = isp_hw_cmd_args.u.packet_op_code;
	if ((packet_opcode == CAM_ISP_PACKET_UPDATE_DEV)
		&& (packet->header.request_id <= ctx->last_flush_req)) {
		CAM_INFO(CAM_ISP,
			"request %lld has been flushed, reject packet, ctx_idx: %u, link: 0x%x",
			packet->header.request_id, ctx->ctx_id, ctx->link_hdl);
		rc = -EBADR;
		goto free_packet;
	} else if ((packet_opcode == CAM_ISP_PACKET_INIT_DEV)
		&& (packet->header.request_id <= ctx->last_flush_req)
		&& ctx->last_flush_req && packet->header.request_id) {
		CAM_WARN(CAM_ISP,
			"last flushed req is %lld, config dev(init) for req %lld, ctx_idx: %u, link: 0x%x",
			ctx->last_flush_req, packet->header.request_id, ctx->ctx_id, ctx->link_hdl);
		rc = -EBADR;
		goto free_packet;
	}

	cfg.packet = packet;
	cfg.remain_len = remain_len;
	cfg.ctxt_to_hw_map = ctx_isp->hw_ctx;
	cfg.max_hw_update_entries = ctx->max_hw_update_entries;
	cfg.hw_update_entries = req_isp->cfg;
	cfg.max_out_map_entries = ctx->max_out_map_entries;
	cfg.max_in_map_entries = ctx->max_in_map_entries;
	cfg.out_map_entries = req_isp->fence_map_out;
	cfg.in_map_entries = req_isp->fence_map_in;
	cfg.priv  = &req_isp->hw_update_data;
	cfg.pf_data = &(req->pf_data);
	cfg.num_out_map_entries = 0;
	cfg.num_in_map_entries = 0;
	cfg.buf_tracker = &req->buf_tracker;
	memset(&req_isp->hw_update_data, 0, sizeof(req_isp->hw_update_data));
	memset(req_isp->fence_map_out, 0, sizeof(struct cam_hw_fence_map_entry)
		* ctx->max_out_map_entries);

	INIT_LIST_HEAD(cfg.buf_tracker);

	rc = ctx->hw_mgr_intf->hw_prepare_update(
		ctx->hw_mgr_intf->hw_mgr_priv, &cfg);
	if (rc != 0) {
		CAM_ERR(CAM_ISP, "Prepare config packet failed in HW layer, ctx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto free_req_and_buf_tracker_list;
	}

	hw_update_data = cfg.priv;
	req_isp->num_cfg = cfg.num_hw_update_entries;
	req_isp->num_fence_map_out = cfg.num_out_map_entries;
	req_isp->num_fence_map_in = cfg.num_in_map_entries;
	req_isp->num_acked = 0;
	req_isp->num_deferred_acks = 0;
	req_isp->bubble_detected = false;
	req_isp->cdm_reset_before_apply = false;
	req_isp->hw_update_data.packet = packet;
	req_isp->hw_update_data.num_exp = hw_update_data->num_exp;
	req_isp->hw_update_data.mup_en = hw_update_data->mup_en;
	req->pf_data.packet_handle = cmd->packet_handle;
	req->pf_data.packet_offset = cmd->offset;
	req->pf_data.req = req;
	req->packet = packet;

	for (i = 0; i < req_isp->num_fence_map_out; i++) {
		rc = cam_sync_get_obj_ref(req_isp->fence_map_out[i].sync_id);
		if (rc) {
			CAM_ERR(CAM_ISP, "Can't get ref for fence %d, ctx_idx: %u, link: 0x%x",
				req_isp->fence_map_out[i].sync_id, ctx->ctx_id, ctx->link_hdl);
			goto put_ref;
		}
	}

	CAM_DBG(CAM_ISP,
		"packet req-id:%lld, opcode:%d, num_entry:%d, num_fence_out: %d, num_fence_in: %d, ctx: %u, link: 0x%x",
		packet->header.request_id, req_isp->hw_update_data.packet_opcode_type,
		req_isp->num_cfg, req_isp->num_fence_map_out, req_isp->num_fence_map_in,
		ctx->ctx_id, ctx->link_hdl);

	req->request_id = packet->header.request_id;
	req->status = 1;

	if (req_isp->hw_update_data.packet_opcode_type ==
		CAM_ISP_PACKET_INIT_DEV) {
		if (ctx->state < CAM_CTX_ACTIVATED) {
			rc = __cam_isp_ctx_enqueue_init_request(ctx, req);
			if (rc)
				CAM_ERR(CAM_ISP, "Enqueue INIT pkt failed, ctx: %u, link: 0x%x",
					ctx->ctx_id, ctx->link_hdl);
			ctx_isp->init_received = true;

			if ((ctx_isp->vfps_aux_context) && (req->request_id > 0))
				ctx_isp->resume_hw_in_flushed = true;
			else
				ctx_isp->resume_hw_in_flushed = false;
		} else {
			rc = -EINVAL;
			CAM_ERR(CAM_ISP, "Received INIT pkt in wrong state:%d, ctx:%u, link:0x%x",
				ctx->state, ctx->ctx_id, ctx->link_hdl);
		}
	} else {
		if ((ctx->state == CAM_CTX_FLUSHED) || (ctx->state < CAM_CTX_READY)) {
			rc = -EINVAL;
			CAM_ERR(CAM_ISP,
			    "Received update req %lld in wrong state:%d, ctx_idx: %u, link: 0x%x",
			    req->request_id, ctx->state, ctx->ctx_id, ctx->link_hdl);
			goto put_ref;
		}

		if ((ctx_isp->offline_context) || (ctx_isp->vfps_aux_context)) {
			__cam_isp_ctx_enqueue_request_in_order(ctx, req, true);
		} else if (ctx->ctx_crm_intf->add_req) {
			memset(&add_req, 0, sizeof(add_req));
			add_req.link_hdl = ctx->link_hdl;
			add_req.dev_hdl  = ctx->dev_hdl;
			add_req.req_id   = req->request_id;
			add_req.num_exp = ctx_isp->last_num_exp;

			if (req_isp->hw_update_data.mup_en) {
				add_req.num_exp = req_isp->hw_update_data.num_exp;
				ctx_isp->last_num_exp = add_req.num_exp;
			}
			rc = ctx->ctx_crm_intf->add_req(&add_req);
			if (rc) {
				if (rc == -EBADR)
					CAM_INFO(CAM_ISP,
						"Add req failed: req id=%llu, it has been flushed on link 0x%x ctx %u",
						req->request_id, ctx->link_hdl, ctx->ctx_id);
				else
					CAM_ERR(CAM_ISP,
						"Add req failed: req id=%llu on link 0x%x ctx %u",
						req->request_id, ctx->link_hdl, ctx->ctx_id);
			} else {
				__cam_isp_ctx_enqueue_request_in_order(ctx, req, true);
			}
		} else {
			CAM_ERR(CAM_ISP, "Unable to add request: req id=%llu,ctx: %u,link: 0x%x",
				req->request_id, ctx->ctx_id, ctx->link_hdl);
			rc = -ENODEV;
		}
	}
	if (rc)
		goto put_ref;

	CAM_DBG(CAM_REQ,
		"Preprocessing Config req_id %lld successful on ctx %u, link: 0x%x",
		req->request_id, ctx->ctx_id, ctx->link_hdl);

	if (ctx_isp->offline_context && atomic_read(&ctx_isp->rxd_epoch))
		__cam_isp_ctx_schedule_apply_req(ctx_isp);
	else if (ctx_isp->vfps_aux_context &&
		(req_isp->hw_update_data.packet_opcode_type != CAM_ISP_PACKET_INIT_DEV))
		__cam_isp_ctx_schedule_apply_req(ctx_isp);

	return rc;

put_ref:
	for (--i; i >= 0; i--) {
		if (cam_sync_put_obj_ref(req_isp->fence_map_out[i].sync_id))
			CAM_ERR(CAM_CTXT, "Failed to put ref of fence %d, ctx_idx: %u, link: 0x%x",
				req_isp->fence_map_out[i].sync_id, ctx->ctx_id, ctx->link_hdl);
	}
free_req_and_buf_tracker_list:
	cam_smmu_buffer_tracker_putref(&req->buf_tracker);
	kmd_buff = &(req_isp->hw_update_data.kmd_cmd_buff_info);
	cam_mem_put_kref(kmd_buff->handle);
free_packet:
	cam_common_mem_free(packet);
	req->packet = NULL;
free_req:
	spin_lock_bh(&ctx->lock);
	__cam_isp_ctx_move_req_to_free_list(ctx, req);
	spin_unlock_bh(&ctx->lock);

	return rc;
}

static int __cam_isp_ctx_allocate_mem_hw_entries(
	struct cam_context *ctx,
	struct cam_hw_acquire_args *param)
{
	int rc = 0, i;
	uint32_t max_res = 0;
	uint32_t max_hw_upd_entries = CAM_ISP_CTX_CFG_MAX;
	struct cam_ctx_request          *req;
	struct cam_ctx_request          *temp_req;
	struct cam_isp_ctx_req          *req_isp;
	struct cam_isp_context          *ctx_isp =
			(struct cam_isp_context *) ctx->ctx_priv;

	if (!param->op_params.param_list[0])
		max_res = CAM_ISP_CTX_RES_MAX;
	else {
		max_res = param->op_params.param_list[0];
		if (param->op_flags & CAM_IFE_CTX_SFE_EN) {
			max_res += param->op_params.param_list[1];
			max_hw_upd_entries = CAM_ISP_SFE_CTX_CFG_MAX;
		}
	}

	ctx->max_in_map_entries    = max_res;
	ctx->max_out_map_entries   = max_res;
	ctx->max_hw_update_entries = max_hw_upd_entries;

	CAM_DBG(CAM_ISP,
		"Allocate max_entries: 0x%x max_res: 0x%x is_sfe_en: %d, ctx: %u, link: 0x%x",
		max_hw_upd_entries, max_res, (param->op_flags & CAM_IFE_CTX_SFE_EN),
		ctx->ctx_id, ctx->link_hdl);

	ctx->hw_update_entry = kcalloc(CAM_ISP_CTX_REQ_MAX, sizeof(struct cam_hw_update_entry *),
		GFP_KERNEL);

	if (!ctx->hw_update_entry) {
		CAM_ERR(CAM_CTXT, "%s[%u] no memory, link: 0x%x",
			ctx->dev_name, ctx->ctx_id, ctx->link_hdl);
		return -ENOMEM;
	}

	ctx->in_map_entries = kcalloc(CAM_ISP_CTX_REQ_MAX, sizeof(struct cam_hw_fence_map_entry *),
		GFP_KERNEL);

	if (!ctx->in_map_entries) {
		CAM_ERR(CAM_CTXT, "%s[%u] no memory for in_map_entries, link: 0x%x",
			ctx->dev_name, ctx->ctx_id, ctx->link_hdl);
		rc = -ENOMEM;
		goto end;
	}

	ctx->out_map_entries = kcalloc(CAM_ISP_CTX_REQ_MAX, sizeof(struct cam_hw_fence_map_entry *),
			GFP_KERNEL);

	if (!ctx->out_map_entries) {
		CAM_ERR(CAM_CTXT, "%s[%u] no memory for out_map_entries, link: 0x%x",
			ctx->dev_name, ctx->ctx_id, ctx->link_hdl);
		rc = -ENOMEM;
		goto end;
	}


	for (i = 0; i < CAM_ISP_CTX_REQ_MAX; i++) {
		ctx->hw_update_entry[i] = kcalloc(ctx->max_hw_update_entries,
			sizeof(struct cam_hw_update_entry), GFP_KERNEL);

		if (!ctx->hw_update_entry[i]) {
			CAM_ERR(CAM_CTXT, "%s[%u] no memory for hw_update_entry: %u, link: 0x%x",
				ctx->dev_name, ctx->ctx_id, i, ctx->link_hdl);
			rc = -ENOMEM;
			goto end;
		}

		ctx->in_map_entries[i] = kcalloc(ctx->max_in_map_entries,
			sizeof(struct cam_hw_fence_map_entry),
			GFP_KERNEL);

		if (!ctx->in_map_entries[i]) {
			CAM_ERR(CAM_CTXT, "%s[%u] no memory for in_map_entries: %u, link: 0x%x",
				ctx->dev_name, ctx->ctx_id, i, ctx->link_hdl);
			rc = -ENOMEM;
			goto end;
		}

		ctx->out_map_entries[i] = kcalloc(ctx->max_out_map_entries,
			sizeof(struct cam_hw_fence_map_entry),
			GFP_KERNEL);

		if (!ctx->out_map_entries[i]) {
			CAM_ERR(CAM_CTXT, "%s[%u] no memory for out_map_entries: %u, link: 0x%x",
				ctx->dev_name, ctx->ctx_id, i, ctx->link_hdl);
			rc = -ENOMEM;
			goto end;
		}

		ctx_isp->req_isp[i].deferred_fence_map_index = kcalloc(param->total_ports_acq,
			sizeof(uint32_t), GFP_KERNEL);

		if (!ctx_isp->req_isp[i].deferred_fence_map_index) {
			CAM_ERR(CAM_ISP, "%s[%d] no memory for defer fence map idx arr, ports:%u",
				ctx->dev_name, ctx->ctx_id, param->total_ports_acq);
			rc = -ENOMEM;
			goto end;
		}
	}

	list_for_each_entry_safe(req, temp_req,
		&ctx->free_req_list, list) {
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;

		req_isp->cfg = ctx->hw_update_entry[req->index];
		req_isp->fence_map_in = ctx->in_map_entries[req->index];
		req_isp->fence_map_out = ctx->out_map_entries[req->index];
	}

	return rc;

end:
	__cam_isp_ctx_free_mem_hw_entries(ctx);

	return rc;
}

static int __cam_isp_ctx_acquire_dev_in_available(struct cam_context *ctx,
	struct cam_acquire_dev_cmd *cmd)
{
	int rc = 0;
	int i;
	struct cam_hw_acquire_args       param;
	struct cam_isp_resource         *isp_res = NULL;
	struct cam_create_dev_hdl        req_hdl_param;
	struct cam_hw_release_args       release;
	struct cam_isp_context          *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_hw_cmd_args           hw_cmd_args;
	struct cam_isp_hw_cmd_args       isp_hw_cmd_args;

	if (!ctx->hw_mgr_intf) {
		CAM_ERR(CAM_ISP, "HW interface is not ready, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	CAM_DBG(CAM_ISP,
		"session_hdl 0x%x, num_resources %d, hdl type %d, res %lld, ctx_idx: %u, link: 0x%x",
		cmd->session_handle, cmd->num_resources,
		cmd->handle_type, cmd->resource_hdl, ctx->ctx_id, ctx->link_hdl);

	ctx_isp->v4l2_event_sub_ids = cam_req_mgr_get_id_subscribed();

	if (cmd->num_resources == CAM_API_COMPAT_CONSTANT) {
		ctx_isp->split_acquire = true;
		CAM_DBG(CAM_ISP, "Acquire dev handle, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto get_dev_handle;
	}

	if (cmd->num_resources > CAM_ISP_CTX_RES_MAX) {
		CAM_ERR(CAM_ISP, "Too much resources in the acquire, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -ENOMEM;
		goto end;
	}

	/* for now we only support user pointer */
	if (cmd->handle_type != 1)  {
		CAM_ERR(CAM_ISP, "Only user pointer is supported, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
		goto end;
	}

	isp_res = kzalloc(
		sizeof(*isp_res)*cmd->num_resources, GFP_KERNEL);
	if (!isp_res) {
		rc = -ENOMEM;
		goto end;
	}

	CAM_DBG(CAM_ISP, "start copy %d resources from user, ctx_idx: %u, link: 0x%x",
		 cmd->num_resources, ctx->ctx_id, ctx->link_hdl);

	if (copy_from_user(isp_res, u64_to_user_ptr(cmd->resource_hdl),
		sizeof(*isp_res)*cmd->num_resources)) {
		rc = -EFAULT;
		goto free_res;
	}

	memset(&param, 0, sizeof(param));
	param.context_data = ctx;
	param.event_cb = ctx->irq_cb_intf;
	param.sec_pf_evt_cb = cam_context_dump_pf_info;
	param.num_acq = cmd->num_resources;
	param.acquire_info = (uintptr_t) isp_res;

	/* call HW manager to reserve the resource */
	rc = ctx->hw_mgr_intf->hw_acquire(ctx->hw_mgr_intf->hw_mgr_priv,
		&param);
	if (rc != 0) {
		CAM_ERR(CAM_ISP, "Acquire device failed, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_res;
	}

	rc = __cam_isp_ctx_allocate_mem_hw_entries(ctx, &param);
	if (rc) {
		CAM_ERR(CAM_ISP, "Ctx[%u] link: 0x%x allocate hw entry fail",
			ctx->ctx_id, ctx->link_hdl);
		goto free_res;
	}

	/* Query the context has rdi only resource */
	hw_cmd_args.ctxt_to_hw_map = param.ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_CTX_TYPE;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "HW command failed, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_hw;
	}

	if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_RDI) {
		/*
		 * this context has rdi only resource assign rdi only
		 * state machine
		 */
		CAM_DBG(CAM_ISP, "RDI only session Context, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

		ctx_isp->substate_machine_irq =
			cam_isp_ctx_rdi_only_activated_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_rdi_only_activated_state_machine;
		ctx_isp->rdi_only_context = true;
	} else if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_FS2) {
		CAM_DBG(CAM_ISP, "FS2 Session has PIX, RD and RDI, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_fs2_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_fs2_state_machine;
	} else if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_OFFLINE) {
		CAM_DBG(CAM_ISP,
			"offline Session has PIX and RD resources, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_offline_state_machine_irq;
	} else {
		CAM_DBG(CAM_ISP,
			"Session has PIX or PIX and RDI resources, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_activated_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_activated_state_machine;
	}

	ctx_isp->hw_ctx = param.ctxt_to_hw_map;
	ctx_isp->hw_acquired = true;
	ctx_isp->split_acquire = false;
	ctx->ctxt_to_hw_map = param.ctxt_to_hw_map;
	atomic64_set(&ctx_isp->dbg_monitors.state_monitor_head, -1);
	atomic64_set(&ctx_isp->dbg_monitors.frame_monitor_head, -1);
	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->dbg_monitors.event_record_head[i], -1);

	CAM_INFO(CAM_ISP, "Ctx_type: %u, ctx_id: %u, hw_mgr_ctx: %u", isp_hw_cmd_args.u.ctx_type,
		ctx->ctx_id, param.hw_mgr_ctx_id);
	kfree(isp_res);
	isp_res = NULL;

get_dev_handle:

	req_hdl_param.session_hdl = cmd->session_handle;
	/* bridge is not ready for these flags. so false for now */
	req_hdl_param.v4l2_sub_dev_flag = 0;
	req_hdl_param.media_entity_flag = 0;
	req_hdl_param.ops = ctx->crm_ctx_intf;
	req_hdl_param.priv = ctx;
	req_hdl_param.dev_id = CAM_ISP;
	CAM_DBG(CAM_ISP, "get device handle form bridge, ctx_idx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);
	ctx->dev_hdl = cam_create_device_hdl(&req_hdl_param);
	if (ctx->dev_hdl <= 0) {
		rc = -EFAULT;
		CAM_ERR(CAM_ISP, "Can not create device handle, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_hw;
	}
	cmd->dev_handle = ctx->dev_hdl;

	/* store session information */
	ctx->session_hdl = cmd->session_handle;
	ctx->state = CAM_CTX_ACQUIRED;

	trace_cam_context_state("ISP", ctx);
	CAM_INFO(CAM_ISP,
		"Acquire success: session_hdl 0x%x num_rsrces %d ctx %u link: 0x%x",
		cmd->session_handle, cmd->num_resources, ctx->ctx_id, ctx->link_hdl);

	return rc;

free_hw:
	release.ctxt_to_hw_map = ctx_isp->hw_ctx;
	if (ctx_isp->hw_acquired)
		ctx->hw_mgr_intf->hw_release(ctx->hw_mgr_intf->hw_mgr_priv,
			&release);
	ctx_isp->hw_ctx = NULL;
	ctx_isp->hw_acquired = false;
free_res:
	kfree(isp_res);
end:
	return rc;
}

static int __cam_isp_ctx_acquire_hw_v1(struct cam_context *ctx,
	void *args)
{
	int rc = 0;
	int i;
	struct cam_acquire_hw_cmd_v1 *cmd =
		(struct cam_acquire_hw_cmd_v1 *)args;
	struct cam_hw_acquire_args        param;
	struct cam_hw_release_args        release;
	struct cam_isp_context           *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_hw_cmd_args            hw_cmd_args;
	struct cam_isp_hw_cmd_args        isp_hw_cmd_args;
	struct cam_isp_acquire_hw_info   *acquire_hw_info = NULL;
	struct cam_isp_comp_record_query  query_cmd;

	if (!ctx->hw_mgr_intf) {
		CAM_ERR(CAM_ISP, "HW interface is not ready, ctx %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	CAM_DBG(CAM_ISP,
		"session_hdl 0x%x, hdl type %d, res %lld ctx %u link: 0x%x",
		cmd->session_handle, cmd->handle_type, cmd->resource_hdl,
		ctx->ctx_id, ctx->link_hdl);

	/* for now we only support user pointer */
	if (cmd->handle_type != 1)  {
		CAM_ERR(CAM_ISP, "Only user pointer is supported, ctx %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
		goto end;
	}

	if (cmd->data_size < sizeof(*acquire_hw_info)) {
		CAM_ERR(CAM_ISP, "data_size is not a valid value, ctx %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	acquire_hw_info = kzalloc(cmd->data_size, GFP_KERNEL);
	if (!acquire_hw_info) {
		rc = -ENOMEM;
		goto end;
	}

	CAM_DBG(CAM_ISP, "start copy resources from user, ctx_idx: %u, link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);

	if (copy_from_user(acquire_hw_info, (void __user *)cmd->resource_hdl,
		cmd->data_size)) {
		rc = -EFAULT;
		goto free_res;
	}

	memset(&param, 0, sizeof(param));
	param.context_data = ctx;
	param.event_cb = ctx->irq_cb_intf;
	param.sec_pf_evt_cb = cam_context_dump_pf_info;
	param.num_acq = CAM_API_COMPAT_CONSTANT;
	param.acquire_info_size = cmd->data_size;
	param.acquire_info = (uint64_t) acquire_hw_info;
	param.mini_dump_cb = __cam_isp_ctx_minidump_cb;
	param.link_hdl = ctx->link_hdl;

	/* call HW manager to reserve the resource */
	rc = ctx->hw_mgr_intf->hw_acquire(ctx->hw_mgr_intf->hw_mgr_priv,
		&param);
	if (rc != 0) {
		CAM_ERR(CAM_ISP, "Acquire device failed, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_res;
	}

	rc = __cam_isp_ctx_allocate_mem_hw_entries(ctx,
		&param);
	if (rc) {
		CAM_ERR(CAM_ISP, "Ctx[%u] link: 0x%x allocate hw entry fail",
			ctx->ctx_id, ctx->link_hdl);
		goto free_res;
	}

	ctx_isp->last_num_exp = 0;
	ctx_isp->support_consumed_addr =
		(param.op_flags & CAM_IFE_CTX_CONSUME_ADDR_EN);
	ctx_isp->is_tfe_shdr = (param.op_flags & CAM_IFE_CTX_SHDR_EN);
	ctx_isp->is_shdr_master = (param.op_flags & CAM_IFE_CTX_SHDR_IS_MASTER);

	/* Query the context bus comp group information */
	ctx_isp->vfe_bus_comp_grp = kcalloc(CAM_IFE_BUS_COMP_NUM_MAX,
			sizeof(struct cam_isp_context_comp_record), GFP_KERNEL);
	if (!ctx_isp->vfe_bus_comp_grp) {
		CAM_ERR(CAM_CTXT, "%s[%d] no memory for vfe_bus_comp_grp",
			ctx->dev_name, ctx->ctx_id);
		rc = -ENOMEM;
		goto free_hw;
	}

	query_cmd.vfe_bus_comp_grp = ctx_isp->vfe_bus_comp_grp;
	hw_cmd_args.ctxt_to_hw_map = param.ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_GET_BUS_COMP_GROUP;
	isp_hw_cmd_args.cmd_data = &query_cmd;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
			&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Bus Comp HW command failed, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_hw;
	}

	/* Query the context has rdi only resource */
	hw_cmd_args.ctxt_to_hw_map = param.ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_CTX_TYPE;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "HW command failed, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_hw;
	}

	if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_RDI) {
		/*
		 * this context has rdi only resource assign rdi only
		 * state machine
		 */
		CAM_DBG(CAM_ISP, "RDI only session Context, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

		ctx_isp->substate_machine_irq =
			cam_isp_ctx_rdi_only_activated_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_rdi_only_activated_state_machine;
		ctx_isp->rdi_only_context = true;
	} else if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_FS2) {
		CAM_DBG(CAM_ISP, "FS2 Session has PIX, RD and RDI, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_fs2_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_fs2_state_machine;
	} else if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_OFFLINE) {
		CAM_DBG(CAM_ISP, "Offline session has PIX and RD resources, ctx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_offline_state_machine_irq;
		ctx_isp->substate_machine = NULL;
	} else {
		CAM_DBG(CAM_ISP, "Session has PIX or PIX and RDI resources, ctx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_activated_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_activated_state_machine;
	}

	ctx_isp->hw_ctx = param.ctxt_to_hw_map;
	ctx_isp->hw_acquired = true;
	ctx->ctxt_to_hw_map = param.ctxt_to_hw_map;

	atomic64_set(&ctx_isp->dbg_monitors.state_monitor_head, -1);
	atomic64_set(&ctx_isp->dbg_monitors.frame_monitor_head, -1);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->dbg_monitors.event_record_head[i], -1);

	trace_cam_context_state("ISP", ctx);
	CAM_INFO(CAM_ISP,
		"Acquire success:session_hdl 0x%xs ctx_type %d ctx %u link: 0x%x hw_mgr_ctx: %u is_shdr %d is_shdr_master %d",
		ctx->session_hdl, isp_hw_cmd_args.u.ctx_type, ctx->ctx_id, ctx->link_hdl,
		param.hw_mgr_ctx_id, ctx_isp->is_tfe_shdr, ctx_isp->is_shdr_master);
	kfree(acquire_hw_info);
	return rc;

free_hw:
	release.ctxt_to_hw_map = ctx_isp->hw_ctx;
	ctx->hw_mgr_intf->hw_release(ctx->hw_mgr_intf->hw_mgr_priv, &release);
	ctx_isp->hw_ctx = NULL;
	ctx_isp->hw_acquired = false;
free_res:
	kfree(acquire_hw_info);
end:
	return rc;
}

static void cam_req_mgr_process_workq_apply_req_worker(struct work_struct *w)
{
	cam_req_mgr_process_workq(w);
}

static int __cam_isp_ctx_acquire_hw_v2(struct cam_context *ctx,
	void *args)
{
	int rc = 0, i, j;
	struct cam_acquire_hw_cmd_v2 *cmd =
		(struct cam_acquire_hw_cmd_v2 *)args;
	struct cam_hw_acquire_args       param;
	struct cam_hw_release_args       release;
	struct cam_isp_context          *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_hw_cmd_args           hw_cmd_args;
	struct cam_isp_hw_cmd_args       isp_hw_cmd_args;
	struct cam_isp_acquire_hw_info  *acquire_hw_info = NULL;
	struct cam_isp_comp_record_query query_cmd;

	if (!ctx->hw_mgr_intf) {
		CAM_ERR(CAM_ISP, "HW interface is not ready, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	CAM_DBG(CAM_ISP,
		"session_hdl 0x%x, hdl type %d, res %lld, ctx_id %u link: 0x%x",
		cmd->session_handle, cmd->handle_type, cmd->resource_hdl,
		ctx->ctx_id, ctx->link_hdl);

	/* for now we only support user pointer */
	if (cmd->handle_type != 1)  {
		CAM_ERR(CAM_ISP, "Only user pointer is supported, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
		goto end;
	}

	if (cmd->data_size < sizeof(*acquire_hw_info)) {
		CAM_ERR(CAM_ISP, "data_size is not a valid value, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	acquire_hw_info = kzalloc(cmd->data_size, GFP_KERNEL);
	if (!acquire_hw_info) {
		rc = -ENOMEM;
		goto end;
	}

	CAM_DBG(CAM_ISP, "start copy resources from user, ctx_id %u link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);

	if (copy_from_user(acquire_hw_info, (void __user *)cmd->resource_hdl,
		cmd->data_size)) {
		rc = -EFAULT;
		goto free_res;
	}

	memset(&param, 0, sizeof(param));
	param.context_data = ctx;
	param.event_cb = ctx->irq_cb_intf;
	param.sec_pf_evt_cb = cam_context_dump_pf_info;
	param.num_acq = CAM_API_COMPAT_CONSTANT;
	param.acquire_info_size = cmd->data_size;
	param.acquire_info = (uint64_t) acquire_hw_info;
	param.mini_dump_cb = __cam_isp_ctx_minidump_cb;
	param.link_hdl = ctx->link_hdl;

	/* call HW manager to reserve the resource */
	rc = ctx->hw_mgr_intf->hw_acquire(ctx->hw_mgr_intf->hw_mgr_priv,
		&param);
	if (rc != 0) {
		CAM_ERR(CAM_ISP, "Acquire device failed, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_res;
	}

	rc = __cam_isp_ctx_allocate_mem_hw_entries(ctx, &param);
	if (rc) {
		CAM_ERR(CAM_ISP, "Ctx[%u] link: 0x%x allocate hw entry fail",
			ctx->ctx_id, ctx->link_hdl);
		goto free_hw;
	}

	/*
	 * Set feature flag if applicable
	 * custom hw is supported only on v2
	 */
	ctx_isp->last_num_exp = 0;
	ctx_isp->custom_enabled =
		(param.op_flags & CAM_IFE_CTX_CUSTOM_EN);
	ctx_isp->use_frame_header_ts =
		(param.op_flags & CAM_IFE_CTX_FRAME_HEADER_EN);
	ctx_isp->use_default_apply =
		(param.op_flags & CAM_IFE_CTX_APPLY_DEFAULT_CFG);
	ctx_isp->support_consumed_addr =
		(param.op_flags & CAM_IFE_CTX_CONSUME_ADDR_EN);
	ctx_isp->aeb_enabled =
		(param.op_flags & CAM_IFE_CTX_AEB_EN);
	ctx_isp->mode_switch_en =
		(param.op_flags & CAM_IFE_CTX_DYNAMIC_SWITCH_EN);
	ctx_isp->is_tfe_shdr = (param.op_flags & CAM_IFE_CTX_SHDR_EN);
	ctx_isp->is_shdr_master = (param.op_flags & CAM_IFE_CTX_SHDR_IS_MASTER);

	/* Query the context bus comp group information */
	ctx_isp->vfe_bus_comp_grp = kcalloc(CAM_IFE_BUS_COMP_NUM_MAX,
		sizeof(struct cam_isp_context_comp_record), GFP_KERNEL);
	if (!ctx_isp->vfe_bus_comp_grp) {
		CAM_ERR(CAM_CTXT, "%s[%d] no memory for vfe_bus_comp_grp",
			ctx->dev_name, ctx->ctx_id);
		rc = -ENOMEM;
		goto end;
	}

	if (param.op_flags & CAM_IFE_CTX_SFE_EN) {
		ctx_isp->sfe_bus_comp_grp = kcalloc(CAM_SFE_BUS_COMP_NUM_MAX,
			sizeof(struct cam_isp_context_comp_record), GFP_KERNEL);
		if (!ctx_isp->sfe_bus_comp_grp) {
			CAM_ERR(CAM_CTXT, "%s[%d] no memory for sfe_bus_comp_grp",
				ctx->dev_name, ctx->ctx_id);
			rc = -ENOMEM;
			goto end;
		}
	}

	query_cmd.vfe_bus_comp_grp = ctx_isp->vfe_bus_comp_grp;
	if (ctx_isp->sfe_bus_comp_grp)
		query_cmd.sfe_bus_comp_grp = ctx_isp->sfe_bus_comp_grp;
	hw_cmd_args.ctxt_to_hw_map = param.ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_GET_BUS_COMP_GROUP;
	isp_hw_cmd_args.cmd_data = &query_cmd;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "HW command failed");
		goto free_hw;
	}

	/* Query the context has rdi only resource */
	hw_cmd_args.ctxt_to_hw_map = param.ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_CTX_TYPE;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
				&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "HW command failed, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto free_hw;
	}

	if (param.valid_acquired_hw) {
		for (i = 0; i < CAM_MAX_ACQ_RES; i++)
			cmd->hw_info.acquired_hw_id[i] =
				param.acquired_hw_id[i];

		for (i = 0; i < CAM_MAX_ACQ_RES; i++)
			for (j = 0; j < CAM_MAX_HW_SPLIT; j++)
				cmd->hw_info.acquired_hw_path[i][j] =
					param.acquired_hw_path[i][j];

		ctx_isp->hw_idx = param.acquired_hw_id[0];
	}
	cmd->hw_info.valid_acquired_hw =
		param.valid_acquired_hw;

	cmd->hw_info.valid_acquired_hw = param.valid_acquired_hw;

	if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_RDI) {
		/*
		 * this context has rdi only resource assign rdi only
		 * state machine
		 */
		CAM_DBG(CAM_ISP, "RDI only session Context, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

		ctx_isp->substate_machine_irq =
			cam_isp_ctx_rdi_only_activated_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_rdi_only_activated_state_machine;
		ctx_isp->rdi_only_context = true;
	} else if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_FS2) {
		CAM_DBG(CAM_ISP, "FS2 Session has PIX, RD and RDI, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_fs2_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_fs2_state_machine;
	} else if (isp_hw_cmd_args.u.ctx_type == CAM_ISP_CTX_OFFLINE) {
		CAM_DBG(CAM_ISP, "Offline Session has PIX and RD resources, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_offline_state_machine_irq;
		ctx_isp->substate_machine = NULL;
		ctx_isp->offline_context = true;
	} else {
		CAM_DBG(CAM_ISP, "Session has PIX or PIX and RDI resources, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx_isp->substate_machine_irq =
			cam_isp_ctx_activated_state_machine_irq;
		ctx_isp->substate_machine =
			cam_isp_ctx_activated_state_machine;
	}

	if (ctx_isp->offline_context || ctx_isp->vfps_aux_context) {
		rc = cam_req_mgr_workq_create("ife_apply_req", 20,
			&ctx_isp->workq, CRM_WORKQ_USAGE_IRQ, 0,
			cam_req_mgr_process_workq_apply_req_worker);
		if (rc)
			CAM_ERR(CAM_ISP,
				"Failed to create workq for IFE rc:%d offline: %s vfps: %s ctx_id %u link: 0x%x",
				rc, CAM_BOOL_TO_YESNO(ctx_isp->offline_context),
				CAM_BOOL_TO_YESNO(ctx_isp->vfps_aux_context),
				ctx->ctx_id, ctx->link_hdl);
	}

	ctx_isp->hw_ctx = param.ctxt_to_hw_map;
	ctx_isp->hw_acquired = true;
	ctx->ctxt_to_hw_map = param.ctxt_to_hw_map;
	ctx->hw_mgr_ctx_id = param.hw_mgr_ctx_id;

	snprintf(ctx->ctx_id_string, sizeof(ctx->ctx_id_string),
		"%s_ctx[%d]_hwmgrctx[%d]_hwidx[0x%x]",
		ctx->dev_name,
		ctx->ctx_id,
		ctx->hw_mgr_ctx_id,
		ctx_isp->hw_idx);

	trace_cam_context_state("ISP", ctx);
	CAM_INFO(CAM_ISP,
		"Acquire success: session_hdl 0x%xs ctx_type %d ctx %u link 0x%x hw_mgr_ctx %u is_shdr %d is_shdr_master %d",
		ctx->session_hdl, isp_hw_cmd_args.u.ctx_type, ctx->ctx_id, ctx->link_hdl,
		param.hw_mgr_ctx_id, ctx_isp->is_tfe_shdr, ctx_isp->is_shdr_master);
	kfree(acquire_hw_info);
	return rc;

free_hw:
	release.ctxt_to_hw_map = ctx_isp->hw_ctx;
	ctx->hw_mgr_intf->hw_release(ctx->hw_mgr_intf->hw_mgr_priv, &release);
	ctx_isp->hw_ctx = NULL;
	ctx_isp->hw_acquired = false;
free_res:
	kfree(acquire_hw_info);
end:
	return rc;
}

static int __cam_isp_ctx_acquire_hw_in_acquired(struct cam_context *ctx,
	void *args)
{
	int rc = -EINVAL;
	uint32_t api_version;

	if (!ctx || !args) {
		CAM_ERR(CAM_ISP, "Invalid input pointer");
		return rc;
	}

	api_version = *((uint32_t *)args);
	if (api_version == 1)
		rc = __cam_isp_ctx_acquire_hw_v1(ctx, args);
	else if (api_version == 2)
		rc = __cam_isp_ctx_acquire_hw_v2(ctx, args);
	else
		CAM_ERR(CAM_ISP, "Unsupported api version %d, ctx_id %u link: 0x%x",
			api_version, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_config_dev_in_acquired(struct cam_context *ctx,
	struct cam_config_dev_cmd *cmd)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	if (!ctx_isp->hw_acquired) {
		CAM_ERR(CAM_ISP, "HW is not acquired, reject packet, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	rc = __cam_isp_ctx_config_dev_in_top_state(ctx, cmd);

	if (!rc && ((ctx->link_hdl >= 0) || ctx_isp->offline_context)) {
		ctx->state = CAM_CTX_READY;
		trace_cam_context_state("ISP", ctx);
	}

	CAM_DBG(CAM_ISP, "next state %d, ctx %u link: 0x%x",
		ctx->state, ctx->ctx_id, ctx->link_hdl);
	return rc;
}

static int __cam_isp_ctx_config_dev_in_flushed(struct cam_context *ctx,
	struct cam_config_dev_cmd *cmd)
{
	int rc = 0;
	struct cam_start_stop_dev_cmd start_cmd;
	struct cam_hw_cmd_args hw_cmd_args;
	struct cam_isp_hw_cmd_args isp_hw_cmd_args;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	if (!ctx_isp->hw_acquired) {
		CAM_ERR(CAM_ISP, "HW is not acquired, reject packet, ctx_id %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
		goto end;
	}

	rc = __cam_isp_ctx_config_dev_in_top_state(ctx, cmd);
	if (rc)
		goto end;

	if (!ctx_isp->init_received) {
		CAM_WARN(CAM_ISP,
			"Received update pckt in flushed state, skip start, ctx %u link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	CAM_DBG(CAM_ISP, "vfps_ctx:%s resume_hw_in_flushed:%d ctx:%u link: 0x%x",
		CAM_BOOL_TO_YESNO(ctx_isp->vfps_aux_context),
		ctx_isp->resume_hw_in_flushed,
		ctx->ctx_id, ctx->link_hdl);

	if (ctx_isp->vfps_aux_context) {
		/* Resume the HW only when we get first valid req */
		if (!ctx_isp->resume_hw_in_flushed)
			goto end;
		else
			ctx_isp->resume_hw_in_flushed = false;
	}

	hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_RESUME_HW;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Failed to resume HW rc: %d, ctx_id %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}

	start_cmd.dev_handle = cmd->dev_handle;
	start_cmd.session_handle = cmd->session_handle;
	rc = __cam_isp_ctx_start_dev_in_ready(ctx, &start_cmd);
	if (rc)
		CAM_ERR(CAM_ISP,
			"Failed to re-start HW after flush rc: %d, ctx_id %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
	else
		CAM_INFO(CAM_ISP,
			"Received init after flush. Re-start HW complete in ctx:%d, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

end:
	CAM_DBG(CAM_ISP, "next state %d sub_state:%d ctx_id %u link: 0x%x", ctx->state,
		ctx_isp->substate_activated, ctx->ctx_id, ctx->link_hdl);
	return rc;
}

static int __cam_isp_ctx_link_in_acquired(struct cam_context *ctx,
	struct cam_req_mgr_core_dev_link_setup *link)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	if (!link) {
		CAM_ERR(CAM_ISP, "setup link info is null: %pK ctx: %u link: 0x%x",
			link, ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	if (!link->crm_cb) {
		CAM_ERR(CAM_ISP, "crm cb is null: %pK ctx: %u, link: 0x%x",
			link->crm_cb, ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	CAM_DBG(CAM_ISP, "Enter.........ctx: %u, link: 0x%x", ctx->ctx_id, ctx->link_hdl);

	ctx->link_hdl = link->link_hdl;
	ctx->ctx_crm_intf = link->crm_cb;
	ctx_isp->subscribe_event =
		CAM_TRIGGER_POINT_SOF | CAM_TRIGGER_POINT_EOF;
	ctx_isp->trigger_id = link->trigger_id;
	ctx_isp->mswitch_default_apply_delay_max_cnt = 0;
	atomic_set(&ctx_isp->mswitch_default_apply_delay_ref_cnt, 0);

	if ((link->mode_switch_max_delay - CAM_MODESWITCH_DELAY_1) > 0) {
		ctx_isp->handle_mswitch = true;
		ctx_isp->mswitch_default_apply_delay_max_cnt =
			link->mode_switch_max_delay - CAM_MODESWITCH_DELAY_1;
		CAM_DBG(CAM_ISP,
			"Enabled mode switch handling on ctx: %u max delay cnt: %u",
			ctx->ctx_id, ctx_isp->mswitch_default_apply_delay_max_cnt);
		atomic_set(&ctx_isp->mswitch_default_apply_delay_ref_cnt,
			ctx_isp->mswitch_default_apply_delay_max_cnt);
	}

	/* change state only if we had the init config */
	if (ctx_isp->init_received) {
		ctx->state = CAM_CTX_READY;
		trace_cam_context_state("ISP", ctx);
	}

	CAM_DBG(CAM_ISP, "next state %d, ctx: %u, link: 0x%x",
		ctx->state, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_unlink_in_acquired(struct cam_context *ctx,
	struct cam_req_mgr_core_dev_link_setup *unlink)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	ctx->link_hdl = -1;
	ctx->ctx_crm_intf = NULL;
	ctx_isp->trigger_id = -1;
	ctx_isp->mswitch_default_apply_delay_max_cnt = 0;
	atomic_set(&ctx_isp->mswitch_default_apply_delay_ref_cnt, 0);

	return rc;
}

static int __cam_isp_ctx_get_dev_info(struct cam_context *ctx,
	struct cam_req_mgr_device_info *dev_info)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	dev_info->dev_hdl = ctx->dev_hdl;
	strlcpy(dev_info->name, CAM_ISP_DEV_NAME, sizeof(dev_info->name));
	dev_info->dev_id = CAM_REQ_MGR_DEVICE_IFE;
	dev_info->p_delay = CAM_PIPELINE_DELAY_1;
	dev_info->m_delay = CAM_MODESWITCH_DELAY_1;
	dev_info->trigger = CAM_TRIGGER_POINT_SOF;
	dev_info->trigger_on = true;
	dev_info->is_shdr = ctx_isp->is_tfe_shdr;
	dev_info->is_shdr_master = ctx_isp->is_shdr_master;

	return rc;
}

static inline void __cam_isp_context_reset_ctx_params(
	struct cam_isp_context    *ctx_isp)
{
	atomic_set(&ctx_isp->process_bubble, 0);
	atomic_set(&ctx_isp->rxd_epoch, 0);
	atomic_set(&ctx_isp->internal_recovery_set, 0);
	ctx_isp->frame_id = 0;
	ctx_isp->sof_timestamp_val = 0;
	ctx_isp->boot_timestamp = 0;
	ctx_isp->active_req_cnt = 0;
	ctx_isp->reported_req_id = 0;
	ctx_isp->reported_frame_id = 0;
	ctx_isp->bubble_frame_cnt = 0;
	ctx_isp->congestion_cnt = 0;
	ctx_isp->recovery_req_id = 0;
	ctx_isp->aeb_error_cnt = 0;
	ctx_isp->out_of_sync_cnt = 0;
	ctx_isp->sof_dbg_irq_en = false;
	ctx_isp->last_sof_jiffies = 0;
	ctx_isp->last_applied_jiffies = 0;
}

static int __cam_isp_ctx_start_dev_in_ready(struct cam_context *ctx,
	struct cam_start_stop_dev_cmd *cmd)
{
	int rc = 0;
	int i;
	struct cam_isp_start_args        start_isp;
	struct cam_ctx_request          *req;
	struct cam_isp_ctx_req          *req_isp;
	struct cam_isp_context          *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	if (cmd->session_handle != ctx->session_hdl ||
		cmd->dev_handle != ctx->dev_hdl) {
		rc = -EPERM;
		goto end;
	}

	if (list_empty(&ctx->pending_req_list)) {
		/* should never happen */
		CAM_ERR(CAM_ISP, "Start device with empty configuration, ctx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	} else {
		req = list_first_entry(&ctx->pending_req_list,
			struct cam_ctx_request, list);
	}
	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	if (!ctx_isp->hw_ctx) {
		CAM_ERR(CAM_ISP, "Wrong hw context pointer.ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	start_isp.hw_config.ctxt_to_hw_map = ctx_isp->hw_ctx;
	start_isp.hw_config.request_id = req->request_id;
	start_isp.hw_config.hw_update_entries = req_isp->cfg;
	start_isp.hw_config.num_hw_update_entries = req_isp->num_cfg;
	start_isp.hw_config.priv  = &req_isp->hw_update_data;
	start_isp.hw_config.init_packet = 1;
	start_isp.hw_config.reapply_type = CAM_CONFIG_REAPPLY_NONE;
	start_isp.hw_config.cdm_reset_before_apply = false;
	start_isp.is_internal_start = false;

	ctx_isp->last_applied_req_id = req->request_id;

	if (ctx->state == CAM_CTX_FLUSHED)
		start_isp.start_only = true;
	else
		start_isp.start_only = false;

	__cam_isp_context_reset_ctx_params(ctx_isp);

	ctx_isp->substate_activated = ctx_isp->rdi_only_context ?
		CAM_ISP_CTX_ACTIVATED_APPLIED :
		(req_isp->num_fence_map_out) ? CAM_ISP_CTX_ACTIVATED_EPOCH :
		CAM_ISP_CTX_ACTIVATED_SOF;

	atomic64_set(&ctx_isp->dbg_monitors.state_monitor_head, -1);
	atomic64_set(&ctx_isp->dbg_monitors.frame_monitor_head, -1);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->dbg_monitors.event_record_head[i], -1);

	/*
	 * In case of CSID TPG we might receive SOF and RUP IRQs
	 * before hw_mgr_intf->hw_start has returned. So move
	 * req out of pending list before hw_start and add it
	 * back to pending list if hw_start fails.
	 */
	list_del_init(&req->list);

	if (ctx_isp->offline_context && !req_isp->num_fence_map_out) {
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
		atomic_set(&ctx_isp->rxd_epoch, 1);
		CAM_DBG(CAM_REQ,
			"Move pending req: %lld to free list(cnt: %d) offline ctx %u link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
	} else if (ctx_isp->rdi_only_context || !req_isp->num_fence_map_out) {
		list_add_tail(&req->list, &ctx->wait_req_list);
		CAM_DBG(CAM_REQ,
			"Move pending req: %lld to wait list(cnt: %d) ctx %u link: 0x%x",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);
	} else {
		list_add_tail(&req->list, &ctx->active_req_list);
		ctx_isp->active_req_cnt++;
		CAM_DBG(CAM_REQ,
			"Move pending req: %lld to active list(cnt: %d) ctx %u link: 0x%x offline %d",
			req->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl,
			ctx_isp->offline_context);
	}

	/*
	 * Only place to change state before calling the hw due to
	 * hardware tasklet has higher priority that can cause the
	 * irq handling comes early
	 */
	ctx->state = CAM_CTX_ACTIVATED;
	trace_cam_context_state("ISP", ctx);
	rc = ctx->hw_mgr_intf->hw_start(ctx->hw_mgr_intf->hw_mgr_priv,
		&start_isp);
	if (rc) {
		/* HW failure. user need to clean up the resource */
		CAM_ERR(CAM_ISP, "Start HW failed, ctx %u link: 0x%x", ctx->ctx_id, ctx->link_hdl);
		ctx->state = CAM_CTX_READY;
		if ((rc == -ETIMEDOUT) &&
			(isp_ctx_debug.enable_cdm_cmd_buff_dump))
			rc = cam_isp_ctx_dump_req(req_isp, 0, 0, NULL, false);

		trace_cam_context_state("ISP", ctx);
		if (req->packet) {
			cam_common_mem_free(req->packet);
			req->packet = NULL;
		}
		list_del_init(&req->list);
		list_add(&req->list, &ctx->pending_req_list);
		goto end;
	}
	CAM_DBG(CAM_ISP, "start device success ctx %u link: 0x%x", ctx->ctx_id, ctx->link_hdl);

end:
	return rc;
}

static int __cam_isp_ctx_unlink_in_ready(struct cam_context *ctx,
	struct cam_req_mgr_core_dev_link_setup *unlink)
{
	int rc = 0;

	ctx->link_hdl = -1;
	ctx->ctx_crm_intf = NULL;
	ctx->state = CAM_CTX_ACQUIRED;
	trace_cam_context_state("ISP", ctx);

	return rc;
}

static int __cam_isp_ctx_stop_dev_in_activated_unlock(
	struct cam_context *ctx, struct cam_start_stop_dev_cmd *stop_cmd)
{
	int rc = 0;
	uint32_t i;
	struct cam_hw_stop_args          stop;
	struct cam_ctx_request          *req;
	struct cam_isp_ctx_req          *req_isp;
	struct cam_isp_context          *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_isp_stop_args         stop_isp;

	/* Mask off all the incoming hardware events */
	spin_lock_bh(&ctx->lock);
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_HALT;
	spin_unlock_bh(&ctx->lock);

	/* stop hw first */
	if (ctx_isp->hw_ctx) {
		stop.ctxt_to_hw_map = ctx_isp->hw_ctx;

		stop_isp.hw_stop_cmd = CAM_ISP_HW_STOP_IMMEDIATELY;
		stop_isp.stop_only = false;
		stop_isp.is_internal_stop = false;

		stop.args = (void *) &stop_isp;
		ctx->hw_mgr_intf->hw_stop(ctx->hw_mgr_intf->hw_mgr_priv,
			&stop);
	}

	CAM_DBG(CAM_ISP, "next Substate[%s], ctx_idx: %u, link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	if (ctx->ctx_crm_intf &&
		ctx->ctx_crm_intf->notify_stop) {
		struct cam_req_mgr_notify_stop notify;

		notify.link_hdl = ctx->link_hdl;
		CAM_DBG(CAM_ISP,
			"Notify CRM about device stop ctx %u link 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx->ctx_crm_intf->notify_stop(&notify);
	} else if (!ctx_isp->offline_context)
		CAM_ERR(CAM_ISP, "cb not present, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	while (!list_empty(&ctx->pending_req_list)) {
		req = list_first_entry(&ctx->pending_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in pending list. fence num %d ctx:%u, link: 0x%x",
			 req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_CANCEL,
					CAM_SYNC_ISP_EVENT_HW_STOP);
			}
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
	}

	while (!list_empty(&ctx->wait_req_list)) {
		req = list_first_entry(&ctx->wait_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in wait list. fence num %d ctx: %u, link: 0x%x",
			 req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_CANCEL,
					CAM_SYNC_ISP_EVENT_HW_STOP);
			}
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
	}

	while (!list_empty(&ctx->active_req_list)) {
		req = list_first_entry(&ctx->active_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		cam_smmu_buffer_tracker_putref(&req->buf_tracker);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in active list. fence num %d ctx: %u, link: 0x%x",
			 req_isp->num_fence_map_out, ctx->ctx_id, ctx->link_hdl);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_CANCEL,
					CAM_SYNC_ISP_EVENT_HW_STOP);
			}
		__cam_isp_ctx_move_req_to_free_list(ctx, req);
	}

	ctx_isp->frame_id = 0;
	ctx_isp->active_req_cnt = 0;
	ctx_isp->reported_req_id = 0;
	ctx_isp->reported_frame_id = 0;
	ctx_isp->last_applied_req_id = 0;
	ctx_isp->req_info.last_bufdone_req_id = 0;
	ctx_isp->bubble_frame_cnt = 0;
	ctx_isp->congestion_cnt = 0;
	ctx_isp->sof_dbg_irq_en = false;
	atomic_set(&ctx_isp->process_bubble, 0);
	atomic_set(&ctx_isp->internal_recovery_set, 0);
	atomic_set(&ctx_isp->rxd_epoch, 0);
	atomic64_set(&ctx_isp->dbg_monitors.state_monitor_head, -1);
	atomic64_set(&ctx_isp->dbg_monitors.frame_monitor_head, -1);

	/* Reset skipped_list for FCG config */
	__cam_isp_ctx_reset_fcg_tracker(ctx);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->dbg_monitors.event_record_head[i], -1);

	CAM_DBG(CAM_ISP, "Stop device success next state %d on ctx %u link: 0x%x",
		ctx->state, ctx->ctx_id, ctx->link_hdl);

	if (!stop_cmd) {
		rc = __cam_isp_ctx_unlink_in_ready(ctx, NULL);
		if (rc)
			CAM_ERR(CAM_ISP, "Unlink failed rc=%d, ctx %u link: 0x%x",
				rc, ctx->ctx_id, ctx->link_hdl);
	}
	return rc;
}

static int __cam_isp_ctx_stop_dev_in_activated(struct cam_context *ctx,
	struct cam_start_stop_dev_cmd *cmd)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *)ctx->ctx_priv;

	__cam_isp_ctx_stop_dev_in_activated_unlock(ctx, cmd);
	ctx_isp->init_received = false;
	ctx->state = CAM_CTX_ACQUIRED;
	trace_cam_context_state("ISP", ctx);
	return rc;
}

static int __cam_isp_ctx_release_dev_in_activated(struct cam_context *ctx,
	struct cam_release_dev_cmd *cmd)
{
	int rc = 0;

	rc = __cam_isp_ctx_stop_dev_in_activated_unlock(ctx, NULL);
	if (rc)
		CAM_ERR(CAM_ISP, "Stop device failed rc=%d, ctx %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);

	rc = __cam_isp_ctx_release_dev_in_top_state(ctx, cmd);
	if (rc)
		CAM_ERR(CAM_ISP, "Release device failed rc=%d ctx %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_release_hw_in_activated(struct cam_context *ctx,
	void *cmd)
{
	int rc = 0;

	rc = __cam_isp_ctx_stop_dev_in_activated_unlock(ctx, NULL);
	if (rc)
		CAM_ERR(CAM_ISP, "Stop device failed rc=%d, ctx %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);

	rc = __cam_isp_ctx_release_hw_in_top_state(ctx, cmd);
	if (rc)
		CAM_ERR(CAM_ISP, "Release hw failed rc=%d, ctx %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_link_pause(struct cam_context *ctx)
{
	int rc = 0;
	struct cam_hw_cmd_args       hw_cmd_args;
	struct cam_isp_hw_cmd_args   isp_hw_cmd_args;

	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_PAUSE_HW;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);

	return rc;
}

static int __cam_isp_ctx_link_resume(struct cam_context *ctx)
{
	int rc = 0;
	struct cam_hw_cmd_args       hw_cmd_args;
	struct cam_isp_hw_cmd_args   isp_hw_cmd_args;

	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_RESUME_HW;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);

	return rc;
}

static int __cam_isp_ctx_reset_and_recover(
	bool skip_resume, struct cam_context *ctx)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *)ctx->ctx_priv;
	struct cam_isp_stop_args              stop_isp;
	struct cam_hw_stop_args               stop_args;
	struct cam_isp_start_args             start_isp;
	struct cam_hw_cmd_args                hw_cmd_args;
	struct cam_isp_hw_cmd_args            isp_hw_cmd_args;
	struct cam_ctx_request               *req;
	struct cam_isp_ctx_req               *req_isp;

	spin_lock_bh(&ctx->lock);
	if (ctx_isp->active_req_cnt) {
		spin_unlock_bh(&ctx->lock);
		CAM_WARN(CAM_ISP,
			"Active list not empty: %u in ctx: %u on link: 0x%x, retry recovery for req: %lld after buf_done",
			ctx_isp->active_req_cnt, ctx->ctx_id,
			ctx->link_hdl, ctx_isp->recovery_req_id);
		goto end;
	}

	if (ctx->state != CAM_CTX_ACTIVATED) {
		spin_unlock_bh(&ctx->lock);
		CAM_ERR(CAM_ISP,
			"In wrong state %d, for recovery ctx: %u in link: 0x%x recovery req: %lld",
			ctx->state, ctx->ctx_id,
			ctx->link_hdl, ctx_isp->recovery_req_id);
		rc = -EINVAL;
		goto end;
	}

	if (list_empty(&ctx->pending_req_list)) {
		/* Cannot start with no request */
		spin_unlock_bh(&ctx->lock);
		CAM_ERR(CAM_ISP,
			"Failed to reset and recover last_applied_req: %llu in ctx: %u on link: 0x%x",
			ctx_isp->last_applied_req_id, ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	if (!ctx_isp->hw_ctx) {
		spin_unlock_bh(&ctx->lock);
		CAM_ERR(CAM_ISP,
			"Invalid hw context pointer ctx: %u on link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
		goto end;
	}

	/* Block all events till HW is resumed */
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_HALT;

	req = list_first_entry(&ctx->pending_req_list,
		struct cam_ctx_request, list);
	spin_unlock_bh(&ctx->lock);

	req_isp = (struct cam_isp_ctx_req *) req->req_priv;

	CAM_INFO(CAM_ISP,
		"Trigger Halt, Reset & Resume for req: %llu ctx: %u in state: %d link: 0x%x",
		req->request_id, ctx->ctx_id, ctx->state, ctx->link_hdl);

	stop_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
	stop_isp.hw_stop_cmd = CAM_ISP_HW_STOP_IMMEDIATELY;
	stop_isp.stop_only = true;
	stop_isp.is_internal_stop = true;
	stop_args.args = (void *)&stop_isp;
	rc = ctx->hw_mgr_intf->hw_stop(ctx->hw_mgr_intf->hw_mgr_priv,
		&stop_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Failed to stop HW rc: %d ctx: %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	CAM_DBG(CAM_ISP, "Stop HW success ctx: %u link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);

	/* API provides provision to stream off and not resume as well in case of fatal errors */
	if (skip_resume) {
		atomic_set(&ctx_isp->internal_recovery_set, 0);
		CAM_INFO(CAM_ISP,
			"Halting streaming off IFE/SFE ctx: %u last_applied_req: %lld [recovery_req: %lld] on link: 0x%x",
			ctx->ctx_id, ctx_isp->last_applied_req_id,
			ctx_isp->recovery_req_id, ctx->link_hdl);
		goto end;
	}

	hw_cmd_args.ctxt_to_hw_map = ctx_isp->hw_ctx;
	hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
	isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_RESUME_HW;
	hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
	rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
		&hw_cmd_args);
	if (rc) {
		CAM_ERR(CAM_ISP, "Failed to resume HW rc: %d ctx: %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);
		goto end;
	}
	CAM_DBG(CAM_ISP, "Resume call success ctx: %u on link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);

	start_isp.hw_config.ctxt_to_hw_map = ctx_isp->hw_ctx;
	start_isp.hw_config.request_id = req->request_id;
	start_isp.hw_config.hw_update_entries = req_isp->cfg;
	start_isp.hw_config.num_hw_update_entries = req_isp->num_cfg;
	start_isp.hw_config.priv  = &req_isp->hw_update_data;
	start_isp.hw_config.init_packet = 1;
	start_isp.hw_config.reapply_type = CAM_CONFIG_REAPPLY_IQ;
	start_isp.hw_config.cdm_reset_before_apply = false;
	start_isp.start_only = true;
	start_isp.is_internal_start = true;

	__cam_isp_context_reset_internal_recovery_params(ctx_isp);

	ctx_isp->substate_activated = ctx_isp->rdi_only_context ?
		CAM_ISP_CTX_ACTIVATED_APPLIED : CAM_ISP_CTX_ACTIVATED_SOF;

	rc = ctx->hw_mgr_intf->hw_start(ctx->hw_mgr_intf->hw_mgr_priv,
		&start_isp);
	if (rc) {
		CAM_ERR(CAM_ISP, "Start HW failed, ctx: %u link: 0x%x", ctx->ctx_id, ctx->link_hdl);
		ctx->state = CAM_CTX_READY;
		goto end;
	}

	/* IQ applied for this request, on next trigger skip IQ cfg */
	req_isp->reapply_type = CAM_CONFIG_REAPPLY_IO;

	/* Notify userland that KMD has done internal recovery */
	__cam_isp_ctx_notify_v4l2_error_event(CAM_REQ_MGR_WARN_TYPE_KMD_RECOVERY,
		0, req->request_id, ctx);

	CAM_INFO(CAM_ISP, "Internal Start HW success ctx %u on link: 0x%x for req: %llu",
		ctx->ctx_id, ctx->link_hdl, req->request_id);

end:
	return rc;
}

static bool __cam_isp_ctx_try_internal_recovery_for_bubble(
	int64_t error_req_id, struct cam_context *ctx)
{
	int rc;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *)ctx->ctx_priv;

	if (isp_ctx_debug.disable_internal_recovery_mask &
		CAM_ISP_CTX_DISABLE_RECOVERY_BUBBLE)
		return false;

	/* Perform recovery if bubble recovery is stalled */
	if (!atomic_read(&ctx_isp->process_bubble))
		return false;

	/* Validate if errored request has been applied */
	if (ctx_isp->last_applied_req_id < error_req_id) {
		CAM_WARN(CAM_ISP,
			"Skip trying for internal recovery last applied: %lld error_req: %lld for ctx: %u on link: 0x%x",
			ctx_isp->last_applied_req_id, error_req_id,
			ctx->ctx_id, ctx->link_hdl);
		return false;
	}

	if (__cam_isp_ctx_validate_for_req_reapply_util(ctx_isp)) {
		CAM_WARN(CAM_ISP,
			"Internal recovery not possible for ctx: %u on link: 0x%x req: %lld [last_applied: %lld]",
			ctx->ctx_id, ctx->link_hdl, error_req_id, ctx_isp->last_applied_req_id);
		return false;
	}

	/* Trigger reset and recover */
	atomic_set(&ctx_isp->internal_recovery_set, 1);
	rc = __cam_isp_ctx_reset_and_recover(false, ctx);
	if (rc) {
		CAM_WARN(CAM_ISP,
			"Internal recovery failed in ctx: %u on link: 0x%x req: %lld [last_applied: %lld]",
			ctx->ctx_id, ctx->link_hdl, error_req_id, ctx_isp->last_applied_req_id);
		atomic_set(&ctx_isp->internal_recovery_set, 0);
		goto error;
	}

	CAM_DBG(CAM_ISP,
		"Internal recovery done in ctx: %u on link: 0x%x req: %lld [last_applied: %lld]",
		ctx->ctx_id, ctx->link_hdl, error_req_id, ctx_isp->last_applied_req_id);

	return true;

error:
	return false;
}

static int __cam_isp_ctx_process_evt(struct cam_context *ctx,
	struct cam_req_mgr_link_evt_data *link_evt_data)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	if ((ctx->state == CAM_CTX_ACQUIRED) &&
		(link_evt_data->evt_type != CAM_REQ_MGR_LINK_EVT_UPDATE_PROPERTIES)) {
		CAM_WARN(CAM_ISP,
			"Get unexpect evt:%d in acquired state, ctx: %u on link: 0x%x",
			link_evt_data->evt_type, ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}

	switch (link_evt_data->evt_type) {
	case CAM_REQ_MGR_LINK_EVT_ERR:
	case CAM_REQ_MGR_LINK_EVT_EOF:
		/* No handling */
		break;
	case CAM_REQ_MGR_LINK_EVT_PAUSE:
		rc = __cam_isp_ctx_link_pause(ctx);
		break;
	case CAM_REQ_MGR_LINK_EVT_RESUME:
		rc =  __cam_isp_ctx_link_resume(ctx);
		break;
	case CAM_REQ_MGR_LINK_EVT_SOF_FREEZE:
		rc = __cam_isp_ctx_handle_sof_freeze_evt(ctx);
		break;
	case CAM_REQ_MGR_LINK_EVT_STALLED: {
		bool internal_recovery_skipped = false;

		if (ctx->state == CAM_CTX_ACTIVATED) {
			if (link_evt_data->try_for_recovery)
				internal_recovery_skipped =
					__cam_isp_ctx_try_internal_recovery_for_bubble(
						link_evt_data->req_id, ctx);

			if (!internal_recovery_skipped)
				rc = __cam_isp_ctx_trigger_reg_dump(
					CAM_HW_MGR_CMD_REG_DUMP_ON_ERROR, ctx);
		}
		link_evt_data->try_for_recovery = internal_recovery_skipped;
	}
		break;
	case CAM_REQ_MGR_LINK_EVT_UPDATE_PROPERTIES:
		if (link_evt_data->u.properties_mask &
			CAM_LINK_PROPERTY_SENSOR_STANDBY_AFTER_EOF)
			ctx_isp->vfps_aux_context = true;
		else
			ctx_isp->vfps_aux_context = false;
		CAM_DBG(CAM_ISP, "vfps_aux_context:%s on ctx: %u link: 0x%x",
			CAM_BOOL_TO_YESNO(ctx_isp->vfps_aux_context), ctx->ctx_id, ctx->link_hdl);
		break;
	default:
		CAM_WARN(CAM_ISP,
			"Unsupported event type: 0x%x on ctx: %u link: 0x%x",
			link_evt_data->evt_type, ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int __cam_isp_ctx_unlink_in_activated(struct cam_context *ctx,
	struct cam_req_mgr_core_dev_link_setup *unlink)
{
	int rc = 0;

	CAM_WARN(CAM_ISP,
		"Received unlink in activated state. It's unexpected, ctx: %u link: 0x%x",
		ctx->ctx_id, ctx->link_hdl);

	rc = __cam_isp_ctx_stop_dev_in_activated_unlock(ctx, NULL);
	if (rc)
		CAM_WARN(CAM_ISP, "Stop device failed rc=%d, ctx: %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);

	rc = __cam_isp_ctx_unlink_in_ready(ctx, unlink);
	if (rc)
		CAM_ERR(CAM_ISP, "Unlink failed rc=%d, ctx: %u link: 0x%x",
			rc, ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int __cam_isp_ctx_apply_req(struct cam_context *ctx,
	struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	struct cam_ctx_ops *ctx_ops = NULL;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;

	trace_cam_apply_req("ISP", ctx->ctx_id, apply->request_id, apply->link_hdl);
	CAM_DBG(CAM_ISP, "Enter: apply req in Substate[%s] request_id:%lld, ctx: %u link: 0x%x",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), apply->request_id, ctx->ctx_id, ctx->link_hdl);
	ctx_ops = &ctx_isp->substate_machine[ctx_isp->substate_activated];
	if (ctx_ops->crm_ops.apply_req) {
		rc = ctx_ops->crm_ops.apply_req(ctx, apply);
	} else {
		CAM_WARN_RATE_LIMIT(CAM_ISP,
			"No handle function in activated Substate[%s], ctx: %u link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);
		rc = -EFAULT;
	}

	if (rc)
		CAM_WARN_RATE_LIMIT(CAM_ISP,
			"Apply failed in active Substate[%s] rc %d, ctx: %u link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), rc, ctx->ctx_id, ctx->link_hdl);
	return rc;
}

static int __cam_isp_ctx_apply_default_settings(
	struct cam_context *ctx,
	struct cam_req_mgr_apply_request *apply)
{
	int rc = 0;
	struct cam_ctx_ops *ctx_ops = NULL;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_isp_fcg_prediction_tracker *fcg_tracker;

	if (!(apply->trigger_point & ctx_isp->subscribe_event)) {
		CAM_WARN(CAM_ISP,
			"Trigger: %u not subscribed for: %u, ctx: %u link: 0x%x",
			apply->trigger_point, ctx_isp->subscribe_event, ctx->ctx_id,
			ctx->link_hdl);
		return 0;
	}

	/* Allow apply default settings for IFE only at SOF */
	if (apply->trigger_point != CAM_TRIGGER_POINT_SOF)
		return 0;

	if (atomic_read(&ctx_isp->internal_recovery_set))
		return __cam_isp_ctx_reset_and_recover(false, ctx);

	/* FCG handling */
	fcg_tracker = &ctx_isp->fcg_tracker;
	if (ctx_isp->frame_id != 1)
		fcg_tracker->num_skipped += 1;
	CAM_DBG(CAM_ISP,
		"Apply default settings, number of previous continuous skipped frames: %d, ctx_id: %d",
		fcg_tracker->num_skipped, ctx->ctx_id);

	/*
	 * Call notify frame skip for static offline cases or
	 * mode switch cases where IFE mode switch delay differs
	 * from other devices on the link
	 */
	if ((ctx_isp->use_default_apply) ||
		(ctx_isp->mode_switch_en && ctx_isp->handle_mswitch)) {
		CAM_DBG(CAM_ISP,
			"Enter: apply req in Substate:%d request _id:%lld ctx:%u on link:0x%x",
			ctx_isp->substate_activated, apply->request_id,
			ctx->ctx_id, ctx->link_hdl);

		ctx_ops = &ctx_isp->substate_machine[
			ctx_isp->substate_activated];
		if (ctx_ops->crm_ops.notify_frame_skip) {
			rc = ctx_ops->crm_ops.notify_frame_skip(ctx, apply);
		} else {
			CAM_WARN_RATE_LIMIT(CAM_ISP,
				"No handle function in activated substate %d, ctx:%u on link:0x%x",
				ctx_isp->substate_activated, ctx->ctx_id, ctx->link_hdl);
			rc = -EFAULT;
		}

		if (rc)
			CAM_WARN_RATE_LIMIT(CAM_ISP,
				"Apply default failed in active substate %d rc %d ctx: %u link: 0x%x",
				ctx_isp->substate_activated, rc, ctx->ctx_id, ctx->link_hdl);
	}

	return rc;
}

void __cam_isp_ctx_notify_cpas(struct cam_context *ctx, uint32_t evt_id)
{
	uint64_t request_id = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *)ctx->ctx_priv;
	struct cam_ctx_request *req = NULL;
	char ctx_evt_id_string[128];

	switch (evt_id) {
	case CAM_ISP_HW_EVENT_SOF:
		if (!list_empty(&ctx->wait_req_list)) {
			req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request, list);
			request_id = req->request_id;
		} else
			request_id = 0;
		if (ctx_isp->substate_activated == CAM_ISP_CTX_ACTIVATED_EPOCH &&
			!list_empty(&ctx->active_req_list)) {
			req = list_last_entry(&ctx->active_req_list, struct cam_ctx_request, list);
			request_id = req->request_id;
			CAM_DBG(CAM_ISP, "EPCR notify cpas");
		}
		break;
	case CAM_ISP_HW_EVENT_EOF:
		if (!list_empty(&ctx->active_req_list)) {
			req = list_first_entry(&ctx->active_req_list, struct cam_ctx_request, list);
			request_id = req->request_id;
		} else
			request_id = 0;
		break;
	case CAM_ISP_HW_EVENT_EPOCH:
		if (list_empty(&ctx->wait_req_list)) {
			if (!list_empty(&ctx->active_req_list)) {
				req = list_last_entry(&ctx->active_req_list,
					struct cam_ctx_request, list);
				request_id = req->request_id;
			} else
				request_id = 0;
		} else {
			req = list_first_entry(&ctx->wait_req_list, struct cam_ctx_request, list);
			request_id = req->request_id;
		}
		break;
	default:
		return;
	}

	snprintf(ctx_evt_id_string, sizeof(ctx_evt_id_string),
		"%s_frame[%llu]_%s",
		ctx->ctx_id_string,
		ctx_isp->frame_id,
		cam_isp_hw_evt_type_to_string(evt_id));
	CAM_DBG(CAM_ISP, "Substate[%s] ctx: %s frame: %llu evt: %s req: %llu",
		__cam_isp_ctx_substate_val_to_type(ctx_isp->substate_activated),
		ctx->ctx_id_string,
		ctx_isp->frame_id,
		cam_isp_hw_evt_type_to_string(evt_id),
		request_id);
	cam_cpas_notify_event(ctx_evt_id_string, request_id);
}

static int __cam_isp_ctx_handle_irq_in_activated(void *context,
	uint32_t evt_id, void *evt_data)
{
	int rc = 0;
	struct cam_isp_ctx_irq_ops *irq_ops = NULL;
	struct cam_context *ctx = (struct cam_context *)context;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *)ctx->ctx_priv;

	spin_lock(&ctx->lock);
	trace_cam_isp_activated_irq(ctx, ctx_isp->substate_activated, evt_id,
		__cam_isp_ctx_get_event_ts(evt_id, evt_data));

	CAM_DBG(CAM_ISP, "Enter: State %d, Substate[%s], evt id %d, ctx:%u link: 0x%x",
		ctx->state, __cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), evt_id,
		ctx->ctx_id, ctx->link_hdl);
	irq_ops = &ctx_isp->substate_machine_irq[ctx_isp->substate_activated];
	if (irq_ops->irq_ops[evt_id]) {
		rc = irq_ops->irq_ops[evt_id](ctx_isp, evt_data);
	} else {
		CAM_DBG(CAM_ISP,
			"No handle function for Substate[%s], evt id %d, ctx:%u link: 0x%x",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated), evt_id,
			ctx->ctx_id, ctx->link_hdl);
		if (isp_ctx_debug.enable_state_monitor_dump)
			__cam_isp_ctx_dump_state_monitor_array(ctx_isp);
	}

	if ((evt_id == CAM_ISP_HW_EVENT_SOF) ||
		(evt_id == CAM_ISP_HW_EVENT_EOF) ||
		(evt_id == CAM_ISP_HW_EVENT_EPOCH))
		__cam_isp_ctx_update_frame_timing_record(evt_id, ctx_isp);

	__cam_isp_ctx_notify_cpas(ctx, evt_id);
	CAM_DBG(CAM_ISP, "Exit: State %d Substate[%s], ctx: %u link: 0x%x",
		ctx->state, __cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated), ctx->ctx_id, ctx->link_hdl);

	spin_unlock(&ctx->lock);
	return rc;
}

static int cam_isp_context_validate_event_notify_injection(struct cam_context *ctx,
	struct cam_hw_inject_evt_param *evt_params)
{
	int rc = 0;
	uint32_t evt_type;
	uint64_t req_id;

	req_id   = evt_params->req_id;
	evt_type = evt_params->u.evt_notify.evt_notify_type;

	switch (evt_type) {
	case V4L_EVENT_CAM_REQ_MGR_ERROR: {
		struct cam_hw_inject_err_evt_param *err_evt_params =
			&evt_params->u.evt_notify.u.err_evt_params;

		switch (err_evt_params->err_type) {
		case CAM_REQ_MGR_ERROR_TYPE_RECOVERY:
		case CAM_REQ_MGR_ERROR_TYPE_SOF_FREEZE:
		case CAM_REQ_MGR_ERROR_TYPE_FULL_RECOVERY:
		case CAM_REQ_MGR_WARN_TYPE_KMD_RECOVERY:
			break;
		default:
			CAM_ERR(CAM_ISP,
				"Invalid error type: %u for error event injection err type: %u req id: %llu ctx id: %u link: 0x%x dev hdl: %d",
				err_evt_params->err_type, err_evt_params->err_code,
				req_id, ctx->ctx_id, ctx->link_hdl, ctx->dev_hdl);
			return -EINVAL;
		}

		CAM_INFO(CAM_ISP,
			"Inject ERR evt: err code: %u err type: %u req id: %llu ctx id: %u link: 0x%x dev hdl: %d",
			err_evt_params->err_code, err_evt_params->err_type,
			req_id, ctx->ctx_id, ctx->link_hdl, ctx->dev_hdl);
		break;
	}
	case V4L_EVENT_CAM_REQ_MGR_PF_ERROR: {
		struct cam_hw_inject_pf_evt_param *pf_evt_params =
			&evt_params->u.evt_notify.u.pf_evt_params;
		bool non_fatal_en;

		rc = cam_smmu_is_cb_non_fatal_fault_en(ctx->img_iommu_hdl, &non_fatal_en);
		if (rc) {
			CAM_ERR(CAM_ISP,
				"Fail to query whether device's cb has non-fatal enabled rc:%d, ctx id: %u link: 0x%x",
				rc, ctx->ctx_id, ctx->link_hdl);
			return rc;
		}

		if (!non_fatal_en) {
			CAM_ERR(CAM_ISP,
				"Fail to inject pagefault event notif. Pagefault fatal for ISP,ctx:%u link:0x%x",
				ctx->ctx_id, ctx->link_hdl);
			return -EINVAL;
		}

		CAM_INFO(CAM_ISP,
			"Inject PF evt: req_id:%llu ctx:%u link:0x%x dev hdl:%d ctx found:%hhu",
			req_id, ctx->ctx_id, ctx->link_hdl, ctx->dev_hdl,
			pf_evt_params->ctx_found);
		break;
	}
	default:
		CAM_ERR(CAM_ISP, "Event notification type not supported: %u, ctx: %u link: 0x%x",
			evt_type, ctx->ctx_id, ctx->link_hdl);
		rc = -EINVAL;
	}

	return rc;
}

static int cam_isp_context_inject_evt(void *context, void *evt_args)
{
	struct cam_context *ctx = context;
	struct cam_isp_context *ctx_isp = NULL;
	struct cam_hw_inject_evt_param *evt_params = evt_args;
	int rc = 0;

	if (!ctx || !evt_args) {
		CAM_ERR(CAM_ISP,
			"Invalid params ctx %s event args %s",
			CAM_IS_NULL_TO_STR(ctx), CAM_IS_NULL_TO_STR(evt_args));
		return -EINVAL;
	}

	ctx_isp = ctx->ctx_priv;
	if (evt_params->inject_id == CAM_COMMON_EVT_INJECT_NOTIFY_EVENT_TYPE) {
		rc = cam_isp_context_validate_event_notify_injection(ctx, evt_params);
		if (rc) {
			CAM_ERR(CAM_ISP,
				"Event notif injection failed validation rc:%d, ctx:%u link:0x%x",
				rc, ctx->ctx_id, ctx->link_hdl);
			return rc;
		}
	} else {
		CAM_ERR(CAM_ISP, "Bufdone err injection %u not supported by ISP,ctx:%u link:0x%x",
			evt_params->inject_id, ctx->ctx_id, ctx->link_hdl);
		return -EINVAL;
	}


	memcpy(&ctx_isp->evt_inject_params, evt_params,
		sizeof(struct cam_hw_inject_evt_param));

	ctx_isp->evt_inject_params.is_valid = true;

	return rc;
}

/* top state machine */
static struct cam_ctx_ops
	cam_isp_ctx_top_state_machine[CAM_CTX_STATE_MAX] = {
	/* Uninit */
	{
		.ioctl_ops = {},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* Available */
	{
		.ioctl_ops = {
			.acquire_dev = __cam_isp_ctx_acquire_dev_in_available,
		},
		.crm_ops = {},
		.irq_ops = NULL,
	},
	/* Acquired */
	{
		.ioctl_ops = {
			.acquire_hw = __cam_isp_ctx_acquire_hw_in_acquired,
			.release_dev = __cam_isp_ctx_release_dev_in_top_state,
			.config_dev = __cam_isp_ctx_config_dev_in_acquired,
			.flush_dev = __cam_isp_ctx_flush_dev_in_top_state,
			.release_hw = __cam_isp_ctx_release_hw_in_top_state,
		},
		.crm_ops = {
			.link = __cam_isp_ctx_link_in_acquired,
			.unlink = __cam_isp_ctx_unlink_in_acquired,
			.get_dev_info = __cam_isp_ctx_get_dev_info,
			.process_evt = __cam_isp_ctx_process_evt,
			.flush_req = __cam_isp_ctx_flush_req_in_top_state,
			.dump_req = __cam_isp_ctx_dump_in_top_state,
		},
		.irq_ops = NULL,
		.pagefault_ops = cam_isp_context_dump_requests,
		.dumpinfo_ops = cam_isp_context_info_dump,
		.evt_inject_ops = cam_isp_context_inject_evt,
	},
	/* Ready */
	{
		.ioctl_ops = {
			.start_dev = __cam_isp_ctx_start_dev_in_ready,
			.release_dev = __cam_isp_ctx_release_dev_in_top_state,
			.config_dev = __cam_isp_ctx_config_dev_in_top_state,
			.flush_dev = __cam_isp_ctx_flush_dev_in_top_state,
			.release_hw = __cam_isp_ctx_release_hw_in_top_state,
		},
		.crm_ops = {
			.unlink = __cam_isp_ctx_unlink_in_ready,
			.get_dev_info = __cam_isp_ctx_get_dev_info,
			.flush_req = __cam_isp_ctx_flush_req_in_ready,
			.dump_req = __cam_isp_ctx_dump_in_top_state,
		},
		.irq_ops = NULL,
		.pagefault_ops = cam_isp_context_dump_requests,
		.dumpinfo_ops = cam_isp_context_info_dump,
		.evt_inject_ops = cam_isp_context_inject_evt,
	},
	/* Flushed */
	{
		.ioctl_ops = {
			.stop_dev = __cam_isp_ctx_stop_dev_in_activated,
			.release_dev = __cam_isp_ctx_release_dev_in_activated,
			.config_dev = __cam_isp_ctx_config_dev_in_flushed,
			.release_hw = __cam_isp_ctx_release_hw_in_activated,
		},
		.crm_ops = {
			.unlink = __cam_isp_ctx_unlink_in_ready,
			.process_evt = __cam_isp_ctx_process_evt,
			.flush_req = __cam_isp_ctx_flush_req_in_flushed_state,
		},
		.irq_ops = NULL,
		.pagefault_ops = cam_isp_context_dump_requests,
		.dumpinfo_ops = cam_isp_context_info_dump,
		.evt_inject_ops = cam_isp_context_inject_evt,
		.msg_cb_ops = cam_isp_context_handle_message,
	},
	/* Activated */
	{
		.ioctl_ops = {
			.stop_dev = __cam_isp_ctx_stop_dev_in_activated,
			.release_dev = __cam_isp_ctx_release_dev_in_activated,
			.config_dev = __cam_isp_ctx_config_dev_in_top_state,
			.flush_dev = __cam_isp_ctx_flush_dev_in_top_state,
			.release_hw = __cam_isp_ctx_release_hw_in_activated,
		},
		.crm_ops = {
			.unlink = __cam_isp_ctx_unlink_in_activated,
			.apply_req = __cam_isp_ctx_apply_req,
			.notify_frame_skip =
				__cam_isp_ctx_apply_default_settings,
			.flush_req = __cam_isp_ctx_flush_req_in_top_state,
			.process_evt = __cam_isp_ctx_process_evt,
			.dump_req = __cam_isp_ctx_dump_in_top_state,
		},
		.irq_ops = __cam_isp_ctx_handle_irq_in_activated,
		.pagefault_ops = cam_isp_context_dump_requests,
		.dumpinfo_ops = cam_isp_context_info_dump,
		.recovery_ops = cam_isp_context_hw_recovery,
		.evt_inject_ops = cam_isp_context_inject_evt,
		.msg_cb_ops = cam_isp_context_handle_message,
	},
};

static int cam_isp_context_hw_recovery(void *priv, void *data)
{
	struct cam_context *ctx = priv;
	int rc = -EPERM;

	if (ctx->hw_mgr_intf->hw_recovery)
		rc = ctx->hw_mgr_intf->hw_recovery(ctx->hw_mgr_intf->hw_mgr_priv, data);
	else
		CAM_ERR(CAM_ISP, "hw mgr doesn't support recovery, ctx_idx: %u, link: 0x%x",
			ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static void cam_isp_context_find_faulted_context(struct cam_context *ctx,
	struct list_head *req_list, struct cam_hw_dump_pf_args *pf_args, bool *found)
{
	struct cam_ctx_request *req = NULL;
	struct cam_ctx_request *req_temp = NULL;
	int rc;

	*found = false;
	list_for_each_entry_safe(req, req_temp, req_list, list) {
		CAM_INFO(CAM_ISP, "List req_id: %llu ctx id: %u link: 0x%x",
			req->request_id, ctx->ctx_id, ctx->link_hdl);

		rc = cam_context_dump_pf_info_to_hw(ctx, pf_args, &req->pf_data);
		if (rc)
			CAM_ERR(CAM_ISP, "Failed to dump pf info, ctx_idx: %u, link: 0x%x",
				ctx->ctx_id, ctx->link_hdl);
		/*
		 * Found faulted buffer. Even if faulted ctx is found, but
		 * continue to search for faulted buffer
		 */
		if (pf_args->pf_context_info.mem_type != CAM_FAULT_BUF_NOT_FOUND) {
			*found = true;
			break;
		}
	}
}

static int cam_isp_context_dump_requests(void *data, void *args)
{
	struct cam_context         *ctx = (struct cam_context *)data;
	struct cam_isp_context     *ctx_isp;
	struct cam_hw_dump_pf_args *pf_args = (struct cam_hw_dump_pf_args *)args;
	int rc = 0;
	bool found;

	if (!ctx || !pf_args) {
		CAM_ERR(CAM_ISP, "Invalid ctx %pK or pf args %pK",
			ctx, pf_args);
		return -EINVAL;
	}

	ctx_isp = (struct cam_isp_context *)ctx->ctx_priv;
	if (!ctx_isp) {
		CAM_ERR(CAM_ISP, "Invalid isp ctx");
		return -EINVAL;
	}

	if (pf_args->handle_sec_pf)
		goto end;

	CAM_INFO(CAM_ISP,
		"Iterating over active list for isp ctx %u link: 0x%x state %d",
		ctx->ctx_id, ctx->link_hdl, ctx->state);
	cam_isp_context_find_faulted_context(ctx, &ctx->active_req_list,
		pf_args, &found);
	if (found)
		goto end;

	CAM_INFO(CAM_ISP,
		"Iterating over waiting list of isp ctx %u link: 0x%x state %d",
		ctx->ctx_id, ctx->link_hdl, ctx->state);
	cam_isp_context_find_faulted_context(ctx, &ctx->wait_req_list,
		pf_args, &found);
	if (found)
		goto end;

	/*
	 * In certain scenarios we observe both overflow and SMMU pagefault
	 * for a particular request. If overflow is handled before page fault
	 * we need to traverse through pending request list because if
	 * bubble recovery is enabled on any request we move that request
	 * and all the subsequent requests to the pending list while handling
	 * overflow error.
	 */
	CAM_INFO(CAM_ISP,
		"Iterating over pending req list of isp ctx %u link: 0x%x state %d",
		ctx->ctx_id, ctx->link_hdl, ctx->state);
	cam_isp_context_find_faulted_context(ctx, &ctx->pending_req_list,
		pf_args, &found);
	if (found)
		goto end;

end:
	if (pf_args->pf_context_info.resource_type) {
		ctx_isp = (struct cam_isp_context *)ctx->ctx_priv;
		CAM_INFO(CAM_ISP,
			"Page fault on resource:%s (0x%x) ctx id:%u link: 0x%x frame id:%d reported id:%lld applied id:%lld",
			__cam_isp_resource_handle_id_to_type(ctx_isp->isp_device_type,
			pf_args->pf_context_info.resource_type),
			pf_args->pf_context_info.resource_type,
			ctx->ctx_id, ctx->link_hdl, ctx_isp->frame_id,
			ctx_isp->reported_req_id, ctx_isp->last_applied_req_id);
	}

	/*
	 * Send PF notification to UMD if PF found on current CTX
	 * or it is forced to send PF notification to UMD even if no
	 * faulted context found
	 */
	if (pf_args->pf_context_info.ctx_found ||
			pf_args->pf_context_info.force_send_pf_evt)
		rc = cam_context_send_pf_evt(ctx, pf_args);
	if (rc)
		CAM_ERR(CAM_ISP,
			"Failed to notify PF event to userspace rc: %d, ctx id:%u link: 0x%x",
			rc,  ctx->ctx_id, ctx->link_hdl);

	return rc;
}

static int cam_isp_context_handle_message(void *context,
	uint32_t msg_type, void *data)
{
	int                            rc = -EINVAL;
	struct cam_hw_cmd_args         hw_cmd_args = {0};
	struct cam_isp_hw_cmd_args     isp_hw_cmd_args = {0};
	struct cam_context            *ctx = (struct cam_context *)context;

	hw_cmd_args.ctxt_to_hw_map = ctx->ctxt_to_hw_map;

	switch (msg_type) {
	case CAM_SUBDEV_MESSAGE_CLOCK_UPDATE:
		hw_cmd_args.cmd_type = CAM_HW_MGR_CMD_INTERNAL;
		isp_hw_cmd_args.cmd_type = CAM_ISP_HW_MGR_CMD_UPDATE_CLOCK;
		isp_hw_cmd_args.cmd_data = data;
		hw_cmd_args.u.internal_args = (void *)&isp_hw_cmd_args;
		rc = ctx->hw_mgr_intf->hw_cmd(ctx->hw_mgr_intf->hw_mgr_priv,
			&hw_cmd_args);

		if (rc)
			CAM_ERR(CAM_ISP, "Update clock rate failed rc: %d", rc);
		break;
	default:
		CAM_ERR(CAM_ISP, "Invalid message type %d", msg_type);
	}
	return rc;
}

static int cam_isp_context_debug_register(void)
{
	int rc = 0;
	struct dentry *dbgfileptr = NULL;

	if (!cam_debugfs_available())
		return 0;

	rc = cam_debugfs_create_subdir("isp_ctx", &dbgfileptr);
	if (rc) {
		CAM_ERR(CAM_ISP, "DebugFS could not create directory!");
		return rc;
	}

	/* Store parent inode for cleanup in caller */
	isp_ctx_debug.dentry = dbgfileptr;

	debugfs_create_u32("enable_state_monitor_dump", 0644,
		isp_ctx_debug.dentry, &isp_ctx_debug.enable_state_monitor_dump);
	debugfs_create_u8("enable_cdm_cmd_buffer_dump", 0644,
		isp_ctx_debug.dentry, &isp_ctx_debug.enable_cdm_cmd_buff_dump);
	debugfs_create_u32("disable_internal_recovery_mask", 0644,
		isp_ctx_debug.dentry, &isp_ctx_debug.disable_internal_recovery_mask);

	return 0;
}

int cam_isp_context_init(struct cam_isp_context *ctx,
	struct cam_context *ctx_base,
	struct cam_req_mgr_kmd_ops *crm_node_intf,
	struct cam_hw_mgr_intf *hw_intf,
	uint32_t ctx_id,
	uint32_t isp_device_type,
	int img_iommu_hdl)
{
	int rc = -1;
	int i;
	struct cam_isp_skip_frame_info *skip_info, *temp;

	if (!ctx || !ctx_base) {
		CAM_ERR(CAM_ISP, "Invalid Context");
		goto err;
	}

	/* ISP context setup */
	memset(ctx, 0, sizeof(*ctx));

	ctx->base = ctx_base;
	ctx->frame_id = 0;
	ctx->custom_enabled = false;
	ctx->use_frame_header_ts = false;
	ctx->use_default_apply = false;
	ctx->active_req_cnt = 0;
	ctx->reported_req_id = 0;
	ctx->bubble_frame_cnt = 0;
	ctx->congestion_cnt = 0;
	ctx->req_info.last_bufdone_req_id = 0;
	ctx->v4l2_event_sub_ids = 0;

	ctx->hw_ctx = NULL;
	ctx->substate_activated = CAM_ISP_CTX_ACTIVATED_SOF;
	ctx->substate_machine = cam_isp_ctx_activated_state_machine;
	ctx->substate_machine_irq = cam_isp_ctx_activated_state_machine_irq;
	ctx->init_timestamp = jiffies_to_msecs(jiffies);
	ctx->isp_device_type = isp_device_type;

	for (i = 0; i < CAM_ISP_CTX_REQ_MAX; i++) {
		ctx->req_base[i].req_priv = &ctx->req_isp[i];
		ctx->req_isp[i].base = &ctx->req_base[i];
	}

	/* camera context setup */
	rc = cam_context_init(ctx_base, isp_dev_name, CAM_ISP, ctx_id,
		crm_node_intf, hw_intf, ctx->req_base, CAM_ISP_CTX_REQ_MAX, img_iommu_hdl);
	if (rc) {
		CAM_ERR(CAM_ISP, "Camera Context Base init failed, ctx_idx: %u, link: 0x%x",
			ctx_base->ctx_id, ctx_base->link_hdl);
		goto free_mem;
	}

	/* FCG related struct setup */
	INIT_LIST_HEAD(&ctx->fcg_tracker.skipped_list);
	for (i = 0; i < CAM_ISP_AFD_PIPELINE_DELAY; i++) {
		skip_info = kzalloc(sizeof(struct cam_isp_skip_frame_info), GFP_KERNEL);
		if (!skip_info) {
			CAM_ERR(CAM_ISP,
				"Failed to allocate memory for FCG struct, ctx_idx: %u, link: %x",
				ctx_base->ctx_id, ctx_base->link_hdl);
			rc = -ENOMEM;
			goto free_mem;
		}

		list_add_tail(&skip_info->list, &ctx->fcg_tracker.skipped_list);
	}

	/* link camera context with isp context */
	ctx_base->state_machine = cam_isp_ctx_top_state_machine;
	ctx_base->ctx_priv = ctx;

	/* initializing current state for error logging */
	for (i = 0; i < CAM_ISP_CTX_STATE_MONITOR_MAX_ENTRIES; i++) {
		ctx->dbg_monitors.state_monitor[i].curr_state =
		CAM_ISP_CTX_ACTIVATED_MAX;
	}
	atomic64_set(&ctx->dbg_monitors.state_monitor_head, -1);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx->dbg_monitors.event_record_head[i], -1);

	atomic64_set(&ctx->dbg_monitors.frame_monitor_head, -1);

	if (!isp_ctx_debug.dentry)
		cam_isp_context_debug_register();

	return rc;

free_mem:
	list_for_each_entry_safe(skip_info, temp,
		&ctx->fcg_tracker.skipped_list, list) {
		list_del(&skip_info->list);
		kfree(skip_info);
		skip_info = NULL;
	}
err:
	return rc;
}

int cam_isp_context_deinit(struct cam_isp_context *ctx)
{
	struct cam_isp_skip_frame_info *skip_info, *temp;

	list_for_each_entry_safe(skip_info, temp,
		&ctx->fcg_tracker.skipped_list, list) {
		list_del(&skip_info->list);
		kfree(skip_info);
		skip_info = NULL;
	}

	if (ctx->base)
		cam_context_deinit(ctx->base);

	if (ctx->substate_activated != CAM_ISP_CTX_ACTIVATED_SOF)
		CAM_ERR(CAM_ISP, "ISP context Substate[%s] is invalid",
			__cam_isp_ctx_substate_val_to_type(
			ctx->substate_activated));

	isp_ctx_debug.dentry = NULL;
	memset(ctx, 0, sizeof(*ctx));

	return 0;
}

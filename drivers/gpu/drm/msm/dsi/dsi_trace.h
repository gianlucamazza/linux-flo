/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_MSM_DSI_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _MSM_DSI_TRACE_H_

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM drm_msm_dsi

#define TRACE_INCLUDE_FILE dsi_trace

TRACE_EVENT(msm_dsi_error_reg,
	TP_PROTO(int id, const char *reg, u32 value),
	TP_ARGS(id, reg, value),
	TP_STRUCT__entry(
		__field(int, id)
		__string(reg, reg)
		__field(u32, value)
	),
	TP_fast_assign(
		__entry->id = id;
		__assign_str(reg);
		__entry->value = value;
	),
	TP_printk("id=%d reg=%s value=0x%08x",
		  __entry->id, __get_str(reg), __entry->value)
);

TRACE_EVENT(msm_dsi_phase,
	TP_PROTO(int id, const char *phase, int value),
	TP_ARGS(id, phase, value),
	TP_STRUCT__entry(
		__field(int, id)
		__string(phase, phase)
		__field(int, value)
	),
	TP_fast_assign(
		__entry->id = id;
		__assign_str(phase);
		__entry->value = value;
	),
	TP_printk("id=%d phase=%s value=%d",
		  __entry->id, __get_str(phase), __entry->value)
);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm/msm/dsi
#include <trace/define_trace.h>

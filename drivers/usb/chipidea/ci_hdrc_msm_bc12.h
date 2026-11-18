/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CI_HDRC_MSM_BC12_H
#define __CI_HDRC_MSM_BC12_H

#include <linux/types.h>
#include <linux/power_supply.h>

enum ci_msm_bc12_type {
	CI_MSM_BC12_UNKNOWN,
	CI_MSM_BC12_SDP,
	CI_MSM_BC12_CDP,
	CI_MSM_BC12_DCP,
	CI_MSM_BC12_PROPRIETARY,
};

struct ci_msm_bc12_source_state {
	bool online;
	enum power_supply_usb_type usb_type;
	int current_max;
};

bool ci_msm_bc12_source_complete(struct ci_msm_bc12_source_state *source,
				  bool vbus_active,
				  enum ci_msm_bc12_type type);
bool ci_msm_bc12_source_needs_detection(
	const struct ci_msm_bc12_source_state *source,
	bool vbus_active, bool gadget_role);

bool ci_msm_bc12_source_stop(struct ci_msm_bc12_source_state *source,
			     bool vbus_active, bool gadget_role);

struct ci_msm_bc12_io {
	void *context;
	int (*read)(void *context, u8 register_address);
	int (*write)(void *context, u8 register_address, u8 value);
	int (*line_state)(void *context);
	void (*wait_ms)(void *context, unsigned int milliseconds);
};

int ci_msm_bc12_detect(const struct ci_msm_bc12_io *io,
		       enum ci_msm_bc12_type *type);

#endif

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CI_HDRC_MSM_BC12_H
#define __CI_HDRC_MSM_BC12_H

#include <linux/types.h>

enum ci_msm_bc12_type {
	CI_MSM_BC12_UNKNOWN,
	CI_MSM_BC12_SDP,
	CI_MSM_BC12_CDP,
	CI_MSM_BC12_DCP,
};

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

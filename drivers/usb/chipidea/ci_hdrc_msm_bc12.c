// SPDX-License-Identifier: GPL-2.0
/* Battery Charging 1.2 detection on the APQ8064 Synopsys 28 nm PHY. */

#include <linux/errno.h>
#include <linux/bits.h>
#include <linux/ulpi/regs.h>

#include "ci_hdrc_msm_bc12.h"

#define SNPS_CHG_CTRL_SET	0x85
#define SNPS_CHG_CTRL_CLR	0x86
#define SNPS_CHG_STATUS		0x87
#define SNPS_ALT_INT_LATCH_CLR	0x92
#define SNPS_ALT_INT_EN_CLR	0x95

bool ci_msm_bc12_source_complete(struct ci_msm_bc12_source_state *source,
				  bool vbus_active,
				  enum ci_msm_bc12_type type)
{
	enum power_supply_usb_type usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	int current_max = 0;
	bool changed;

	if (vbus_active) {
		current_max = 100000;
		switch (type) {
		case CI_MSM_BC12_SDP:
			usb_type = POWER_SUPPLY_USB_TYPE_SDP;
			break;
		case CI_MSM_BC12_CDP:
			usb_type = POWER_SUPPLY_USB_TYPE_CDP;
			current_max = 1200000;
			break;
		case CI_MSM_BC12_DCP:
			usb_type = POWER_SUPPLY_USB_TYPE_DCP;
			current_max = 1200000;
			break;
		case CI_MSM_BC12_PROPRIETARY:
			current_max = 1200000;
			break;
		default:
			break;
		}
	}

	changed = source->online != vbus_active ||
		  source->usb_type != usb_type ||
		  source->current_max != current_max;
	source->online = vbus_active;
	source->usb_type = usb_type;
	source->current_max = current_max;
	return changed;
}

bool ci_msm_bc12_source_stop(struct ci_msm_bc12_source_state *source,
			     bool vbus_active, bool gadget_role)
{
	if (vbus_active && gadget_role)
		return false;
	if (!source->online &&
	    source->usb_type == POWER_SUPPLY_USB_TYPE_UNKNOWN &&
	    !source->current_max)
		return false;

	source->online = false;
	source->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	source->current_max = 0;
	return true;
}

static int bc12_clear(const struct ci_msm_bc12_io *io)
{
	int ret, first;

	first = io->write(io->context, SNPS_CHG_CTRL_CLR, 0x3f);
	ret = io->write(io->context, SNPS_ALT_INT_LATCH_CLR, 0x1f);
	if (!first)
		first = ret;
	ret = io->write(io->context, SNPS_ALT_INT_EN_CLR, 0x1f);
	if (!first)
		first = ret;

	return first;
}

int ci_msm_bc12_detect(const struct ci_msm_bc12_io *io,
		       enum ci_msm_bc12_type *type)
{
	int func, otg, status, ret, cleanup;
	unsigned int i;
	bool primary;

	*type = CI_MSM_BC12_UNKNOWN;
	func = io->read(io->context, ULPI_FUNC_CTRL);
	if (func < 0)
		return func;
	/* A PHY reset may clear SUSPENDM; keep ULPI active for detection. */
	func |= ULPI_FUNC_CTRL_SUSPENDM;
	otg = io->read(io->context, ULPI_OTG_CTRL);
	if (otg < 0)
		return otg;

	ret = io->write(io->context, ULPI_FUNC_CTRL,
			(func & ~ULPI_FUNC_CTRL_OPMODE_MASK) |
			ULPI_FUNC_CTRL_OPMODE_NONDRIVING);
	if (ret)
		goto restore;
	ret = io->write(io->context, ULPI_CLR(ULPI_OTG_CTRL),
			ULPI_OTG_CTRL_DP_PULLDOWN | ULPI_OTG_CTRL_DM_PULLDOWN);
	if (ret)
		goto restore;
	ret = bc12_clear(io);
	if (ret)
		goto restore;
	io->wait_ms(io->context, 1);

	ret = io->write(io->context, SNPS_CHG_CTRL_SET, 0x10);
	if (ret)
		goto restore;
	for (i = 0; i < 15; i++) {
		io->wait_ms(io->context, 50);
		status = io->read(io->context, SNPS_CHG_STATUS);
		if (status < 0) {
			ret = status;
			goto restore;
		}
		if (status & BIT(1))
			break;
	}
	ret = io->write(io->context, SNPS_CHG_CTRL_CLR, 0x10);
	if (ret)
		goto restore;
	ret = io->write(io->context, SNPS_CHG_CTRL_SET, 0x02);
	if (ret)
		goto restore;
	ret = io->write(io->context, SNPS_CHG_CTRL_SET, 0x01);
	if (ret)
		goto restore;
	io->wait_ms(io->context, 50);
	status = io->read(io->context, SNPS_CHG_STATUS);
	if (status < 0) {
		ret = status;
		goto restore;
	}
	primary = !!(status & BIT(0));
	status = io->line_state(io->context);
	if (status < 0) {
		ret = status;
		goto restore;
	}
	if (status) {
		*type = CI_MSM_BC12_PROPRIETARY;
		goto restore;
	}
	if (!primary) {
		*type = CI_MSM_BC12_SDP;
		goto restore;
	}

	ret = io->write(io->context, SNPS_CHG_CTRL_SET, 0x08);
	if (ret)
		goto restore;
	ret = io->write(io->context, SNPS_CHG_CTRL_SET, 0x02);
	if (ret)
		goto restore;
	ret = io->write(io->context, SNPS_CHG_CTRL_SET, 0x01);
	if (ret)
		goto restore;
	io->wait_ms(io->context, 40);
	status = io->read(io->context, SNPS_CHG_STATUS);
	if (status < 0) {
		ret = status;
		goto restore;
	}
	*type = status & BIT(0) ? CI_MSM_BC12_DCP : CI_MSM_BC12_CDP;

restore:
	cleanup = bc12_clear(io);
	if (!ret)
		ret = cleanup;
	cleanup = io->write(io->context, ULPI_OTG_CTRL, otg);
	if (!ret)
		ret = cleanup;
	cleanup = io->write(io->context, ULPI_FUNC_CTRL, func);
	if (!ret)
		ret = cleanup;
	if (ret)
		*type = CI_MSM_BC12_UNKNOWN;
	return ret;
}

#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12_KUNIT_TEST)
#include "ci_hdrc_msm_bc12_test.c"
#endif

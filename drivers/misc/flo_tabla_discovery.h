/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FLO_TABLA_DISCOVERY_H
#define _FLO_TABLA_DISCOVERY_H

#include <linux/types.h>

enum flo_tabla_action {
	FLO_TABLA_CONTROLLER_GET,
	FLO_TABLA_S4_ENABLE,
	FLO_TABLA_L25_ENABLE,
	FLO_TABLA_RESET_ASSERT,
	FLO_TABLA_RESET_DEASSERT,
	FLO_TABLA_WAIT_RESET,
	FLO_TABLA_WAIT_BOOT,
	FLO_TABLA_MCLK_RATE,
	FLO_TABLA_MCLK_ENABLE,
	FLO_TABLA_MCLK_DISABLE,
	FLO_TABLA_L25_DISABLE,
	FLO_TABLA_S4_DISABLE,
	FLO_TABLA_CONTROLLER_PUT,
};

struct flo_tabla_power {
	int (*apply)(void *ctx, enum flo_tabla_action action);
	void *ctx;
	bool enabled;
	bool controller_on;
	bool s4_on;
	bool l25_on;
	bool mclk_on;
	bool reset_asserted;
};

int flo_tabla_power_set(struct flo_tabla_power *power, bool enable);

#endif

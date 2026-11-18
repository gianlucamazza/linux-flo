// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include "flo_tabla_discovery.h"

struct tabla_test_context {
	struct flo_tabla_power power;
	enum flo_tabla_action actions[64];
	int count;
	int fail_at;
	int fail_error;
	bool put_drops_on_error;
	bool controller_on;
	bool s4_on;
	bool l25_on;
	bool mclk_on;
	bool reset_asserted;
};

static int tabla_test_apply(void *data, enum flo_tabla_action action)
{
	struct tabla_test_context *ctx = data;

	ctx->actions[ctx->count++] = action;
	if (action == ctx->fail_at) {
		if (action == FLO_TABLA_CONTROLLER_PUT &&
		    ctx->put_drops_on_error)
			ctx->controller_on = false;
		return ctx->fail_error;
	}
	switch (action) {
	case FLO_TABLA_CONTROLLER_GET:
		ctx->controller_on = true;
		break;
	case FLO_TABLA_CONTROLLER_PUT:
		ctx->controller_on = false;
		break;
	case FLO_TABLA_S4_ENABLE:
		ctx->s4_on = true;
		break;
	case FLO_TABLA_S4_DISABLE:
		ctx->s4_on = false;
		break;
	case FLO_TABLA_L25_ENABLE:
		ctx->l25_on = true;
		break;
	case FLO_TABLA_L25_DISABLE:
		ctx->l25_on = false;
		break;
	case FLO_TABLA_MCLK_ENABLE:
		ctx->mclk_on = true;
		break;
	case FLO_TABLA_MCLK_DISABLE:
		ctx->mclk_on = false;
		break;
	case FLO_TABLA_RESET_ASSERT:
		ctx->reset_asserted = true;
		break;
	case FLO_TABLA_RESET_DEASSERT:
		ctx->reset_asserted = false;
		break;
	default:
		break;
	}
	return 0;
}

static void tabla_test_init(struct tabla_test_context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->fail_at = -1;
	ctx->fail_error = -EIO;
	ctx->power.apply = tabla_test_apply;
	ctx->power.ctx = ctx;
	ctx->power.reset_asserted = true;
	ctx->reset_asserted = true;
}

static void tabla_test_off(struct kunit *test, struct tabla_test_context *ctx)
{
	KUNIT_EXPECT_FALSE(test, ctx->power.enabled);
	KUNIT_EXPECT_FALSE(test, ctx->power.controller_on);
	KUNIT_EXPECT_FALSE(test, ctx->power.s4_on);
	KUNIT_EXPECT_FALSE(test, ctx->power.l25_on);
	KUNIT_EXPECT_FALSE(test, ctx->power.mclk_on);
	KUNIT_EXPECT_FALSE(test, ctx->controller_on);
	KUNIT_EXPECT_FALSE(test, ctx->s4_on);
	KUNIT_EXPECT_FALSE(test, ctx->l25_on);
	KUNIT_EXPECT_FALSE(test, ctx->mclk_on);
	KUNIT_EXPECT_TRUE(test, ctx->reset_asserted);
}

static void tabla_enable_disable_is_balanced(struct kunit *test)
{
	struct tabla_test_context ctx;
	int first_on;

	tabla_test_init(&ctx);
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	KUNIT_EXPECT_TRUE(test, ctx.power.enabled);
	KUNIT_EXPECT_TRUE(test, ctx.controller_on);
	KUNIT_EXPECT_TRUE(test, ctx.l25_on);
	KUNIT_EXPECT_TRUE(test, ctx.mclk_on);
	first_on = ctx.count;
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	KUNIT_EXPECT_EQ(test, ctx.count, first_on);
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	tabla_test_off(test, &ctx);
	KUNIT_EXPECT_EQ(test, ctx.actions[ctx.count - 1], FLO_TABLA_CONTROLLER_PUT);
	first_on = ctx.count;
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	KUNIT_EXPECT_EQ(test, ctx.count, first_on);
}

static void tabla_each_enable_failure_unwinds(struct kunit *test)
{
	static const enum flo_tabla_action failures[] = {
		FLO_TABLA_CONTROLLER_GET, FLO_TABLA_S4_ENABLE,
		FLO_TABLA_L25_ENABLE, FLO_TABLA_RESET_ASSERT,
		FLO_TABLA_WAIT_RESET, FLO_TABLA_RESET_DEASSERT,
		FLO_TABLA_WAIT_BOOT, FLO_TABLA_MCLK_RATE,
		FLO_TABLA_MCLK_ENABLE,
	};
	struct tabla_test_context ctx;
	int i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		tabla_test_init(&ctx);
		ctx.fail_at = failures[i];
		/* Probe holds reset asserted before the first sysfs write. */
		ctx.reset_asserted = true;
		KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, true), -EIO);
		tabla_test_off(test, &ctx);
		if (failures[i] > FLO_TABLA_CONTROLLER_GET)
			KUNIT_EXPECT_EQ(test, ctx.actions[ctx.count - 1],
					FLO_TABLA_CONTROLLER_PUT);
	}
}

static void tabla_unbound_controller_never_powers_codec(struct kunit *test)
{
	struct tabla_test_context ctx;

	tabla_test_init(&ctx);
	ctx.reset_asserted = true;
	ctx.fail_at = FLO_TABLA_CONTROLLER_GET;
	ctx.fail_error = -EPROBE_DEFER;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, true),
			-EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, ctx.count, 1);
	tabla_test_off(test, &ctx);
}

static void tabla_suspend_and_remove_power_off(struct kunit *test)
{
	struct tabla_test_context ctx;

	tabla_test_init(&ctx);
	ctx.reset_asserted = true;
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	tabla_test_off(test, &ctx);
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	tabla_test_off(test, &ctx);
}

static void tabla_failed_pm_put_does_not_drop_reference_twice(struct kunit *test)
{
	struct tabla_test_context ctx;
	int count;

	tabla_test_init(&ctx);
	ctx.reset_asserted = true;
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	ctx.fail_at = FLO_TABLA_CONTROLLER_PUT;
	ctx.fail_error = -EAGAIN;
	ctx.put_drops_on_error = true;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), -EAGAIN);
	KUNIT_EXPECT_FALSE(test, ctx.power.controller_on);
	count = ctx.count;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	KUNIT_EXPECT_EQ(test, ctx.count, count);
	ctx.fail_at = -1;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	tabla_test_off(test, &ctx);
}

static void tabla_failed_reset_assert_keeps_power_for_retry(struct kunit *test)
{
	struct tabla_test_context ctx;

	tabla_test_init(&ctx);
	ctx.reset_asserted = true;
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	ctx.fail_at = FLO_TABLA_RESET_ASSERT;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx.controller_on);
	KUNIT_EXPECT_TRUE(test, ctx.s4_on);
	KUNIT_EXPECT_TRUE(test, ctx.l25_on);
	KUNIT_EXPECT_TRUE(test, ctx.mclk_on);
	ctx.fail_at = -1;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	tabla_test_off(test, &ctx);
}

static void tabla_failed_regulator_disable_keeps_controller(struct kunit *test)
{
	struct tabla_test_context ctx;

	tabla_test_init(&ctx);
	ctx.reset_asserted = true;
	KUNIT_ASSERT_EQ(test, flo_tabla_power_set(&ctx.power, true), 0);
	ctx.fail_at = FLO_TABLA_L25_DISABLE;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx.l25_on);
	KUNIT_EXPECT_TRUE(test, ctx.controller_on);
	KUNIT_EXPECT_TRUE(test, ctx.reset_asserted);
	ctx.fail_at = -1;
	KUNIT_EXPECT_EQ(test, flo_tabla_power_set(&ctx.power, false), 0);
	tabla_test_off(test, &ctx);
}

static struct kunit_case tabla_cases[] = {
	KUNIT_CASE(tabla_enable_disable_is_balanced),
	KUNIT_CASE(tabla_each_enable_failure_unwinds),
	KUNIT_CASE(tabla_unbound_controller_never_powers_codec),
	KUNIT_CASE(tabla_suspend_and_remove_power_off),
	KUNIT_CASE(tabla_failed_pm_put_does_not_drop_reference_twice),
	KUNIT_CASE(tabla_failed_reset_assert_keeps_power_for_retry),
	KUNIT_CASE(tabla_failed_regulator_disable_keeps_controller),
	{}
};

static struct kunit_suite tabla_suite = {
	.name = "flo-tabla-discovery",
	.test_cases = tabla_cases,
};

kunit_test_suite(tabla_suite);
MODULE_LICENSE("GPL");

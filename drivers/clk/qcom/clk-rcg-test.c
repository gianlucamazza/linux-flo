// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/regmap.h>

#include "common.h"
#include "clk-rcg.h"

static const struct parent_map rcg_test_parents[] = { { 0, 0 }, { 1, 3 } };
static const struct freq_tbl rcg_test_rates[] = {
	{    144000, 0, 3, 2, 125 },
	{    400000, 1, 4, 1, 240 },
	{  16000000, 1, 4, 1,   6 },
	{  17070000, 1, 1, 2,  45 },
	{  20210000, 1, 1, 1,  19 },
	{  24000000, 1, 4, 1,   4 },
	{  48000000, 1, 4, 1,   2 },
	{  64000000, 1, 3, 1,   2 },
	{  96000000, 1, 4, 0,   0 },
	{ 192000000, 1, 2, 0,   0 },
	{}
};

struct rcg_test_context {
	struct clk_rcg rcg;
	u32 registers[2];
};

static int rcg_test_read(void *context, unsigned int reg, unsigned int *value)
{
	struct rcg_test_context *ctx = context;

	*value = ctx->registers[reg / sizeof(u32)];
	return 0;
}

static int rcg_test_write(void *context, unsigned int reg, unsigned int value)
{
	struct rcg_test_context *ctx = context;

	ctx->registers[reg / sizeof(u32)] = value;
	return 0;
}

static const struct regmap_bus rcg_test_bus = {
	.reg_read = rcg_test_read,
	.reg_write = rcg_test_write,
};

static const struct regmap_config rcg_test_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = sizeof(u32),
	.max_register = sizeof(u32),
};

static struct clk *rcg_test_register(struct kunit *test, const struct clk_ops *ops,
				     const struct freq_tbl *rates, unsigned long flags)
{
	struct rcg_test_context *ctx;
	struct clk_hw *parents[2];
	struct clk_init_data init = {};
	struct device *dev;
	const char *name;
	int i, ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);
	dev = kunit_device_register(test, "rcg-rate-test");
	if (IS_ERR(dev))
		return ERR_CAST(dev);
	for (i = 0; i < ARRAY_SIZE(parents); i++) {
		name = devm_kasprintf(dev, GFP_KERNEL, "%s-parent%d", dev_name(dev), i);
		if (!name)
			return ERR_PTR(-ENOMEM);
		parents[i] = devm_clk_hw_register_fixed_rate(dev, name, NULL, 0,
							     i ? 384000000 : 27000000);
		if (IS_ERR(parents[i]))
			return ERR_CAST(parents[i]);
	}
	init.name = dev_name(dev);
	init.parent_hws = (const struct clk_hw **)parents;
	init.num_parents = ARRAY_SIZE(parents);
	init.flags = flags;
	init.ops = ops;
	ctx->rcg.ns_reg = 0;
	ctx->rcg.md_reg = 4;
	ctx->rcg.mn = (struct mn) {
		.width = 8, .n_val_shift = 16, .m_val_shift = 16,
		.mnctr_mode_shift = 5, .mnctr_en_bit = 8, .mnctr_reset_bit = 7,
	};
	ctx->rcg.p = (struct pre_div) { .pre_div_shift = 3, .pre_div_width = 2 };
	ctx->rcg.s.parent_map = rcg_test_parents;
	ctx->rcg.freq_tbl = rates;
	ctx->rcg.clkr.hw.init = &init;
	ctx->rcg.clkr.regmap = devm_regmap_init(dev, &rcg_test_bus, ctx,
						&rcg_test_regmap_config);
	if (IS_ERR(ctx->rcg.clkr.regmap))
		return ERR_CAST(ctx->rcg.clkr.regmap);
	ret = devm_clk_hw_register(dev, &ctx->rcg.clkr.hw);
	if (ret)
		return ERR_PTR(ret);
	return devm_clk_hw_get_clk(dev, &ctx->rcg.clkr.hw, NULL);
}

static void rcg_floor_round_and_set(struct kunit *test)
{
	const unsigned long requested[] = { 52000000, 400000, 24000000, 48000000,
					    96000000, 100000, 192000000, 300000000 };
	const unsigned long expected[] = { 48000000, 400000, 24000000, 48000000,
					   96000000, 144000, 192000000, 192000000 };
	struct clk *clk = rcg_test_register(test, &clk_rcg_floor_ops, rcg_test_rates, 0);
	int i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, clk);
	for (i = 0; i < ARRAY_SIZE(requested); i++) {
		KUNIT_EXPECT_EQ(test, clk_round_rate(clk, requested[i]), expected[i]);
		KUNIT_ASSERT_EQ(test, clk_set_rate(clk, requested[i]), 0);
		KUNIT_EXPECT_EQ(test, clk_get_rate(clk), expected[i]);
	}
}

static void rcg_ceiling_policy_is_preserved(struct kunit *test)
{
	struct clk *clk = rcg_test_register(test, &clk_rcg_ops, rcg_test_rates, 0);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, clk);
	KUNIT_EXPECT_EQ(test, clk_round_rate(clk, 52000000), 64000000);
	KUNIT_ASSERT_EQ(test, clk_set_rate(clk, 52000000), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(clk), 64000000);
}

static void rcg_floor_with_parent_rate_propagation(struct kunit *test)
{
	struct clk *clk = rcg_test_register(test, &clk_rcg_floor_ops, rcg_test_rates,
					  CLK_SET_RATE_PARENT);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, clk);
	KUNIT_EXPECT_EQ(test, clk_round_rate(clk, 52000000), 48000000);
	KUNIT_ASSERT_EQ(test, clk_set_rate(clk, 52000000), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(clk), 48000000);
}

static void rcg_empty_frequency_table_is_rejected(struct kunit *test)
{
	static const struct freq_tbl empty[] = { {} };
	struct clk *clk = rcg_test_register(test, &clk_rcg_floor_ops, empty, 0);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, clk);
	KUNIT_EXPECT_LT(test, clk_round_rate(clk, 52000000), 0);
	KUNIT_EXPECT_LT(test, clk_set_rate(clk, 52000000), 0);
}

static void rcg_missing_frequency_table_is_rejected(struct kunit *test)
{
	struct clk *clk = rcg_test_register(test, &clk_rcg_floor_ops, NULL, 0);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, clk);
	KUNIT_EXPECT_LT(test, clk_round_rate(clk, 52000000), 0);
	KUNIT_EXPECT_LT(test, clk_set_rate(clk, 52000000), 0);
}

static struct kunit_case rcg_rate_cases[] = {
	KUNIT_CASE(rcg_floor_round_and_set),
	KUNIT_CASE(rcg_ceiling_policy_is_preserved),
	KUNIT_CASE(rcg_floor_with_parent_rate_propagation),
	KUNIT_CASE(rcg_empty_frequency_table_is_rejected),
	KUNIT_CASE(rcg_missing_frequency_table_is_rejected),
	{}
};

static struct kunit_suite rcg_rate_suite = {
	.name = "qcom-rcg-rate",
	.test_cases = rcg_rate_cases,
};

kunit_test_suite(rcg_rate_suite);
MODULE_LICENSE("GPL");

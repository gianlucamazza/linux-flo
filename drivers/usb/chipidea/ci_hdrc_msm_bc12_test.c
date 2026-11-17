// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>

struct bc12_test_state {
	u8 function_control;
	u8 otg_control;
	u8 phase;
	bool dcd;
	bool primary;
	bool secondary;
	bool proprietary;
	bool failed;
	bool primary_cleared;
	bool phy_suspended;
	unsigned int wait_total_ms;
};

static int bc12_test_read(void *context, u8 address)
{
	struct bc12_test_state *state = context;

	if (address == 0x04)
		return state->function_control;
	if (address == 0x0a)
		return state->otg_control;
	if (address == 0x87) {
		if (state->phase == 1)
			return state->dcd ? BIT(1) : 0;
		if (state->phase == 2)
			return state->primary ? BIT(0) : 0;
		if (state->phase == 3)
			return state->secondary ? BIT(0) : 0;
	}
	return -EINVAL;
}

static int bc12_test_write(void *context, u8 address, u8 value)
{
	struct bc12_test_state *state = context;

	if (state->phy_suspended)
		return -ETIMEDOUT;
	if (address == ULPI_FUNC_CTRL &&
	    (value & ULPI_FUNC_CTRL_OPMODE_MASK) ==
	    ULPI_FUNC_CTRL_OPMODE_NONDRIVING &&
	    !(value & ULPI_FUNC_CTRL_SUSPENDM))
		state->phy_suspended = true;

	if (address == 0x86 && value == 0x3f)
		state->primary_cleared = false;
	if (address == 0x86 && value == 0x03)
		state->primary_cleared = true;
	/* The PHY must stay active between primary and secondary detection. */
	if (address == 0x85 && value == 0x08 && state->primary_cleared)
		return -ETIMEDOUT;

	if (address == 0x85 && value == 0x10)
		state->phase = 1;
	else if (address == 0x85 && value == 0x02 && state->phase == 1)
		state->phase = 2;
	else if (address == 0x85 && value == 0x08)
		state->phase = 3;
	if (address == 0x04)
		state->function_control = value;
	if (address == 0x0a)
		state->otg_control = value;
	if (address == 0x85 && value == 0x01 && state->failed)
		return -EIO;
	return 0;
}

static void bc12_test_wait(void *context, unsigned int milliseconds)
{
	struct bc12_test_state *state = context;

	state->wait_total_ms += milliseconds;
}

static int bc12_test_line_state(void *context)
{
	struct bc12_test_state *state = context;

	return state->proprietary ? 1 : 0;
}

static void bc12_test_classification(struct kunit *test)
{
	struct bc12_test_state state = {
		.function_control = 0x40,
		.otg_control = 0x06,
		.dcd = true,
	};
	struct ci_msm_bc12_io io = {
		.context = &state,
		.read = bc12_test_read,
		.write = bc12_test_write,
		.line_state = bc12_test_line_state,
		.wait_ms = bc12_test_wait,
	};
	enum ci_msm_bc12_type type;
	int ret;

	ret = ci_msm_bc12_detect(&io, &type);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_SDP);
	KUNIT_EXPECT_EQ(test, state.function_control, (u8)0x40);
	KUNIT_EXPECT_EQ(test, state.otg_control, (u8)0x06);

	state.primary = true;
	ret = ci_msm_bc12_detect(&io, &type);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_CDP);

	state.secondary = true;
	ret = ci_msm_bc12_detect(&io, &type);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_DCP);

	state.proprietary = true;
	ret = ci_msm_bc12_detect(&io, &type);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_PROPRIETARY);

	state.primary = false;
	ret = ci_msm_bc12_detect(&io, &type);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_PROPRIETARY);
}

static void bc12_test_error_restores_phy(struct kunit *test)
{
	struct bc12_test_state state = {
		.function_control = 0x40,
		.otg_control = 0x06,
		.dcd = true,
		.failed = true,
	};
	struct ci_msm_bc12_io io = {
		.context = &state,
		.read = bc12_test_read,
		.write = bc12_test_write,
		.line_state = bc12_test_line_state,
		.wait_ms = bc12_test_wait,
	};
	enum ci_msm_bc12_type type = CI_MSM_BC12_DCP;

	KUNIT_EXPECT_EQ(test, ci_msm_bc12_detect(&io, &type), -EIO);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_UNKNOWN);
	KUNIT_EXPECT_EQ(test, state.function_control, (u8)0x40);
	KUNIT_EXPECT_EQ(test, state.otg_control, (u8)0x06);
}

static void bc12_test_rebind_after_phy_reset(struct kunit *test)
{
	struct bc12_test_state state = {
		.function_control = 0,
		.otg_control = 0x07,
		.dcd = true,
		.primary = true,
		.secondary = true,
	};
	struct ci_msm_bc12_io io = {
		.context = &state,
		.read = bc12_test_read,
		.write = bc12_test_write,
		.line_state = bc12_test_line_state,
		.wait_ms = bc12_test_wait,
	};
	enum ci_msm_bc12_type type;

	KUNIT_ASSERT_EQ(test, ci_msm_bc12_detect(&io, &type), 0);
	KUNIT_EXPECT_EQ(test, type, CI_MSM_BC12_DCP);
	KUNIT_EXPECT_TRUE(test, state.function_control &
			  ULPI_FUNC_CTRL_SUSPENDM);
}

static void bc12_test_gadget_stop_keeps_live_source(struct kunit *test)
{
	struct ci_msm_bc12_source_state source = {
		.online = true,
		.usb_type = POWER_SUPPLY_USB_TYPE_DCP,
		.current_max = 1200000,
	};

	KUNIT_EXPECT_FALSE(test, ci_msm_bc12_source_stop(&source, true, true));
	KUNIT_EXPECT_TRUE(test, source.online);
	KUNIT_EXPECT_EQ(test, source.usb_type, POWER_SUPPLY_USB_TYPE_DCP);
	KUNIT_EXPECT_EQ(test, source.current_max, 1200000);
}

static void bc12_test_stop_clears_disconnected_source(struct kunit *test)
{
	struct ci_msm_bc12_source_state source = {
		.online = true,
		.usb_type = POWER_SUPPLY_USB_TYPE_DCP,
		.current_max = 1200000,
	};

	KUNIT_EXPECT_TRUE(test, ci_msm_bc12_source_stop(&source, false, true));
	KUNIT_EXPECT_FALSE(test, source.online);
	KUNIT_EXPECT_EQ(test, source.usb_type, POWER_SUPPLY_USB_TYPE_UNKNOWN);
	KUNIT_EXPECT_EQ(test, source.current_max, 0);

	source.online = true;
	source.usb_type = POWER_SUPPLY_USB_TYPE_CDP;
	source.current_max = 1200000;
	KUNIT_EXPECT_TRUE(test, ci_msm_bc12_source_stop(&source, true, false));
	KUNIT_EXPECT_FALSE(test, source.online);
	KUNIT_EXPECT_EQ(test, source.current_max, 0);
}

static struct kunit_case bc12_test_cases[] = {
	KUNIT_CASE(bc12_test_classification),
	KUNIT_CASE(bc12_test_error_restores_phy),
	KUNIT_CASE(bc12_test_rebind_after_phy_reset),
	KUNIT_CASE(bc12_test_gadget_stop_keeps_live_source),
	KUNIT_CASE(bc12_test_stop_clears_disconnected_source),
	{}
};

static struct kunit_suite bc12_test_suite = {
	.name = "ci-msm-bc12",
	.test_cases = bc12_test_cases,
};

kunit_test_suite(bc12_test_suite);

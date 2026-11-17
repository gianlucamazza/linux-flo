// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>

struct smb347_test_bus {
	u8 regs[SMB347_MAX_REGISTER + 1];
	unsigned int writes[SMB347_MAX_REGISTER + 1];
	bool fail_read;
	unsigned int fail_reg;
	bool fail_write;
	unsigned int fail_write_reg;
};

static int smb347_test_read(void *context, unsigned int reg, unsigned int *val)
{
	struct smb347_test_bus *bus = context;

	if (bus->fail_read && reg == bus->fail_reg)
		return -EIO;
	*val = bus->regs[reg];
	return 0;
}

static int smb347_test_write(void *context, unsigned int reg, unsigned int val)
{
	struct smb347_test_bus *bus = context;

	if (bus->fail_write && reg == bus->fail_write_reg)
		return -EIO;
	bus->regs[reg] = val;
	bus->writes[reg]++;
	return 0;
}

static const struct regmap_bus smb347_test_regmap_bus = {
	.reg_read = smb347_test_read,
	.reg_write = smb347_test_write,
};

static void smb347_test_regmap_exit(void *data)
{
	regmap_exit(data);
}

static struct regmap *
smb347_test_regmap(struct kunit *test, struct smb347_test_bus *bus)
{
	struct device *dev;
	struct regmap *map;
	int ret;

	dev = kunit_device_register(test, "smb347-reset-test");
	if (IS_ERR(dev))
		return ERR_CAST(dev);
	map = regmap_init(dev, &smb347_test_regmap_bus, bus, &smb347_regmap);
	if (IS_ERR(map))
		return map;
	ret = kunit_add_action_or_reset(test, smb347_test_regmap_exit, map);
	if (ret)
		return ERR_PTR(ret);
	return map;
}

static void smb347_config_read_tracks_chip_reset(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct regmap *map;
	unsigned int val;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->regs[CFG_CURRENT_LIMIT] = 0x44;
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_ASSERT_EQ(test, regmap_read(map, CFG_CURRENT_LIMIT, &val), 0);
	KUNIT_ASSERT_EQ(test, val, 0x44U);

	/* Power loss reloads config RAM without informing regmap. */
	bus->regs[CFG_CURRENT_LIMIT] = 0x41;
	KUNIT_ASSERT_EQ(test, regmap_read(map, CFG_CURRENT_LIMIT, &val), 0);
	KUNIT_EXPECT_EQ(test, val, 0x41U);
}

static void smb347_command_update_uses_chip_state(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct regmap *map;
	unsigned int val;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->regs[CMD_A] = 0x40;
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	KUNIT_ASSERT_EQ(test, regmap_read(map, CMD_A, &val), 0);
	KUNIT_ASSERT_EQ(test, val, 0x40U);

	bus->regs[CMD_A] = 0;
	ret = regmap_update_bits(map, CMD_A, CMD_A_ALLOW_WRITE,
				 CMD_A_ALLOW_WRITE);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_A], 0x80);
}

static void smb347_usb_type_reports_only_known_apsd_codes(struct kunit *test)
{
	static const struct {
		u8 status;
		int type;
	} cases[] = {
		{ 0x09, POWER_SUPPLY_USB_TYPE_CDP },
		{ 0x0a, POWER_SUPPLY_USB_TYPE_DCP },
		{ 0x0c, POWER_SUPPLY_USB_TYPE_SDP },
		{ 0x0b, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x0e, POWER_SUPPLY_USB_TYPE_UNKNOWN },
		{ 0x02, POWER_SUPPLY_USB_TYPE_UNKNOWN },
	};
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct power_supply psy = { .desc = &smb347_usb_desc };
	struct smb347_charger smb = { .usb_online = true };
	union power_supply_propval val;
	struct regmap *map;
	int i, ret;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.regmap = map;
	psy.drv_data = &smb;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		bus->regs[0x3e] = cases[i].status;
		ret = smb347_get_property_locked(&psy, POWER_SUPPLY_PROP_USB_TYPE, &val);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, val.intval, cases[i].type);
	}

	smb.usb_online = false;
	bus->regs[0x3e] = 0x0a;
	ret = smb347_get_property_locked(&psy, POWER_SUPPLY_PROP_USB_TYPE, &val);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, val.intval, POWER_SUPPLY_USB_TYPE_UNKNOWN);
}

static void smb347_usb_type_is_only_advertised_for_usb(struct kunit *test)
{
	bool found_usb = false;
	bool found_mains = false;
	int i;

	for (i = 0; i < smb347_usb_desc.num_properties; i++)
		found_usb |= smb347_usb_desc.properties[i] ==
			     POWER_SUPPLY_PROP_USB_TYPE;
	for (i = 0; i < smb347_mains_desc.num_properties; i++)
		found_mains |= smb347_mains_desc.properties[i] ==
			       POWER_SUPPLY_PROP_USB_TYPE;
	KUNIT_EXPECT_TRUE(test, found_usb);
	KUNIT_EXPECT_FALSE(test, found_mains);
}

static void smb347_input_event_controls_charging_without_chip_irq(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = { .use_usb = true,
		.enable_control = SMB3XX_CHG_ENABLE_SW };
	struct device *dev;
	struct regmap *map;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	dev = kunit_device_register(test, "smb347-input-event");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.dev = dev;
	smb.regmap = map;

	/* No IRQSTAT_E interrupt latch: INOK is a separate physical pin. */
	bus->regs[IRQSTAT_E] = 0;
	KUNIT_ASSERT_EQ(test, smb347_handle_input_change(&smb), 1);
	KUNIT_EXPECT_TRUE(test, smb.usb_online);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_A] & CMD_A_CHG_ENABLED,
			CMD_A_CHG_ENABLED);

	bus->regs[IRQSTAT_E] = IRQSTAT_E_USBIN_UV_STAT;
	KUNIT_ASSERT_EQ(test, smb347_handle_input_change(&smb), 1);
	KUNIT_EXPECT_FALSE(test, smb.usb_online);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_A] & CMD_A_CHG_ENABLED, 0);
}

static void smb347_input_event_read_failure_preserves_charging_state(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = { .use_usb = true,
		.enable_control = SMB3XX_CHG_ENABLE_SW };
	struct regmap *map;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.regmap = map;
	bus->fail_read = true;
	bus->fail_reg = IRQSTAT_E;
	KUNIT_EXPECT_EQ(test, smb347_handle_input_change(&smb), -EIO);
	KUNIT_EXPECT_FALSE(test, smb.usb_online);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_A] & CMD_A_CHG_ENABLED, 0);
}

static void smb347_inok_restores_config_without_online_transition(struct kunit *test)
{
	static const struct property_entry properties[] = {
		PROPERTY_ENTRY_BOOL("power-supplies"),
		{ }
	};
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = {
		.id = SMB345,
		.use_usb = true,
		.usb_online = true,
		.irq_unsupported = true,
		.enable_control = SMB3XX_CHG_ENABLE_PIN_ACTIVE_LOW,
		.usb_hc_current_limit = 1500000,
		.soft_cold_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.soft_hot_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.hard_cold_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.hard_hot_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.soft_temp_limit_compensation = SMB3XX_SOFT_TEMP_COMPENSATE_DEFAULT,
	};
	struct device *dev;
	struct regmap *map;
	unsigned int current_writes, pin_writes;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	dev = kunit_device_register(test, "smb347-inok-reset");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	ret = device_create_managed_software_node(dev, properties, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.dev = dev;
	smb.regmap = map;
	mutex_init(&smb.usb_policy_lock);
	bus->regs[CFG_CURRENT_LIMIT] = 0x41;
	bus->regs[CFG_PIN] = 0x70;
	bus->regs[IRQSTAT_E] = 0;

	KUNIT_ASSERT_EQ(test, smb347_recover_after_inok(&smb), 1);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_CURRENT_LIMIT] & CFG_CURRENT_LIMIT_USB_MASK,
			1);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_PIN] & CFG_PIN_EN_CTRL_MASK,
			CFG_PIN_EN_CTRL_ACTIVE_LOW);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_PIN] & CFG_PIN_USB_MODE_CTRL, 0);
	current_writes = bus->writes[CFG_CURRENT_LIMIT];
	pin_writes = bus->writes[CFG_PIN];

	KUNIT_ASSERT_EQ(test, smb347_recover_after_inok(&smb), 1);
	KUNIT_EXPECT_EQ(test, bus->writes[CFG_CURRENT_LIMIT], current_writes);
	KUNIT_EXPECT_EQ(test, bus->writes[CFG_PIN], pin_writes);
}

static void smb347_inok_does_not_program_disconnected_input(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = {
		.id = SMB345,
		.use_usb = true,
		.usb_online = true,
		.irq_unsupported = true,
		.enable_control = SMB3XX_CHG_ENABLE_PIN_ACTIVE_LOW,
		.usb_hc_current_limit = 1500000,
	};
	struct device *dev;
	struct regmap *map;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	dev = kunit_device_register(test, "smb347-inok-offline");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.dev = dev;
	smb.regmap = map;
	bus->regs[CFG_CURRENT_LIMIT] = 0x41;
	bus->regs[IRQSTAT_E] = IRQSTAT_E_USBIN_UV_STAT;

	KUNIT_ASSERT_EQ(test, smb347_recover_after_inok(&smb), 1);
	KUNIT_EXPECT_FALSE(test, smb.usb_online);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_CURRENT_LIMIT], 0x41);
	KUNIT_EXPECT_EQ(test, bus->writes[CFG_CURRENT_LIMIT], 0U);
}

static void smb347_inok_reports_connection_when_restore_write_fails(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = {
		.id = SMB345,
		.use_usb = true,
		.irq_unsupported = true,
		.enable_control = SMB3XX_CHG_ENABLE_PIN_ACTIVE_LOW,
		.usb_hc_current_limit = 1500000,
	};
	struct device *dev;
	struct regmap *map;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	dev = kunit_device_register(test, "smb347-inok-write-error");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.dev = dev;
	smb.regmap = map;
	bus->regs[CFG_CURRENT_LIMIT] = 0x41;
	bus->regs[IRQSTAT_E] = 0;
	bus->fail_write = true;
	bus->fail_write_reg = CFG_CURRENT_LIMIT;

	KUNIT_EXPECT_EQ(test, smb347_recover_after_inok(&smb), 1);
	KUNIT_EXPECT_TRUE(test, smb.usb_online);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_CURRENT_LIMIT], 0x41);
}

static void smb345_flo_usb_input_current_indices(struct kunit *test)
{
	const unsigned int *table = icl_tbl[SMB345];
	size_t count = ARRAY_SIZE(icl_tbl[SMB345]);

	KUNIT_EXPECT_EQ(test, table[3], 900000U);
	KUNIT_EXPECT_EQ(test, table[4], 1200000U);
	KUNIT_EXPECT_EQ(test, current_to_hw(table, count, 1200000), 4);
	KUNIT_EXPECT_EQ(test, current_to_hw(table, count, 1199999), 3);
}

static void smb345_usb_source_policy_limits(struct kunit *test)
{
	struct smb347_usb_policy policy;

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_UNKNOWN,
					  100000, 1200000);
	KUNIT_EXPECT_EQ(test, policy.mode, 0);
	KUNIT_EXPECT_EQ(test, policy.icl_ua, 500000U);

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_UNKNOWN,
					  1200000, 1200000);
	KUNIT_EXPECT_EQ(test, policy.mode, CMD_B_USB_HC_MODE);
	KUNIT_EXPECT_EQ(test, policy.icl_ua, 1200000U);

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_SDP,
					  500000, 1200000);
	KUNIT_EXPECT_EQ(test, policy.mode, CMD_B_USB_500_MODE);
	KUNIT_EXPECT_EQ(test, policy.icl_ua, 500000U);

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_SDP,
					  250000, 1200000);
	KUNIT_EXPECT_EQ(test, policy.mode, 0);

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_DCP, 1200000,
					  1200000);
	KUNIT_EXPECT_EQ(test, policy.mode, CMD_B_USB_HC_MODE);
	KUNIT_EXPECT_EQ(test, policy.icl_ua, 1200000U);

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_CDP, 1200000,
					  900000);
	KUNIT_EXPECT_EQ(test, policy.mode, CMD_B_USB_HC_MODE);
	KUNIT_EXPECT_EQ(test, policy.icl_ua, 900000U);
}

static void smb345_usb_source_policy_programs_chip(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = {
		.id = SMB345,
		.usb_online = true,
		.irq_unsupported = true,
	};
	struct smb347_usb_policy policy;
	struct regmap *map;
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	dev = kunit_device_register(test, "smb345-source-policy");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.dev = dev;
	smb.regmap = map;
	bus->regs[CFG_AICL] = CFG_AICL_ENABLED;
	bus->regs[CFG_CURRENT_LIMIT] = 0x41;

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_DCP, 1200000,
					  1200000);
	KUNIT_ASSERT_EQ(test, smb347_apply_usb_policy_locked(&smb, policy), 0);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_B] & 0x03, CMD_B_USB_HC_MODE);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_CURRENT_LIMIT] & 0x0f, 4);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_AICL] & CFG_AICL_ENABLED,
			CFG_AICL_ENABLED);
	KUNIT_EXPECT_EQ(test, smb.usb_current_max, 1200000U);

	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_SDP, 500000,
					  1200000);
	KUNIT_ASSERT_EQ(test, smb347_apply_usb_policy_locked(&smb, policy), 0);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_B] & 0x03, CMD_B_USB_500_MODE);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_CURRENT_LIMIT] & 0x0f, 1);
	KUNIT_EXPECT_EQ(test, smb.usb_current_max, 500000U);

	bus->fail_write = true;
	bus->fail_write_reg = CFG_CURRENT_LIMIT;
	policy = smb347_usb_source_policy(POWER_SUPPLY_USB_TYPE_DCP, 1200000,
					  1200000);
	KUNIT_EXPECT_EQ(test, smb347_apply_usb_policy_locked(&smb, policy), -EIO);
	KUNIT_EXPECT_EQ(test, bus->regs[CMD_B] & 0x03, 0);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_AICL] & CFG_AICL_ENABLED,
			CFG_AICL_ENABLED);
}

static void smb347_without_usb_supplier_preserves_pin_mode(struct kunit *test)
{
	struct smb347_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct smb347_charger smb = {
		.id = SMB347,
		.use_usb = true,
		.irq_unsupported = true,
		.enable_control = SMB3XX_CHG_ENABLE_PIN_ACTIVE_LOW,
		.soft_cold_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.soft_hot_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.hard_cold_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.hard_hot_temp_limit = SMB3XX_TEMP_USE_DEFAULT,
		.soft_temp_limit_compensation = SMB3XX_SOFT_TEMP_COMPENSATE_DEFAULT,
	};
	struct regmap *map;
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	dev = kunit_device_register(test, "smb347-no-source");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	map = smb347_test_regmap(test, bus);
	KUNIT_ASSERT_FALSE(test, IS_ERR(map));
	smb.dev = dev;
	smb.regmap = map;
	bus->regs[CFG_PIN] = CFG_PIN_USB_MODE_CTRL;
	bus->regs[IRQSTAT_E] = IRQSTAT_E_USBIN_UV_STAT;

	KUNIT_ASSERT_EQ(test, smb347_hw_init(&smb), 0);
	KUNIT_EXPECT_EQ(test, bus->regs[CFG_PIN] & CFG_PIN_USB_MODE_CTRL,
			CFG_PIN_USB_MODE_CTRL);
	KUNIT_EXPECT_EQ(test, bus->writes[CMD_B], 0U);
}

static struct kunit_case smb347_reset_cases[] = {
	KUNIT_CASE(smb345_flo_usb_input_current_indices),
	KUNIT_CASE(smb345_usb_source_policy_limits),
	KUNIT_CASE(smb345_usb_source_policy_programs_chip),
	KUNIT_CASE(smb347_without_usb_supplier_preserves_pin_mode),
	KUNIT_CASE(smb347_config_read_tracks_chip_reset),
	KUNIT_CASE(smb347_command_update_uses_chip_state),
	KUNIT_CASE(smb347_usb_type_reports_only_known_apsd_codes),
	KUNIT_CASE(smb347_usb_type_is_only_advertised_for_usb),
	KUNIT_CASE(smb347_input_event_controls_charging_without_chip_irq),
	KUNIT_CASE(smb347_input_event_read_failure_preserves_charging_state),
	KUNIT_CASE(smb347_inok_restores_config_without_online_transition),
	KUNIT_CASE(smb347_inok_does_not_program_disconnected_input),
	KUNIT_CASE(smb347_inok_reports_connection_when_restore_write_fails),
	{}
};

static struct kunit_suite smb347_reset_suite = {
	.name = "smb347-register-reset",
	.test_cases = smb347_reset_cases,
};

kunit_test_suite(smb347_reset_suite);

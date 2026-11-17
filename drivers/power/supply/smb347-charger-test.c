// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>

struct smb347_test_bus {
	u8 regs[SMB347_MAX_REGISTER + 1];
};

static int smb347_test_read(void *context, unsigned int reg, unsigned int *val)
{
	struct smb347_test_bus *bus = context;

	*val = bus->regs[reg];
	return 0;
}

static int smb347_test_write(void *context, unsigned int reg, unsigned int val)
{
	struct smb347_test_bus *bus = context;

	bus->regs[reg] = val;
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

static struct kunit_case smb347_reset_cases[] = {
	KUNIT_CASE(smb347_config_read_tracks_chip_reset),
	KUNIT_CASE(smb347_command_update_uses_chip_state),
	KUNIT_CASE(smb347_usb_type_reports_only_known_apsd_codes),
	KUNIT_CASE(smb347_usb_type_is_only_advertised_for_usb),
	{}
};

static struct kunit_suite smb347_reset_suite = {
	.name = "smb347-register-reset",
	.test_cases = smb347_reset_cases,
};

kunit_test_suite(smb347_reset_suite);

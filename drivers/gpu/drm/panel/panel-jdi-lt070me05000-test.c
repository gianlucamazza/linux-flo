// SPDX-License-Identifier: GPL-2.0-only
/* Exercise panel callbacks through the real MIPI DSI helpers. */
#include <kunit/test.h>

struct jdi_test_host {
	struct mipi_dsi_host host;
	u8 brightness_payload[4];
	u8 brightness_type;
	size_t brightness_len;
	size_t read_len;
	int error;
};

static ssize_t jdi_test_transfer(struct mipi_dsi_host *host,
				 const struct mipi_dsi_msg *msg)
{
	struct jdi_test_host *fake = container_of(host, struct jdi_test_host, host);

	if (fake->error)
		return fake->error;
	if (msg->rx_len) {
		fake->read_len = msg->rx_len;
		((u8 *)msg->rx_buf)[0] = 77;
		return 1;
	}
	if (msg->tx_len && ((u8 *)msg->tx_buf)[0] == MIPI_DCS_SET_DISPLAY_BRIGHTNESS) {
		fake->brightness_type = msg->type;
		fake->brightness_len = msg->tx_len;
		memcpy(fake->brightness_payload, msg->tx_buf,
		       min(msg->tx_len, sizeof(fake->brightness_payload)));
	}
	return msg->tx_len;
}

static const struct mipi_dsi_host_ops jdi_test_ops = {
	.transfer = jdi_test_transfer,
};

static void jdi_test_setup(struct jdi_test_host *host,
			   struct mipi_dsi_device *dsi,
			   struct backlight_device *backlight)
{
	memset(host, 0, sizeof(*host));
	memset(dsi, 0, sizeof(*dsi));
	memset(backlight, 0, sizeof(*backlight));
	host->host.ops = &jdi_test_ops;
	dsi->host = &host->host;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;
	backlight->props.brightness = 127;
	dev_set_drvdata(&backlight->dev, dsi);
}

static void jdi_test_brightness_short_write(struct kunit *test)
{
	struct jdi_test_host host;
	struct mipi_dsi_device dsi;
	struct backlight_device backlight;

	jdi_test_setup(&host, &dsi, &backlight);
	KUNIT_ASSERT_EQ(test, dsi_dcs_bl_update_status(&backlight), 0);
	KUNIT_EXPECT_EQ(test, host.brightness_type, MIPI_DSI_DCS_SHORT_WRITE_PARAM);
	KUNIT_EXPECT_EQ(test, host.brightness_len, (size_t)2);
	KUNIT_EXPECT_EQ(test, host.brightness_payload[0], (u8)0x51);
	KUNIT_EXPECT_EQ(test, host.brightness_payload[1], (u8)127);
}

static void jdi_test_brightness_single_byte_read(struct kunit *test)
{
	struct jdi_test_host host;
	struct mipi_dsi_device dsi;
	struct backlight_device backlight;

	jdi_test_setup(&host, &dsi, &backlight);
	KUNIT_EXPECT_EQ(test, dsi_dcs_bl_get_brightness(&backlight), 77);
	KUNIT_EXPECT_EQ(test, host.read_len, (size_t)1);
}

static void jdi_test_restore_flags_after_error(struct kunit *test)
{
	struct jdi_test_host host;
	struct mipi_dsi_device dsi;
	struct backlight_device backlight;
	unsigned long flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	jdi_test_setup(&host, &dsi, &backlight);
	host.error = -EIO;
	KUNIT_EXPECT_EQ(test, dsi_dcs_bl_update_status(&backlight), -EIO);
	KUNIT_EXPECT_EQ(test, dsi.mode_flags, flags);
	dsi.mode_flags = flags;
	KUNIT_EXPECT_EQ(test, dsi_dcs_bl_get_brightness(&backlight), -EIO);
	KUNIT_EXPECT_EQ(test, dsi.mode_flags, flags);
}

static void jdi_test_initial_brightness_short_write(struct kunit *test)
{
	struct jdi_test_host host;
	struct mipi_dsi_device dsi;
	struct backlight_device backlight;
	struct jdi_panel panel = { .dsi = &dsi, .backlight = &backlight,
				   .mode = &default_mode };

	jdi_test_setup(&host, &dsi, &backlight);
	KUNIT_ASSERT_EQ(test, jdi_panel_init(&panel), 0);
	KUNIT_EXPECT_EQ(test, host.brightness_type, MIPI_DSI_DCS_SHORT_WRITE_PARAM);
	KUNIT_EXPECT_EQ(test, host.brightness_len, (size_t)2);
}

static struct kunit_case jdi_cases[] = {
	KUNIT_CASE(jdi_test_brightness_short_write),
	KUNIT_CASE(jdi_test_brightness_single_byte_read),
	KUNIT_CASE(jdi_test_restore_flags_after_error),
	KUNIT_CASE(jdi_test_initial_brightness_short_write),
	{}
};

static struct kunit_suite jdi_suite = {
	.name = "jdi-lt070me05000-backlight",
	.test_cases = jdi_cases,
};
kunit_test_suite(jdi_suite);

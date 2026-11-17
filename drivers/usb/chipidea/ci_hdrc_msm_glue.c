// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2010, Code Aurora Forum. All rights reserved. */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/usb/chipidea.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/reset-controller.h>
#include <linux/extcon.h>
#include <linux/of.h>
#include <linux/delay.h>
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
#include <linux/power_supply.h>
#include <linux/ulpi/driver.h>
#endif

#include "ci.h"
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
#include "ci_hdrc_msm_bc12.h"
#endif

#define HS_PHY_AHB_MODE			0x0098

#define HS_PHY_GENCONFIG		0x009c
#define HS_PHY_TXFIFO_IDLE_FORCE_DIS	BIT(4)

#define HS_PHY_GENCONFIG_2		0x00a0
#define HS_PHY_SESS_VLD_CTRL_EN		BIT(7)
#define HS_PHY_ULPI_TX_PKT_EN_CLR_FIX	BIT(19)

#define HSPHY_SESS_VLD_CTRL		BIT(25)

/* Vendor base starts at 0x200 beyond CI base */
#define HS_PHY_CTRL			0x0040
#define HS_PHY_SEC_CTRL			0x0078
#define HS_PHY_DIG_CLAMP_N		BIT(16)
#define HS_PHY_POR_ASSERT		BIT(0)

struct ci_hdrc_msm {
	struct platform_device *ci;
	struct clk *core_clk;
	struct clk *iface_clk;
	struct clk *fs_clk;
	struct ci_hdrc_platform_data pdata;
	struct reset_controller_dev rcdev;
	bool secondary_phy;
	bool hsic;
	bool gadget_phy_on;
	void __iomem *base;
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
	struct power_supply *source_psy;
	struct power_supply_desc source_desc;
	/* Protect the source properties read from power_supply callbacks. */
	spinlock_t source_lock;
	bool source_online;
	enum power_supply_usb_type source_type;
	int source_current_max;
#endif
};

#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
static enum power_supply_property ci_msm_source_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int ci_msm_source_get_property(struct power_supply *psy,
				      enum power_supply_property property,
				      union power_supply_propval *value)
{
	struct ci_hdrc_msm *msm_ci = power_supply_get_drvdata(psy);
	unsigned long flags;

	spin_lock_irqsave(&msm_ci->source_lock, flags);
	switch (property) {
	case POWER_SUPPLY_PROP_ONLINE:
		value->intval = msm_ci->source_online;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		value->intval = msm_ci->source_type;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		value->intval = msm_ci->source_current_max;
		break;
	default:
		spin_unlock_irqrestore(&msm_ci->source_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&msm_ci->source_lock, flags);
	return 0;
}

static void ci_msm_source_update(struct ci_hdrc_msm *msm_ci, bool online,
				 enum ci_msm_bc12_type type)
{
	unsigned long flags;

	if (!msm_ci->source_psy)
		return;

	spin_lock_irqsave(&msm_ci->source_lock, flags);
	msm_ci->source_online = online;
	msm_ci->source_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	msm_ci->source_current_max = online ? 100000 : 0;
	if (online) {
		switch (type) {
		case CI_MSM_BC12_SDP:
			msm_ci->source_type = POWER_SUPPLY_USB_TYPE_SDP;
			break;
		case CI_MSM_BC12_CDP:
			msm_ci->source_type = POWER_SUPPLY_USB_TYPE_CDP;
			msm_ci->source_current_max = 1200000;
			break;
		case CI_MSM_BC12_DCP:
			msm_ci->source_type = POWER_SUPPLY_USB_TYPE_DCP;
			msm_ci->source_current_max = 1200000;
			break;
		case CI_MSM_BC12_PROPRIETARY:
			msm_ci->source_current_max = 1200000;
			break;
		default:
			break;
		}
	}
	spin_unlock_irqrestore(&msm_ci->source_lock, flags);
	power_supply_changed(msm_ci->source_psy);
}

static int ci_msm_source_set_vbus_draw(struct ci_hdrc *ci, unsigned int milliamp)
{
	struct ci_hdrc_msm *msm_ci = dev_get_drvdata(ci->dev->parent);
	unsigned long flags;
	int current_max;

	if (!msm_ci->source_psy)
		return -EOPNOTSUPP;

	current_max = min(milliamp, 500U) * 1000;
	spin_lock_irqsave(&msm_ci->source_lock, flags);
	if (!msm_ci->source_online ||
	    msm_ci->source_type != POWER_SUPPLY_USB_TYPE_SDP) {
		spin_unlock_irqrestore(&msm_ci->source_lock, flags);
		return 0;
	}
	msm_ci->source_current_max = current_max;
	spin_unlock_irqrestore(&msm_ci->source_lock, flags);
	power_supply_changed(msm_ci->source_psy);
	return 0;
}

static int ci_msm_bc12_ulpi_read(void *context, u8 address)
{
	struct ci_hdrc *ci = context;

	return ulpi_read(ci->ulpi, address);
}

static int ci_msm_bc12_ulpi_write(void *context, u8 address, u8 value)
{
	struct ci_hdrc *ci = context;

	return ulpi_write(ci->ulpi, address, value);
}

static int ci_msm_bc12_line_state(void *context)
{
	struct ci_hdrc *ci = context;

	return hw_read(ci, OP_PORTSC, GENMASK(11, 10));
}

static void ci_msm_bc12_wait(void *context, unsigned int milliseconds)
{
	msleep(milliseconds);
}

static void ci_msm_source_detect(struct ci_hdrc_msm *msm_ci,
				 struct ci_hdrc *ci)
{
	const struct ci_msm_bc12_io io = {
		.context = ci,
		.read = ci_msm_bc12_ulpi_read,
		.write = ci_msm_bc12_ulpi_write,
		.line_state = ci_msm_bc12_line_state,
		.wait_ms = ci_msm_bc12_wait,
	};
	enum ci_msm_bc12_type type = CI_MSM_BC12_UNKNOWN;
	int ret;

	if (!msm_ci->source_psy || !ci->ulpi ||
	    ci->role != CI_ROLE_GADGET || !ci->vbus_active)
		return;

	ret = ci_msm_bc12_detect(&io, &type);
	if (ret)
		dev_warn(ci->dev, "USB source detection failed: %d\n", ret);
	ci_msm_source_update(msm_ci, true, type);
}

static int ci_msm_source_register(struct platform_device *pdev,
				  struct ci_hdrc_msm *msm_ci,
				  struct device_node *phy_node)
{
	struct power_supply_config config = {
		.drv_data = msm_ci,
		.fwnode = dev_fwnode(&pdev->dev),
	};
	struct power_supply *psy;
	const char *name;

	if (!of_device_is_compatible(phy_node, "qcom,usb-hs-phy-apq8064"))
		return 0;

	spin_lock_init(&msm_ci->source_lock);
	msm_ci->source_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-source", dev_name(&pdev->dev));
	if (!name)
		return -ENOMEM;
	msm_ci->source_desc.name = name;
	msm_ci->source_desc.type = POWER_SUPPLY_TYPE_USB;
	msm_ci->source_desc.properties = ci_msm_source_properties;
	msm_ci->source_desc.num_properties = ARRAY_SIZE(ci_msm_source_properties);
	msm_ci->source_desc.get_property = ci_msm_source_get_property;
	msm_ci->source_desc.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		BIT(POWER_SUPPLY_USB_TYPE_DCP);

	psy = devm_power_supply_register(&pdev->dev, &msm_ci->source_desc,
					 &config);
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	msm_ci->source_psy = psy;
	return 0;
}
#endif

static int
ci_hdrc_msm_por_reset(struct reset_controller_dev *r, unsigned long id)
{
	struct ci_hdrc_msm *ci_msm = container_of(r, struct ci_hdrc_msm, rcdev);
	void __iomem *addr = ci_msm->base;
	u32 val;

	if (id)
		addr += HS_PHY_SEC_CTRL;
	else
		addr += HS_PHY_CTRL;

	val = readl_relaxed(addr);
	val |= HS_PHY_POR_ASSERT;
	writel(val, addr);
	/*
	 * wait for minimum 10 microseconds as suggested by manual.
	 * Use a slightly larger value since the exact value didn't
	 * work 100% of the time.
	 */
	udelay(12);
	val &= ~HS_PHY_POR_ASSERT;
	writel(val, addr);

	return 0;
}

static const struct reset_control_ops ci_hdrc_msm_reset_ops = {
	.reset = ci_hdrc_msm_por_reset,
};

static int ci_hdrc_msm_notify_event(struct ci_hdrc *ci, unsigned event)
{
	struct device *dev = ci->dev->parent;
	struct ci_hdrc_msm *msm_ci = dev_get_drvdata(dev);
	int ret;

	switch (event) {
	case CI_HDRC_CONTROLLER_RESET_EVENT:
		dev_dbg(dev, "CI_HDRC_CONTROLLER_RESET_EVENT received\n");

		hw_phymode_configure(ci);
		if (msm_ci->secondary_phy) {
			u32 val = readl_relaxed(msm_ci->base + HS_PHY_SEC_CTRL);
			val |= HS_PHY_DIG_CLAMP_N;
			writel_relaxed(val, msm_ci->base + HS_PHY_SEC_CTRL);
		}

		ret = phy_init(ci->phy);
		if (ret)
			return ret;

		ret = phy_power_on(ci->phy);
		if (ret) {
			phy_exit(ci->phy);
			return ret;
		}
		msm_ci->gadget_phy_on = true;

		/* use AHB transactor, allow posted data writes */
		hw_write_id_reg(ci, HS_PHY_AHB_MODE, 0xffffffff, 0x8);

		/* workaround for rx buffer collision issue */
		hw_write_id_reg(ci, HS_PHY_GENCONFIG,
				HS_PHY_TXFIFO_IDLE_FORCE_DIS, 0);

		if (!msm_ci->hsic)
			hw_write_id_reg(ci, HS_PHY_GENCONFIG_2,
					HS_PHY_ULPI_TX_PKT_EN_CLR_FIX, 0);

		if (!IS_ERR(ci->platdata->vbus_extcon.edev) || ci->role_switch) {
			hw_write_id_reg(ci, HS_PHY_GENCONFIG_2,
					HS_PHY_SESS_VLD_CTRL_EN,
					HS_PHY_SESS_VLD_CTRL_EN);
			hw_write(ci, OP_USBCMD, HSPHY_SESS_VLD_CTRL,
				 HSPHY_SESS_VLD_CTRL);

		}
		/* USB PHY is now powered; the gadget has not joined the bus yet. */
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
		ci_msm_source_detect(msm_ci, ci);
#endif
		break;
	case CI_HDRC_CONTROLLER_VBUS_EVENT:
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
		ci_msm_source_update(msm_ci, ci->vbus_active,
				     CI_MSM_BC12_UNKNOWN);
#endif
		break;
	case CI_HDRC_CONTROLLER_STOPPED_EVENT:
		dev_dbg(dev, "CI_HDRC_CONTROLLER_STOPPED_EVENT received\n");
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
		ci_msm_source_update(msm_ci, ci->vbus_active &&
				     ci->role == CI_ROLE_GADGET,
				     CI_MSM_BC12_UNKNOWN);
#endif
		if (msm_ci->gadget_phy_on) {
			msm_ci->gadget_phy_on = false;
			phy_power_off(ci->phy);
			phy_exit(ci->phy);
		}
		break;
	default:
		dev_dbg(dev, "unknown ci_hdrc event\n");
		break;
	}

	return 0;
}

static int ci_hdrc_msm_mux_phy(struct ci_hdrc_msm *ci,
			       struct platform_device *pdev)
{
	struct regmap *regmap;
	struct device *dev = &pdev->dev;
	struct of_phandle_args args;
	u32 val;
	int ret;

	ret = of_parse_phandle_with_fixed_args(dev->of_node, "phy-select", 2, 0,
					       &args);
	if (ret)
		return 0;

	regmap = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = regmap_write(regmap, args.args[0], args.args[1]);
	if (ret)
		return ret;

	ci->secondary_phy = !!args.args[1];
	if (ci->secondary_phy) {
		val = readl_relaxed(ci->base + HS_PHY_SEC_CTRL);
		val |= HS_PHY_DIG_CLAMP_N;
		writel_relaxed(val, ci->base + HS_PHY_SEC_CTRL);
	}

	return 0;
}

static int ci_hdrc_msm_probe(struct platform_device *pdev)
{
	struct ci_hdrc_msm *ci;
	struct platform_device *plat_ci;
	struct clk *clk;
	struct reset_control *reset;
	int ret;
	struct device_node *ulpi_node, *phy_node;

	dev_dbg(&pdev->dev, "ci_hdrc_msm_probe\n");

	ci = devm_kzalloc(&pdev->dev, sizeof(*ci), GFP_KERNEL);
	if (!ci)
		return -ENOMEM;
	platform_set_drvdata(pdev, ci);

	ci->pdata.name = "ci_hdrc_msm";
	ci->pdata.capoffset = DEF_CAPOFFSET;
	ci->pdata.flags	= CI_HDRC_REGS_SHARED | CI_HDRC_DISABLE_STREAMING |
			  CI_HDRC_OVERRIDE_AHB_BURST |
			  CI_HDRC_OVERRIDE_PHY_CONTROL;
	ci->pdata.notify_event = ci_hdrc_msm_notify_event;
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
	ci->pdata.set_vbus_draw = ci_msm_source_set_vbus_draw;
#endif

	reset = devm_reset_control_get(&pdev->dev, "core");
	if (IS_ERR(reset))
		return PTR_ERR(reset);

	ci->core_clk = clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ci->iface_clk = clk = devm_clk_get(&pdev->dev, "iface");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ci->fs_clk = clk = devm_clk_get_optional(&pdev->dev, "fs");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ci->base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ci->base))
		return PTR_ERR(ci->base);

	ci->rcdev.owner = THIS_MODULE;
	ci->rcdev.ops = &ci_hdrc_msm_reset_ops;
	ci->rcdev.of_node = pdev->dev.of_node;
	ci->rcdev.nr_resets = 2;
	ret = devm_reset_controller_register(&pdev->dev, &ci->rcdev);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ci->fs_clk);
	if (ret)
		return ret;

	reset_control_assert(reset);
	usleep_range(10000, 12000);
	reset_control_deassert(reset);

	clk_disable_unprepare(ci->fs_clk);

	ret = clk_prepare_enable(ci->core_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ci->iface_clk);
	if (ret)
		goto err_iface;

	ret = ci_hdrc_msm_mux_phy(ci, pdev);
	if (ret)
		goto err_mux;

	ulpi_node = of_get_child_by_name(pdev->dev.of_node, "ulpi");
	if (ulpi_node) {
		phy_node = of_get_next_available_child(ulpi_node, NULL);
		ci->hsic = of_device_is_compatible(phy_node, "qcom,usb-hsic-phy");
#if IS_ENABLED(CONFIG_USB_CHIPIDEA_MSM_BC12)
		ret = ci_msm_source_register(pdev, ci, phy_node);
		if (ret) {
			of_node_put(phy_node);
			of_node_put(ulpi_node);
			goto err_mux;
		}
#endif
		of_node_put(phy_node);
	}
	of_node_put(ulpi_node);

	plat_ci = ci_hdrc_add_device(&pdev->dev, pdev->resource,
				     pdev->num_resources, &ci->pdata);
	if (IS_ERR(plat_ci)) {
		ret = PTR_ERR(plat_ci);
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "ci_hdrc_add_device failed!\n");
		goto err_mux;
	}

	ci->ci = plat_ci;

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_no_callbacks(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	return 0;

err_mux:
	clk_disable_unprepare(ci->iface_clk);
err_iface:
	clk_disable_unprepare(ci->core_clk);
	return ret;
}

static void ci_hdrc_msm_remove(struct platform_device *pdev)
{
	struct ci_hdrc_msm *ci = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	ci_hdrc_remove_device(ci->ci);
	clk_disable_unprepare(ci->iface_clk);
	clk_disable_unprepare(ci->core_clk);
}

static const struct of_device_id msm_ci_dt_match[] = {
	{ .compatible = "qcom,ci-hdrc", },
	{ }
};
MODULE_DEVICE_TABLE(of, msm_ci_dt_match);

static struct platform_driver ci_hdrc_msm_driver = {
	.probe = ci_hdrc_msm_probe,
	.remove = ci_hdrc_msm_remove,
	.driver = {
		.name = "msm_hsusb",
		.of_match_table = msm_ci_dt_match,
	},
};

module_platform_driver(ci_hdrc_msm_driver);

MODULE_ALIAS("platform:msm_hsusb");
MODULE_ALIAS("platform:ci13xxx_msm");
MODULE_DESCRIPTION("ChipIdea Highspeed Dual Role Controller");
MODULE_LICENSE("GPL v2");

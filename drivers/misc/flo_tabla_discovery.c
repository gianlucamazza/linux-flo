// SPDX-License-Identifier: GPL-2.0-only
/* Flo tabla enumeration aid. No sound card or amplifier is registered here. */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>

#include "flo_tabla_discovery.h"

#define FLO_TABLA_MCLK_HZ 12288000

struct flo_tabla_discovery {
	struct device *controller;
	struct regulator *s4;
	struct regulator *l25;
	struct gpio_desc *reset;
	struct clk *mclk;
	struct mutex lock;
	struct flo_tabla_power power;
};

static int flo_tabla_apply(void *data, enum flo_tabla_action action)
{
	struct flo_tabla_discovery *td = data;

	switch (action) {
	case FLO_TABLA_CONTROLLER_GET:
		return pm_runtime_resume_and_get(td->controller);
	case FLO_TABLA_S4_ENABLE:
		return regulator_enable(td->s4);
	case FLO_TABLA_L25_ENABLE:
		return regulator_enable(td->l25);
	case FLO_TABLA_RESET_ASSERT:
		return gpiod_set_value_cansleep(td->reset, 1);
	case FLO_TABLA_RESET_DEASSERT:
		return gpiod_set_value_cansleep(td->reset, 0);
	case FLO_TABLA_WAIT_RESET:
	case FLO_TABLA_WAIT_BOOT:
		msleep(20);
		return 0;
	case FLO_TABLA_MCLK_RATE:
		return clk_set_rate(td->mclk, FLO_TABLA_MCLK_HZ);
	case FLO_TABLA_MCLK_ENABLE:
		return clk_prepare_enable(td->mclk);
	case FLO_TABLA_MCLK_DISABLE:
		clk_disable_unprepare(td->mclk);
		return 0;
	case FLO_TABLA_L25_DISABLE:
		return regulator_disable(td->l25);
	case FLO_TABLA_S4_DISABLE:
		return regulator_disable(td->s4);
	case FLO_TABLA_CONTROLLER_PUT:
		pm_runtime_mark_last_busy(td->controller);
		return pm_runtime_put_autosuspend(td->controller);
	}
	return -EINVAL;
}

static int flo_tabla_power_off(struct flo_tabla_power *p)
{
	int ret;

	if (!p->controller_on && !p->s4_on && !p->l25_on && !p->mclk_on &&
	    p->reset_asserted)
		return 0;

	p->enabled = false;
	if (!p->reset_asserted) {
		ret = p->apply(p->ctx, FLO_TABLA_RESET_ASSERT);
		if (ret)
			return ret;
		p->reset_asserted = true;
	}
	if (p->mclk_on) {
		ret = p->apply(p->ctx, FLO_TABLA_MCLK_DISABLE);
		if (ret)
			return ret;
		p->mclk_on = false;
	}
	if (p->l25_on) {
		ret = p->apply(p->ctx, FLO_TABLA_L25_DISABLE);
		if (ret)
			return ret;
		p->l25_on = false;
	}
	if (p->s4_on) {
		ret = p->apply(p->ctx, FLO_TABLA_S4_DISABLE);
		if (ret)
			return ret;
		p->s4_on = false;
	}
	if (p->controller_on) {
		ret = p->apply(p->ctx, FLO_TABLA_CONTROLLER_PUT);
		/* The reference is dropped even if autosuspend scheduling fails. */
		p->controller_on = false;
		return ret;
	}
	return 0;
}

int flo_tabla_power_set(struct flo_tabla_power *p, bool enable)
{
	int ret, cleanup_ret;

	if (!enable)
		return flo_tabla_power_off(p);
	if (p->enabled)
		return 0;
	if (p->controller_on || p->s4_on || p->l25_on || p->mclk_on)
		return -EBUSY;

	ret = p->apply(p->ctx, FLO_TABLA_CONTROLLER_GET);
	if (ret)
		return ret;
	p->controller_on = true;
	ret = p->apply(p->ctx, FLO_TABLA_S4_ENABLE);
	if (ret)
		goto unwind;
	p->s4_on = true;
	ret = p->apply(p->ctx, FLO_TABLA_L25_ENABLE);
	if (ret)
		goto unwind;
	p->l25_on = true;
	ret = p->apply(p->ctx, FLO_TABLA_RESET_ASSERT);
	if (ret)
		goto unwind;
	p->reset_asserted = true;
	ret = p->apply(p->ctx, FLO_TABLA_WAIT_RESET);
	if (ret)
		goto unwind;
	ret = p->apply(p->ctx, FLO_TABLA_RESET_DEASSERT);
	if (ret)
		goto unwind;
	p->reset_asserted = false;
	ret = p->apply(p->ctx, FLO_TABLA_WAIT_BOOT);
	if (ret)
		goto unwind;
	ret = p->apply(p->ctx, FLO_TABLA_MCLK_RATE);
	if (ret)
		goto unwind;
	ret = p->apply(p->ctx, FLO_TABLA_MCLK_ENABLE);
	if (ret)
		goto unwind;
	p->mclk_on = true;
	p->enabled = true;
	return 0;

unwind:
	cleanup_ret = flo_tabla_power_off(p);
	if (cleanup_ret)
		return cleanup_ret;
	return ret;
}
EXPORT_SYMBOL_GPL(flo_tabla_power_set);

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct flo_tabla_discovery *td = dev_get_drvdata(dev);
	bool enabled;

	mutex_lock(&td->lock);
	enabled = td->power.enabled;
	mutex_unlock(&td->lock);
	return sysfs_emit(buf, "%u\n", enabled);
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct flo_tabla_discovery *td = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	mutex_lock(&td->lock);
	ret = flo_tabla_power_set(&td->power, enable);
	mutex_unlock(&td->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enabled);

static int flo_tabla_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct platform_device *controller;
	struct flo_tabla_discovery *td;
	int ret;

	node = of_parse_phandle(dev->of_node, "slim-controller", 0);
	if (!node)
		return dev_err_probe(dev, -EINVAL, "missing SLIMbus controller\n");
	controller = of_find_device_by_node(node);
	of_node_put(node);
	if (!controller)
		return -EPROBE_DEFER;
	if (!device_link_add(dev, &controller->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = -ENOMEM;
		goto put_controller;
	}
	if (!device_is_bound(&controller->dev)) {
		ret = -EPROBE_DEFER;
		goto put_controller;
	}

	td = devm_kzalloc(dev, sizeof(*td), GFP_KERNEL);
	if (!td) {
		ret = -ENOMEM;
		goto put_controller;
	}
	td->controller = &controller->dev;
	td->s4 = devm_regulator_get(dev, "vdd-1p8");
	if (IS_ERR(td->s4)) {
		ret = dev_err_probe(dev, PTR_ERR(td->s4), "S4 supply\n");
		goto put_controller;
	}
	td->l25 = devm_regulator_get(dev, "vdd-1p25");
	if (IS_ERR(td->l25)) {
		ret = dev_err_probe(dev, PTR_ERR(td->l25), "L25 supply\n");
		goto put_controller;
	}
	td->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(td->reset)) {
		ret = dev_err_probe(dev, PTR_ERR(td->reset), "codec reset\n");
		goto put_controller;
	}
	td->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(td->mclk)) {
		ret = dev_err_probe(dev, PTR_ERR(td->mclk), "codec MCLK\n");
		goto put_controller;
	}
	mutex_init(&td->lock);
	td->power.apply = flo_tabla_apply;
	td->power.ctx = td;
	td->power.reset_asserted = true;
	platform_set_drvdata(pdev, td);
	ret = device_create_file(dev, &dev_attr_enabled);
	if (ret)
		goto put_controller;
	ret = 0;

put_controller:
	put_device(&controller->dev);
	return ret;
}

static void flo_tabla_remove(struct platform_device *pdev)
{
	struct flo_tabla_discovery *td = platform_get_drvdata(pdev);
	int ret;

	device_remove_file(&pdev->dev, &dev_attr_enabled);
	mutex_lock(&td->lock);
	ret = flo_tabla_power_set(&td->power, false);
	mutex_unlock(&td->lock);
	if (ret)
		dev_err(&pdev->dev, "codec shutdown failed: %d\n", ret);
}

static int flo_tabla_suspend(struct device *dev)
{
	struct flo_tabla_discovery *td = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&td->lock);
	ret = flo_tabla_power_set(&td->power, false);
	mutex_unlock(&td->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(flo_tabla_pm_ops, flo_tabla_suspend, NULL);

static const struct of_device_id flo_tabla_of_match[] = {
	{ .compatible = "asus,flo-tabla-discovery" },
	{}
};
MODULE_DEVICE_TABLE(of, flo_tabla_of_match);

static struct platform_driver flo_tabla_driver = {
	.probe = flo_tabla_probe,
	.remove = flo_tabla_remove,
	.driver = {
		.name = "flo-tabla-discovery",
		.suppress_bind_attrs = true,
		.of_match_table = flo_tabla_of_match,
		.pm = pm_sleep_ptr(&flo_tabla_pm_ops),
	},
};
module_platform_driver(flo_tabla_driver);

MODULE_DESCRIPTION("Flo tabla SLIMbus codec discovery power sequencer");
MODULE_LICENSE("GPL");

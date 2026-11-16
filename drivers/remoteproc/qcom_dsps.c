// SPDX-License-Identifier: GPL-2.0-only
/* Qualcomm APQ8064 ARM7 sensor processor, secure peripheral loader. */
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include "qcom_common.h"
#include "qcom_dsps_fw.h"

#define DSPS_PAS_ID		2
#define DSPS_WDOG_INT_EN	0x1808
#define DSPS_DDR_BASE		0x8fe00000
#define DSPS_DDR_SIZE		SZ_1M

struct qcom_dsps {
	struct device *dev;
	struct rproc *rproc;
	void __iomem *ppss;
	struct dsps_mem_region mem[4];
	struct qcom_rproc_subdev smd;
	int watchdog_irq;
	int fatal_irq;
	bool pas_initialized;
	bool irqs_enabled;
};

static int dsps_shutdown(struct qcom_dsps *dsps)
{
	int ret;

	if (!dsps->pas_initialized)
		return 0;

	ret = qcom_scm_pas_shutdown(DSPS_PAS_ID);
	if (!ret)
		dsps->pas_initialized = false;
	else
		dev_err(dsps->dev, "PAS shutdown failed: %d\n", ret);

	return ret;
}

static int dsps_unprepare(struct rproc *rproc)
{
	return dsps_shutdown(rproc->priv);
}

static int dsps_sanity_check(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_dsps *dsps = rproc->priv;

	return dsps_validate_fw(fw, dsps->mem, ARRAY_SIZE(dsps->mem));
}

static void *dsps_da_to_va(struct rproc *rproc, u64 da, size_t len,
			 bool *is_iomem)
{
	struct qcom_dsps *dsps = rproc->priv;
	int i;

	for (i = 0; i < ARRAY_SIZE(dsps->mem); i++) {
		struct dsps_mem_region *mem = &dsps->mem[i];

		if (da < mem->addr || da - mem->addr > mem->size ||
		    len > mem->size - (da - mem->addr))
			continue;

		if (is_iomem)
			*is_iomem = true;
		return (__force void *)(mem->va + (da - mem->addr));
	}

	return NULL;
}

static int dsps_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_dsps *dsps = rproc->priv;
	const struct elf32_hdr *ehdr = (const void *)fw->data;
	const struct elf32_phdr *phdr;
	const struct firmware **blobs;
	size_t metadata_size;
	void *metadata;
	int ret, i;

	ret = dsps_sanity_check(rproc, fw);
	if (ret)
		return ret;

	/* A previous failed authentication must be quiesced before another load. */
	ret = dsps_shutdown(dsps);
	if (ret)
		return ret;

	phdr = (const void *)(fw->data + ehdr->e_phoff);
	blobs = kcalloc(ehdr->e_phnum, sizeof(*blobs), GFP_KERNEL);
	if (!blobs)
		return -ENOMEM;

	/* Stage all split blobs: no PAS or hardware write until all sizes match. */
	for (i = 0; i < ehdr->e_phnum; i++) {
		char *name;

		if (phdr[i].p_type != PT_LOAD ||
		    (phdr[i].p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH ||
		    !phdr[i].p_filesz)
			continue;

		name = kasprintf(GFP_KERNEL, "%.*sb%02d",
				 (int)strlen(rproc->firmware) - 3, rproc->firmware, i);
		if (!name) {
			ret = -ENOMEM;
			goto release_blobs;
		}
		ret = request_firmware(&blobs[i], name, dsps->dev);
		kfree(name);
		if (ret)
			goto release_blobs;
		if (blobs[i]->size != phdr[i].p_filesz) {
			dev_err(dsps->dev, "firmware segment %d has invalid size\n", i);
			ret = -EINVAL;
			goto release_blobs;
		}
	}

	metadata = qcom_mdt_read_metadata(fw, &metadata_size,
					rproc->firmware, dsps->dev);
	if (IS_ERR(metadata)) {
		ret = PTR_ERR(metadata);
		goto release_blobs;
	}
	ret = qcom_scm_pas_init_image(DSPS_PAS_ID, metadata, metadata_size, NULL);
	kfree(metadata);
	if (ret)
		goto release_blobs;
	dsps->pas_initialized = true;

	for (i = 0; i < ehdr->e_phnum; i++) {
		void __iomem *ptr;

		if (phdr[i].p_type != PT_LOAD || !phdr[i].p_memsz ||
		    (phdr[i].p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH)
			continue;

		ptr = (__force void __iomem *)dsps_da_to_va(rproc, phdr[i].p_paddr,
							 phdr[i].p_memsz, NULL);
		if (phdr[i].p_filesz)
			memcpy_toio(ptr, blobs[i]->data, phdr[i].p_filesz);
		memset_io(ptr + phdr[i].p_filesz, 0,
			  phdr[i].p_memsz - phdr[i].p_filesz);
	}
	/* Complete SRAM/DDR writes before PAS can release the processor. */
	wmb();
	ret = qcom_register_dump_segments(rproc, fw);

release_blobs:
	for (i = 0; i < ehdr->e_phnum; i++)
		release_firmware(blobs[i]);
	kfree(blobs);
	return ret;
}

static int dsps_start(struct rproc *rproc)
{
	struct qcom_dsps *dsps = rproc->priv;
	int ret;

	ret = qcom_scm_pas_auth_and_reset(DSPS_PAS_ID);
	if (ret)
		return ret;

	enable_irq(dsps->watchdog_irq);
	enable_irq(dsps->fatal_irq);
	dsps->irqs_enabled = true;
	return 0;
}

static int dsps_stop(struct rproc *rproc)
{
	struct qcom_dsps *dsps = rproc->priv;
	u32 watchdog_mask;
	int ret;

	if (dsps->irqs_enabled) {
		disable_irq(dsps->watchdog_irq);
		disable_irq(dsps->fatal_irq);
		dsps->irqs_enabled = false;
	}
	watchdog_mask = readl(dsps->ppss + DSPS_WDOG_INT_EN);
	writel(0, dsps->ppss + DSPS_WDOG_INT_EN);
	ret = dsps_shutdown(dsps);
	if (ret) {
		writel(watchdog_mask, dsps->ppss + DSPS_WDOG_INT_EN);
		enable_irq(dsps->watchdog_irq);
		enable_irq(dsps->fatal_irq);
		dsps->irqs_enabled = true;
	}
	return ret;
}

static irqreturn_t dsps_watchdog(int irq, void *data)
{
	struct qcom_dsps *dsps = data;

	rproc_report_crash(dsps->rproc, RPROC_WATCHDOG);
	return IRQ_HANDLED;
}

static irqreturn_t dsps_fatal(int irq, void *data)
{
	struct qcom_dsps *dsps = data;

	rproc_report_crash(dsps->rproc, RPROC_FATAL_ERROR);
	return IRQ_HANDLED;
}

static const struct rproc_ops dsps_ops = {
	.sanity_check = dsps_sanity_check,
	.load = dsps_load,
	.start = dsps_start,
	.stop = dsps_stop,
	.unprepare = dsps_unprepare,
	.da_to_va = dsps_da_to_va,
};

static void dsps_remove_smd(void *data)
{
	struct qcom_dsps *dsps = data;

	qcom_remove_smd_subdev(dsps->rproc, &dsps->smd);
}

static int dsps_probe(struct platform_device *pdev)
{
	static const char * const mem_names[] = { "tcm-code", "tcm-buffer", "pipe" };
	struct device *dev = &pdev->dev;
	const char *firmware = "dsps.mdt";
	struct device_node *node;
	struct reserved_mem *rmem;
	struct qcom_dsps *dsps;
	struct resource *res;
	struct rproc *rproc;
	int i, ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;
	if (!qcom_scm_pas_supported(DSPS_PAS_ID))
		return dev_err_probe(dev, -EOPNOTSUPP, "secure DSPS boot is unavailable\n");

	ret = of_property_read_string(dev->of_node, "firmware-name", &firmware);
	if (ret && ret != -EINVAL)
		return ret;
	if (strlen(firmware) < 4 || strcmp(firmware + strlen(firmware) - 4, ".mdt"))
		return -EINVAL;

	rproc = devm_rproc_alloc(dev, "apq8064-dsps", &dsps_ops, firmware, sizeof(*dsps));
	if (!rproc)
		return -ENOMEM;
	/* Bring-up is explicit: reserving memory never starts untested firmware. */
	rproc->auto_boot = false;
	rproc->recovery_disabled = true;
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_ARM);
	dsps = rproc->priv;
	dsps->dev = dev;
	dsps->rproc = rproc;

	dsps->ppss = devm_platform_ioremap_resource_byname(pdev, "ppss");
	if (IS_ERR(dsps->ppss))
		return PTR_ERR(dsps->ppss);

	for (i = 0; i < ARRAY_SIZE(mem_names); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, mem_names[i]);
		if (!res)
			return -EINVAL;
		dsps->mem[i].addr = res->start;
		dsps->mem[i].size = resource_size(res);
		dsps->mem[i].va = devm_ioremap_resource(dev, res);
		if (IS_ERR(dsps->mem[i].va))
			return PTR_ERR(dsps->mem[i].va);
	}

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return -EINVAL;
	rmem = of_reserved_mem_lookup(node);
	ret = of_property_present(node, "no-map") ? 0 : -EINVAL;
	of_node_put(node);
	if (ret || !rmem || rmem->base != DSPS_DDR_BASE || rmem->size != DSPS_DDR_SIZE)
		return dev_err_probe(dev, -EINVAL, "DSPS requires reserved no-map DDR\n");
	dsps->mem[3].addr = rmem->base;
	dsps->mem[3].size = rmem->size;
	dsps->mem[3].va = devm_ioremap_wc(dev, rmem->base, rmem->size);
	if (!dsps->mem[3].va)
		return -ENOMEM;

	dsps->watchdog_irq = platform_get_irq_byname(pdev, "watchdog");
	if (dsps->watchdog_irq < 0)
		return dsps->watchdog_irq;
	ret = devm_request_irq(dev, dsps->watchdog_irq, dsps_watchdog,
			       IRQF_NO_AUTOEN, "dsps-watchdog", dsps);
	if (ret)
		return ret;
	dsps->fatal_irq = platform_get_irq_byname(pdev, "fatal");
	if (dsps->fatal_irq < 0)
		return dsps->fatal_irq;
	ret = devm_request_irq(dev, dsps->fatal_irq, dsps_fatal,
			       IRQF_NO_AUTOEN, "dsps-fatal", dsps);
	if (ret)
		return ret;

	qcom_add_smd_subdev(rproc, &dsps->smd);
	ret = devm_add_action_or_reset(dev, dsps_remove_smd, dsps);
	if (ret)
		return ret;
	return devm_rproc_add(dev, rproc);
}

static const struct of_device_id dsps_of_match[] = {
	{ .compatible = "qcom,apq8064-dsps-pil" },
	{}
};
MODULE_DEVICE_TABLE(of, dsps_of_match);

static struct platform_driver dsps_driver = {
	.probe = dsps_probe,
	.driver = {
		.name = "qcom-apq8064-dsps",
		.of_match_table = dsps_of_match,
	},
};
module_platform_driver(dsps_driver);
MODULE_DESCRIPTION("Qualcomm APQ8064 ARM7 sensor processor loader");
MODULE_LICENSE("GPL");

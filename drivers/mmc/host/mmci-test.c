// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <kunit/device.h>
#if IS_ENABLED(CONFIG_INDIRECT_IOMEM)
#include <linux/logic_iomem.h>

static unsigned long mmci_test_mmio_read(void *priv, unsigned int offset, int size)
{
	return ((u32 *)priv)[offset / sizeof(u32)];
}

static void mmci_test_mmio_write(void *priv, unsigned int offset, int size,
				 unsigned long value)
{
	((u32 *)priv)[offset / sizeof(u32)] = value;
}

static void mmci_test_mmio_unmap(void *priv)
{
	kfree(priv);
}

static const struct logic_iomem_ops mmci_test_mmio_ops = {
	.read = mmci_test_mmio_read,
	.write = mmci_test_mmio_write,
	.unmap = mmci_test_mmio_unmap,
};

static long mmci_test_mmio_map(unsigned long offset, size_t size,
			       const struct logic_iomem_ops **ops, void **priv)
{
	*priv = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!*priv)
		return -ENOMEM;
	*ops = &mmci_test_mmio_ops;
	return 0;
}

static const struct logic_iomem_region_ops mmci_test_region_ops = {
	.map = mmci_test_mmio_map,
};

/* This test option is built-in: the region and ops live until kernel exit. */
static struct resource mmci_test_mmio_resource = {
	.name = "mmci-kunit-mmio",
	.start = 0xe0000000,
	.end = 0xe0000000 + PAGE_SIZE - 1,
	.flags = IORESOURCE_MEM,
};

static int mmci_test_suite_init(struct kunit_suite *suite)
{
	return logic_iomem_add_region(&mmci_test_mmio_resource, &mmci_test_region_ops);
}

static void mmci_test_unmap_mmio(void *base)
{
	iounmap((void __iomem *)base);
}
#endif

/* A DMAengine boundary that records the SG owner of prepared transfers. */
struct mmci_test_dma_backend {
	struct mmc_host *mmc;
	struct mmci_host *host;
	struct mmci_dmae_priv dmae;
	struct dma_device engine;
	struct dma_chan chan;
	struct dma_async_tx_descriptor desc[4];
	struct scatterlist *prepared[4];
	unsigned int preparations;
	unsigned int configurations;
	unsigned int terminations;
	int config_error;
	bool prep_error;
	struct mmc_data next;
	struct scatterlist next_sg;
	u8 *buffer;
};

static int mmci_test_dma_config(struct dma_chan *chan,
				struct dma_slave_config *config)
{
	struct mmci_test_dma_backend *ctx = container_of(chan,
					struct mmci_test_dma_backend, chan);

	ctx->configurations++;
	return ctx->config_error;
}

static struct dma_async_tx_descriptor *
mmci_test_dma_prepare(struct dma_chan *chan, struct scatterlist *sg,
		      unsigned int sg_len, enum dma_transfer_direction dir,
		     unsigned long flags, void *context)
{
	struct mmci_test_dma_backend *ctx = container_of(chan,
					struct mmci_test_dma_backend, chan);
	unsigned int index = ctx->preparations++;

	if (ctx->prep_error || index >= ARRAY_SIZE(ctx->desc))
		return NULL;
	ctx->prepared[index] = sg;
	dma_async_tx_descriptor_init(&ctx->desc[index], chan);
	return &ctx->desc[index];
}

static int mmci_test_dma_terminate(struct dma_chan *chan)
{
	struct mmci_test_dma_backend *ctx = container_of(chan,
					struct mmci_test_dma_backend, chan);

	ctx->terminations++;
	return 0;
}

static void mmci_test_free_mmc(void *mmc)
{
	mmc_free_host(mmc);
}

static struct mmci_test_dma_backend *mmci_test_dma_setup(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx;
	struct device *dev;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	dev = kunit_device_register(test, "mmci-dma-test");
	if (IS_ERR(dev))
		return NULL;
	dev->dma_mask = &dev->coherent_dma_mask;
	dev->coherent_dma_mask = DMA_BIT_MASK(64);
	ctx->mmc = mmc_alloc_host(sizeof(*ctx->host), dev);
	if (!ctx->mmc)
		return NULL;
	if (kunit_add_action_or_reset(test, mmci_test_free_mmc, ctx->mmc))
		return NULL;
	ctx->host = mmc_priv(ctx->mmc);
	ctx->host->mmc = ctx->mmc;
#if IS_ENABLED(CONFIG_INDIRECT_IOMEM)
	ctx->host->base = ioremap(mmci_test_mmio_resource.start, PAGE_SIZE);
	if (!ctx->host->base ||
	    kunit_add_action_or_reset(test, mmci_test_unmap_mmio,
				      (__force void *)ctx->host->base))
		return NULL;
#else
	ctx->host->base = (__force void __iomem *)kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	if (!ctx->host->base)
		return NULL;
#endif
	ctx->host->variant = &variant_arm;
	ctx->host->ops = &mmci_variant_ops;
	ctx->host->use_dma = true;
	ctx->host->dma_priv = &ctx->dmae;
	ctx->engine.dev = dev;
	ctx->engine.device_config = mmci_test_dma_config;
	ctx->engine.device_terminate_all = mmci_test_dma_terminate;
	ctx->engine.device_prep_slave_sg = mmci_test_dma_prepare;
	ctx->chan.device = &ctx->engine;
	ctx->dmae.rx_channel = &ctx->chan;
	ctx->dmae.tx_channel = &ctx->chan;
	ctx->buffer = kunit_kzalloc(test, 512, GFP_KERNEL);
	if (!ctx->buffer)
		return NULL;
	sg_init_one(&ctx->next_sg, ctx->buffer, 512);
	ctx->next.blksz = 512;
	ctx->next.blocks = 1;
	ctx->next.flags = MMC_DATA_READ;
	ctx->next.sg = &ctx->next_sg;
	ctx->next.sg_len = 1;
	return ctx;
}

static void mmci_pre_req_does_not_reconfigure_an_active_channel(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx = mmci_test_dma_setup(test);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ret = mmci_prep_data(ctx->host, &ctx->next, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT(test, ctx->next.sg_count, 0);
	KUNIT_EXPECT_EQ(test, ctx->configurations, 0);
	KUNIT_EXPECT_EQ(test, ctx->preparations, 0);
	mmci_unprep_data(ctx->host, &ctx->next, 0);
}

static void mmci_sync_transfer_prepares_its_own_scatterlist(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx = mmci_test_dma_setup(test);
	struct mmc_data sync = {};
	struct scatterlist sg;
	void *buffer;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	buffer = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buffer);
	sg_init_one(&sg, buffer, 512);
	sync.blksz = 512;
	sync.blocks = 1;
	sync.flags = MMC_DATA_READ;
	sync.sg = &sg;
	sync.sg_len = 1;
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, true), 0);
	mmci_get_next_data(ctx->host, &sync);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &sync, false), 0);
	KUNIT_EXPECT_EQ(test, ctx->preparations, 1);
	KUNIT_EXPECT_PTR_EQ(test, ctx->prepared[0], &sg);
	KUNIT_EXPECT_GT(test, ctx->next.sg_count, 0);
	mmci_unprep_data(ctx->host, &sync, 0);
	mmci_unprep_data(ctx->host, &ctx->next, 0);
}

static void mmci_cancel_next_does_not_terminate_current(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx = mmci_test_dma_setup(test);
	struct mmc_data active_data = {};
	struct scatterlist sg;
	struct dma_async_tx_descriptor *desc;
	void *buffer;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	buffer = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buffer);
	sg_init_one(&sg, buffer, 512);
	active_data.blksz = 512;
	active_data.blocks = 1;
	active_data.flags = MMC_DATA_READ;
	active_data.sg = &sg;
	active_data.sg_len = 1;
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, true), 0);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &active_data, false), 0);
	desc = ctx->dmae.desc_current;
	ctx->host->dma_in_progress = true;
	mmci_unprep_data(ctx->host, &ctx->next, -EIO);
	KUNIT_EXPECT_EQ(test, ctx->terminations, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.cur, &ctx->chan);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.desc_current, desc);
	KUNIT_EXPECT_TRUE(test, ctx->host->dma_in_progress);
	KUNIT_EXPECT_EQ(test, ctx->next.host_cookie, 0);
	ctx->host->dma_in_progress = false;
	mmci_unprep_data(ctx->host, &active_data, 0);
}

static void mmci_dma_config_failure_returns_mapping_to_cpu(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx = mmci_test_dma_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, true), 0);
	ctx->config_error = -EIO;
	mmci_get_next_data(ctx->host, &ctx->next);
	KUNIT_EXPECT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, false), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->next.sg_count, 0);
	KUNIT_EXPECT_EQ(test, ctx->next.host_cookie, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.cur, NULL);
}

static void mmci_dma_prep_failure_returns_mapping_to_cpu(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx = mmci_test_dma_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, true), 0);
	ctx->prep_error = true;
	mmci_get_next_data(ctx->host, &ctx->next);
	KUNIT_EXPECT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, false), -ENOMEM);
	KUNIT_EXPECT_EQ(test, ctx->next.sg_count, 0);
	KUNIT_EXPECT_EQ(test, ctx->next.host_cookie, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.desc_current, NULL);
}

static void mmci_post_previous_request_preserves_started_next(struct kunit *test)
{
	struct mmci_test_dma_backend *ctx = mmci_test_dma_setup(test);
	struct mmc_data following = {};
	struct scatterlist sg;
	struct dma_async_tx_descriptor *desc;
	void *buffer;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	buffer = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buffer);
	sg_init_one(&sg, buffer, 512);
	following.blksz = 512;
	following.blocks = 1;
	following.flags = MMC_DATA_READ;
	following.sg = &sg;
	following.sg_len = 1;
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, true), 0);
	mmci_get_next_data(ctx->host, &ctx->next);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &ctx->next, false), 0);
	ctx->host->dma_in_progress = true;
	mmci_dmae_finalize(ctx->host, &ctx->next);
	KUNIT_EXPECT_GT(test, ctx->next.sg_count, 0);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &following, true), 0);
	mmci_get_next_data(ctx->host, &following);
	KUNIT_ASSERT_EQ(test, mmci_prep_data(ctx->host, &following, false), 0);
	desc = ctx->dmae.desc_current;
	mmci_unprep_data(ctx->host, &ctx->next, 0);
	KUNIT_EXPECT_EQ(test, ctx->next.sg_count, 0);
	KUNIT_EXPECT_GT(test, following.sg_count, 0);
	KUNIT_EXPECT_GT(test, following.host_cookie, 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.desc_current, desc);
	mmci_unprep_data(ctx->host, &following, 0);
}

static struct kunit_case mmci_dma_cases[] = {
	KUNIT_CASE(mmci_pre_req_does_not_reconfigure_an_active_channel),
	KUNIT_CASE(mmci_dma_config_failure_returns_mapping_to_cpu),
	KUNIT_CASE(mmci_dma_prep_failure_returns_mapping_to_cpu),
	KUNIT_CASE(mmci_post_previous_request_preserves_started_next),
	KUNIT_CASE(mmci_sync_transfer_prepares_its_own_scatterlist),
	KUNIT_CASE(mmci_cancel_next_does_not_terminate_current),
	{}
};

static struct kunit_suite mmci_dma_suite = {
#if IS_ENABLED(CONFIG_INDIRECT_IOMEM)
	.suite_init = mmci_test_suite_init,
#endif
	.name = "mmci-dma-handover",
	.test_cases = mmci_dma_cases,
};

kunit_test_suite(mmci_dma_suite);

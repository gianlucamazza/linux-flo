// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

struct mmci_dma_test {
	struct mmci_host host;
	struct mmci_dmae_priv dmae;
	struct mmc_data data;
	struct dma_chan chan;
	struct dma_async_tx_descriptor desc;
};

static struct mmci_dma_test *mmci_test_setup(struct kunit *test)
{
	struct mmci_dma_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	ctx->host.use_dma = true;
	ctx->host.dma_priv = &ctx->dmae;
	ctx->dmae.next_data.chan = &ctx->chan;
	ctx->dmae.next_data.desc = &ctx->desc;
	return ctx;
}

static void mmci_sync_request_preserves_next_mapping(struct kunit *test)
{
	struct mmci_dma_test *ctx = mmci_test_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	/* EXT_CSD/BKOPS bypasses pre_req while a block request is pre-mapped. */
	mmci_dmae_get_next_data(&ctx->host, &ctx->data);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.next_data.chan, &ctx->chan);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.next_data.desc, &ctx->desc);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.cur, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.desc_current, NULL);
}

static void mmci_prepared_request_takes_own_mapping(struct kunit *test)
{
	struct mmci_dma_test *ctx = mmci_test_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->data.host_cookie = 7;
	mmci_dmae_get_next_data(&ctx->host, &ctx->data);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.cur, &ctx->chan);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.desc_current, &ctx->desc);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.next_data.chan, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.next_data.desc, NULL);
}

static void mmci_pio_request_preserves_mapping(struct kunit *test)
{
	struct mmci_dma_test *ctx = mmci_test_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->host.use_dma = false;
	ctx->data.host_cookie = 7;
	mmci_dmae_get_next_data(&ctx->host, &ctx->data);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.next_data.chan, &ctx->chan);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.next_data.desc, &ctx->desc);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.cur, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->dmae.desc_current, NULL);
}

static struct kunit_case mmci_dma_cases[] = {
	KUNIT_CASE(mmci_sync_request_preserves_next_mapping),
	KUNIT_CASE(mmci_prepared_request_takes_own_mapping),
	KUNIT_CASE(mmci_pio_request_preserves_mapping),
	{}
};

static struct kunit_suite mmci_dma_suite = {
	.name = "mmci-dma-handover",
	.test_cases = mmci_dma_cases,
};

kunit_test_suite(mmci_dma_suite);

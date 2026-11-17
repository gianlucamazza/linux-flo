// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/device.h>
#include <kunit/test.h>

struct rproc_lifecycle_test {
	struct rproc *rproc;
	struct rproc_subdev subdev;
	unsigned int stops;
	unsigned int subdev_stops;
	unsigned int unprepares;
	int stop_error;
	int detach_error;
	bool crashed_stop;
};

static struct rproc_lifecycle_test *rproc_lifecycle_setup(struct kunit *test);

static int rproc_test_start(struct rproc *rproc)
{
	return 0;
}

static int rproc_test_stop(struct rproc *rproc)
{
	struct rproc_lifecycle_test *ctx = rproc->priv;

	ctx->stops++;
	return ctx->stop_error;
}

static int rproc_test_detach(struct rproc *rproc)
{
	struct rproc_lifecycle_test *ctx = rproc->priv;

	return ctx->detach_error;
}

static int rproc_test_attach(struct rproc *rproc)
{
	return 0;
}

static int rproc_test_unprepare(struct rproc *rproc)
{
	struct rproc_lifecycle_test *ctx = rproc->priv;

	ctx->unprepares++;
	return 0;
}

static int rproc_test_subdev_start(struct rproc_subdev *subdev)
{
	return 0;
}

static void rproc_test_subdev_stop(struct rproc_subdev *subdev, bool crashed)
{
	struct rproc_lifecycle_test *ctx = container_of(subdev,
						struct rproc_lifecycle_test, subdev);

	ctx->subdev_stops++;
	ctx->crashed_stop = crashed;
}

static const struct rproc_ops rproc_lifecycle_test_ops = {
	.start = rproc_test_start,
	.stop = rproc_test_stop,
	.detach = rproc_test_detach,
	.attach = rproc_test_attach,
	.unprepare = rproc_test_unprepare,
};

static void rproc_test_free(void *data)
{
	rproc_free(data);
}

static struct fw_rsc_vdev_vring *rproc_test_vring(struct resource_table *table)
{
	return (void *)table + sizeof(*table) + sizeof(u32) +
	       sizeof(u32) + sizeof(struct fw_rsc_vdev);
}

/* Use different clean and configured vring assignments, as on an attached rproc. */
static struct resource_table *
rproc_test_table(struct kunit *test, struct rproc *rproc)
{
	struct resource_table *table;
	struct fw_rsc_vdev *vdev;

	rproc->table_sz = sizeof(*table) + sizeof(u32) +
			  sizeof(u32) + sizeof(*vdev) +
			  sizeof(struct fw_rsc_vdev_vring);
	table = kunit_kzalloc(test, rproc->table_sz, GFP_KERNEL);
	if (!table)
		return NULL;
	table->ver = 1;
	table->num = 1;
	table->offset[0] = sizeof(*table) + sizeof(u32);
	*(u32 *)((void *)table + table->offset[0]) = RSC_VDEV;
	vdev = (void *)table + table->offset[0] + sizeof(u32);
	vdev->num_of_vrings = 1;
	rproc->table_ptr = table;
	rproc->clean_table = kmemdup(table, rproc->table_sz, GFP_KERNEL);
	if (!rproc->clean_table)
		return NULL;
	rproc_test_vring(table)->da = 0x12345000;
	rproc_test_vring(table)->notifyid = 42;
	return table;
}

static void rproc_failed_stop_keeps_attached_table(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);
	struct resource_table *table;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	table = rproc_test_table(test, ctx->rproc);
	KUNIT_ASSERT_NOT_NULL(test, table);
	ctx->rproc->state = RPROC_CRASHED;
	ctx->stop_error = -EIO;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), -EIO);
	KUNIT_EXPECT_PTR_EQ(test, ctx->rproc->table_ptr, table);
	KUNIT_EXPECT_EQ(test, rproc_test_vring(table)->da, 0x12345000U);
	KUNIT_EXPECT_EQ(test, rproc_test_vring(table)->notifyid, 42U);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 1);
	/* The failed attempt still owns the clean image for recovery/retry. */
	KUNIT_EXPECT_NOT_NULL(test, ctx->rproc->clean_table);
	ctx->stop_error = 0;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->rproc->clean_table, NULL);
}

static void rproc_failed_detach_preserves_active_table(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);
	struct resource_table *table;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	table = rproc_test_table(test, ctx->rproc);
	KUNIT_ASSERT_NOT_NULL(test, table);
	ctx->rproc->state = RPROC_ATTACHED;
	ctx->detach_error = -EIO;
	KUNIT_EXPECT_EQ(test, rproc_detach(ctx->rproc), -EIO);
	KUNIT_EXPECT_PTR_EQ(test, ctx->rproc->table_ptr, table);
	KUNIT_EXPECT_EQ(test, rproc_test_vring(table)->da, 0x12345000U);
	KUNIT_EXPECT_EQ(test, rproc_test_vring(table)->notifyid, 42U);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 1);
	ctx->detach_error = 0;
	KUNIT_EXPECT_EQ(test, rproc_detach(ctx->rproc), 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->rproc->clean_table, NULL);
}

static void rproc_failed_crashed_stop_then_attach_recovery(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);
	struct resource_table *table;
	int cycle;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	table = rproc_test_table(test, ctx->rproc);
	KUNIT_ASSERT_NOT_NULL(test, table);
	rproc_set_feature(ctx->rproc, RPROC_FEAT_ATTACH_ON_RECOVERY);
	ctx->rproc->state = RPROC_CRASHED;
	ctx->stop_error = -EIO;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), -EIO);
	ctx->stop_error = 0;
	for (cycle = 0; cycle < 2; cycle++) {
		KUNIT_ASSERT_EQ(test, rproc_trigger_recovery(ctx->rproc), 0);
		KUNIT_EXPECT_PTR_EQ(test, ctx->rproc->table_ptr, table);
		KUNIT_EXPECT_EQ(test, rproc_test_vring(table)->da, 0x12345000U);
		KUNIT_EXPECT_EQ(test, rproc_test_vring(table)->notifyid, 42U);
		KUNIT_EXPECT_NOT_NULL(test, ctx->rproc->clean_table);
		KUNIT_EXPECT_EQ(test, ctx->rproc->state, RPROC_ATTACHED);
		ctx->rproc->state = RPROC_CRASHED;
	}
	ctx->rproc->state = RPROC_ATTACHED;
	KUNIT_EXPECT_EQ(test, rproc_detach(ctx->rproc), 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->rproc->clean_table, NULL);
}

static struct rproc_lifecycle_test *rproc_lifecycle_setup(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx;
	struct device *dev;
	struct rproc *rproc;

	dev = kunit_device_register(test, "rproc-lifecycle-test");
	if (IS_ERR(dev))
		return NULL;
	rproc = rproc_alloc(dev, "kunit-rproc", &rproc_lifecycle_test_ops,
			    "unused-firmware", sizeof(*ctx));
	if (!rproc)
		return NULL;
	if (kunit_add_action_or_reset(test, rproc_test_free, rproc))
		return NULL;
	ctx = rproc->priv;
	ctx->rproc = rproc;
	ctx->subdev.start = rproc_test_subdev_start;
	ctx->subdev.stop = rproc_test_subdev_stop;
	rproc_add_subdev(rproc, &ctx->subdev);
	if (rproc_start_subdevices(rproc))
		return NULL;
	rproc->state = RPROC_RUNNING;
	rproc->recovery_disabled = true;
	atomic_set(&rproc->power, 1);
	return ctx;
}

static void rproc_shutdown_after_crash_releases_resources(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->rproc->state = RPROC_CRASHED;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 1);
	KUNIT_EXPECT_EQ(test, ctx->subdev_stops, 1);
	KUNIT_EXPECT_TRUE(test, ctx->crashed_stop);
	KUNIT_EXPECT_EQ(test, ctx->unprepares, 1);
	KUNIT_EXPECT_EQ(test, ctx->rproc->state, RPROC_OFFLINE);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 0);
}

static void rproc_failed_stop_retry_does_not_stop_subdevices_twice(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->stop_error = -EIO;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->rproc->state, RPROC_RUNNING);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 1);
	KUNIT_EXPECT_EQ(test, ctx->unprepares, 0);
	ctx->stop_error = 0;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 2);
	KUNIT_EXPECT_EQ(test, ctx->subdev_stops, 1);
	KUNIT_EXPECT_EQ(test, ctx->unprepares, 1);
	KUNIT_EXPECT_EQ(test, ctx->rproc->state, RPROC_OFFLINE);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 0);
}

static void rproc_running_shutdown_still_stops_once(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 1);
	KUNIT_EXPECT_EQ(test, ctx->subdev_stops, 1);
	KUNIT_EXPECT_FALSE(test, ctx->crashed_stop);
	KUNIT_EXPECT_EQ(test, ctx->unprepares, 1);
}

static void rproc_attached_shutdown_still_stops_once(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->rproc->state = RPROC_ATTACHED;
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 1);
	KUNIT_EXPECT_EQ(test, ctx->subdev_stops, 1);
	KUNIT_EXPECT_FALSE(test, ctx->crashed_stop);
}

static void rproc_crashed_shared_reference_waits_for_last_release(struct kunit *test)
{
	struct rproc_lifecycle_test *ctx = rproc_lifecycle_setup(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->rproc->state = RPROC_CRASHED;
	atomic_set(&ctx->rproc->power, 2);
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 1);
	KUNIT_EXPECT_EQ(test, rproc_shutdown(ctx->rproc), 0);
	KUNIT_EXPECT_EQ(test, ctx->stops, 1);
	KUNIT_EXPECT_EQ(test, ctx->rproc->state, RPROC_OFFLINE);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->rproc->power), 0);
}

static struct kunit_case rproc_lifecycle_cases[] = {
	KUNIT_CASE(rproc_shutdown_after_crash_releases_resources),
	KUNIT_CASE(rproc_failed_stop_retry_does_not_stop_subdevices_twice),
	KUNIT_CASE(rproc_running_shutdown_still_stops_once),
	KUNIT_CASE(rproc_attached_shutdown_still_stops_once),
	KUNIT_CASE(rproc_crashed_shared_reference_waits_for_last_release),
	KUNIT_CASE(rproc_failed_stop_keeps_attached_table),
	KUNIT_CASE(rproc_failed_detach_preserves_active_table),
	KUNIT_CASE(rproc_failed_crashed_stop_then_attach_recovery),
	{}
};

static struct kunit_suite rproc_lifecycle_suite = {
	.name = "remoteproc-lifecycle",
	.test_cases = rproc_lifecycle_cases,
};

kunit_test_suite(rproc_lifecycle_suite);

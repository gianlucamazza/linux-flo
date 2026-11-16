// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include "qcom_dsps_fw.h"

struct dsps_test_image {
	struct elf32_hdr hdr;
	struct elf32_phdr phdr[4];
	u8 hash[4];
};

static const struct dsps_mem_region test_regions[] = {
	{ .addr = 0x12000000, .size = 0x28000 },
	{ .addr = 0x8fe00000, .size = 0x100000 },
};

static void dsps_test_init(struct dsps_test_image *image)
{
	memset(image, 0, sizeof(*image));
	memcpy(image->hdr.e_ident, ELFMAG, SELFMAG);
	image->hdr.e_ident[EI_CLASS] = ELFCLASS32;
	image->hdr.e_ident[EI_DATA] = ELFDATA2LSB;
	image->hdr.e_ident[EI_VERSION] = EV_CURRENT;
	image->hdr.e_type = ET_EXEC;
	image->hdr.e_machine = EM_ARM;
	image->hdr.e_version = EV_CURRENT;
	image->hdr.e_ehsize = sizeof(image->hdr);
	image->hdr.e_phoff = sizeof(image->hdr);
	image->hdr.e_phentsize = sizeof(image->phdr[0]);
	image->hdr.e_phnum = ARRAY_SIZE(image->phdr);
	image->phdr[0].p_filesz = offsetof(struct dsps_test_image, hash);
	image->phdr[1].p_type = PT_LOAD;
	image->phdr[1].p_flags = QCOM_MDT_TYPE_HASH;
	image->phdr[1].p_filesz = sizeof(image->hash);
	image->phdr[2].p_type = PT_LOAD;
	image->phdr[2].p_paddr = 0x12000000;
	image->phdr[2].p_filesz = 4;
	image->phdr[2].p_memsz = 8;
	image->phdr[3].p_type = PT_LOAD;
	image->phdr[3].p_paddr = 0x8fe00000;
	image->phdr[3].p_filesz = 4;
	image->phdr[3].p_memsz = 8;
}

static int validate(struct dsps_test_image *image, size_t size)
{
	struct firmware fw = { .data = (u8 *)image, .size = size };

	return dsps_validate_fw(&fw, test_regions, ARRAY_SIZE(test_regions));
}

static void dsps_valid_scatter_image(struct kunit *test)
{
	struct dsps_test_image image;

	dsps_test_init(&image);
	/* Virtual zero is valid; physical TCM address determines the mapping. */
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), 0);
}

static void dsps_invalid_header(struct kunit *test)
{
	struct dsps_test_image image;

	dsps_test_init(&image);
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image.hdr) - 1), -EINVAL);
	image.hdr.e_machine = EM_HEXAGON;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
	dsps_test_init(&image);
	image.hdr.e_phnum = 101;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
	dsps_test_init(&image);
	image.hdr.e_phoff = U32_MAX;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
}

static void dsps_invalid_hash_metadata(struct kunit *test)
{
	struct dsps_test_image image;

	dsps_test_init(&image);
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image) - 1), -EINVAL);
	image.phdr[1].p_type = PT_NULL;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
	dsps_test_init(&image);
	image.phdr[1].p_filesz = 0;
	KUNIT_EXPECT_EQ(test, validate(&image, offsetof(struct dsps_test_image, hash)), -EINVAL);
}

static void dsps_invalid_load_ranges(struct kunit *test)
{
	struct dsps_test_image image;

	dsps_test_init(&image);
	image.phdr[2].p_paddr = 0x12027ffc;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
	image.phdr[2].p_paddr = U32_MAX - 3;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
	image.phdr[2].p_paddr = 0x12000000;
	image.phdr[2].p_memsz = 0;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
	dsps_test_init(&image);
	image.phdr[3].p_paddr = 0x12000004;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
}

static void dsps_rejects_relocation(struct kunit *test)
{
	struct dsps_test_image image;

	dsps_test_init(&image);
	image.phdr[2].p_flags = QCOM_MDT_RELOCATABLE;
	KUNIT_EXPECT_EQ(test, validate(&image, sizeof(image)), -EINVAL);
}

static struct kunit_case dsps_cases[] = {
	KUNIT_CASE(dsps_valid_scatter_image),
	KUNIT_CASE(dsps_invalid_header),
	KUNIT_CASE(dsps_invalid_hash_metadata),
	KUNIT_CASE(dsps_invalid_load_ranges),
	KUNIT_CASE(dsps_rejects_relocation),
	{}
};

static struct kunit_suite dsps_suite = {
	.name = "qcom-dsps-firmware",
	.test_cases = dsps_cases,
};
kunit_test_suite(dsps_suite);
MODULE_LICENSE("GPL");

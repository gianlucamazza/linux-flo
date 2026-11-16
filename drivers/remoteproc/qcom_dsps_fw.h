/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM_DSPS_FW_H
#define QCOM_DSPS_FW_H

#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/soc/qcom/mdt_loader.h>

struct dsps_mem_region {
	u32 addr;
	size_t size;
	void __iomem *va;
};

static inline int dsps_validate_fw(const struct firmware *fw,
				 const struct dsps_mem_region *regions,
				 size_t num_regions)
{
	const struct elf32_hdr *ehdr;
	const struct elf32_phdr *phdr;
	u64 header_end;
	unsigned int i, j;
	bool has_load = false;

	if (!fw || fw->size < sizeof(*ehdr))
		return -EINVAL;
	ehdr = (const void *)fw->data;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) ||
	    ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
	    ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
	    ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
	    ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_ARM ||
	    ehdr->e_version != EV_CURRENT || ehdr->e_ehsize != sizeof(*ehdr) ||
	    ehdr->e_phentsize != sizeof(*phdr) ||
	    ehdr->e_phnum < 3 || ehdr->e_phnum > 100 ||
	    ehdr->e_phoff < sizeof(*ehdr) || !IS_ALIGNED(ehdr->e_phoff, 4))
		return -EINVAL;

	header_end = (u64)ehdr->e_phoff + ehdr->e_phnum * sizeof(*phdr);
	if (header_end > fw->size)
		return -EINVAL;
	phdr = (const void *)(fw->data + ehdr->e_phoff);
	if (phdr[0].p_type != PT_NULL || phdr[0].p_offset ||
	    phdr[0].p_filesz != header_end ||
	    phdr[1].p_type != PT_LOAD || !phdr[1].p_filesz ||
	    (phdr[1].p_flags & QCOM_MDT_TYPE_MASK) != QCOM_MDT_TYPE_HASH ||
	    header_end + phdr[1].p_filesz != fw->size)
		return -EINVAL;

	for (i = 2; i < ehdr->e_phnum; i++) {
		u64 start = phdr[i].p_paddr;
		u64 end = start + phdr[i].p_memsz;
		bool mapped = false;

		if (phdr[i].p_type != PT_LOAD)
			return -EINVAL;
		if (phdr[i].p_filesz > phdr[i].p_memsz || end > BIT_ULL(32) ||
		    (phdr[i].p_flags & (QCOM_MDT_TYPE_MASK | QCOM_MDT_RELOCATABLE)))
			return -EINVAL;
		if (!phdr[i].p_memsz)
			continue;

		for (j = 0; j < num_regions; j++)
			if (start >= regions[j].addr &&
			    end <= (u64)regions[j].addr + regions[j].size) {
				mapped = true;
				break;
			}
		if (!mapped)
			return -EINVAL;
		for (j = 2; j < i; j++)
			if (phdr[j].p_memsz && start < (u64)phdr[j].p_paddr + phdr[j].p_memsz &&
			    phdr[j].p_paddr < end)
				return -EINVAL;
		has_load = true;
	}

	return has_load ? 0 : -EINVAL;
}

#endif

// SPDX-License-Identifier: GPL-2.0
/*
 * The ROM (0x40000000-0x5fffffff, read only to the cores) as a read-only disk, gvmrom: the device tree's
 * "gpuvm,rom" node, whose reg is the ROM's base and the size of the image in it. A read is a memcpy from the ROM into
 * the page, which from GVM_OP_MIN bytes up is the commit's copy operation: the core goes on at once and stops only
 * if it touches the page before the copy is made.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <asm/gvm.h>

struct gvm_rom {
	const u8 *base;
	sector_t sectors;
	struct gendisk *disk;
};

static int gvm_rom_major;

static void gvm_rom_submit_bio(struct bio *bio)
{
	struct gvm_rom *rom = bio->bi_bdev->bd_disk->private_data;
	sector_t sector = bio->bi_iter.bi_sector;
	struct bio_vec bvec;
	struct bvec_iter iter;

	if (bio_op(bio) != REQ_OP_READ) {
		bio_io_error(bio);
		return;
	}
	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = bvec.bv_len;

		if (sector + (len >> SECTOR_SHIFT) > rom->sectors) {
			bio_io_error(bio);
			return;
		}
		memcpy(page_address(bvec.bv_page) + bvec.bv_offset, rom->base + (sector << SECTOR_SHIFT), len);
		sector += len >> SECTOR_SHIFT;
	}
	bio_endio(bio);
}

static const struct block_device_operations gvm_rom_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= gvm_rom_submit_bio,
};

static int gvm_rom_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct gvm_rom *rom;
	int err;

	if (!res || resource_size(res) < SECTOR_SIZE)
		return -ENODEV;
	if (res->start < GVM_ROM || res->end >= GVM_ROM_END)
		return -EINVAL;

	rom = devm_kzalloc(&pdev->dev, sizeof(*rom), GFP_KERNEL);
	if (!rom)
		return -ENOMEM;
	rom->base = (const u8 *)res->start;
	rom->sectors = resource_size(res) >> SECTOR_SHIFT;

	rom->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!rom->disk)
		return -ENOMEM;
	rom->disk->major = gvm_rom_major;
	rom->disk->first_minor = 0;
	rom->disk->minors = 1;
	rom->disk->fops = &gvm_rom_fops;
	rom->disk->private_data = rom;
	strscpy(rom->disk->disk_name, "gvmrom", DISK_NAME_LEN);
	set_capacity(rom->disk, rom->sectors);
	set_disk_ro(rom->disk, true);
	blk_queue_logical_block_size(rom->disk->queue, SECTOR_SIZE);
	blk_queue_physical_block_size(rom->disk->queue, PAGE_SIZE);

	err = add_disk(rom->disk);
	if (err) {
		blk_cleanup_disk(rom->disk);
		return err;
	}
	platform_set_drvdata(pdev, rom);
	dev_info(&pdev->dev, "gvmrom: %llu KB at 0x%08lx\n",
		 (unsigned long long)resource_size(res) >> 10, (unsigned long)res->start);
	return 0;
}

static int gvm_rom_remove(struct platform_device *pdev)
{
	struct gvm_rom *rom = platform_get_drvdata(pdev);

	del_gendisk(rom->disk);
	blk_cleanup_disk(rom->disk);
	return 0;
}

static const struct of_device_id gvm_rom_of_match[] = {
	{ .compatible = "gpuvm,rom" },
	{ }
};
MODULE_DEVICE_TABLE(of, gvm_rom_of_match);

static struct platform_driver gvm_rom_driver = {
	.probe	= gvm_rom_probe,
	.remove	= gvm_rom_remove,
	.driver	= {
		.name		= "gvm-rom",
		.of_match_table	= gvm_rom_of_match,
	},
};

static int __init gvm_rom_init(void)
{
	int err;

	gvm_rom_major = register_blkdev(0, "gvmrom");
	if (gvm_rom_major < 0)
		return gvm_rom_major;
	err = platform_driver_register(&gvm_rom_driver);
	if (err)
		unregister_blkdev(gvm_rom_major, "gvmrom");
	return err;
}
module_init(gvm_rom_init);

static void __exit gvm_rom_exit(void)
{
	platform_driver_unregister(&gvm_rom_driver);
	unregister_blkdev(gvm_rom_major, "gvmrom");
}
module_exit(gvm_rom_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gpu_vm's ROM as a block device");

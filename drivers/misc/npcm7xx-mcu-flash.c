// SPDX-License-Identifier: GPL-2.0
/*
 * Description   : NPCM7xx MCU Flash Driver
 *
 * Copyright (C) 2020 Nuvoton Corporation
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/file.h>

#define DEVICE_NAME		"npcm7xx-mcu-flash"

/* Multiple Function Pin Selection */
#define MFSEL3_OFFSET	0x064
#define SMBSEL_MASK		1
#define SMBSEL_GPIO		0
#define SMBSEL_SMB		1

/* Lock and Fuse Bits */
#define FUSE_LOW		0
#define FUSE_HIGH		1
#define FUSE_EXTEND		2
#define LOCK			3

#define DELAY			20
#define MCU_SIGNATURE	0x0012941E

u8 flash_data_0[16 * 1024];	__aligned(4);
u8 flash_data_1[16 * 1024];	__aligned(4);
u8 eeprom_data[256];		__aligned(4);

enum MCU_PIN {
	PIN_CLK,
	PIN_MOSI,
	PIN_MISO,
	PIN_RESET,
	PIN_TOTAL,
};

static DEFINE_SPINLOCK(mcu_file_lock);

struct npcm7xx_mcu {
	struct device		*dev;
	struct miscdevice	miscdev;
	struct gpio_desc	*pins[PIN_TOTAL];
	struct regmap		*gcr_regmap;
	bool				is_open;
	u32					dev_num;
	u32					smb_offset;
	u32					mfsel_offset;
};

static u8 spi_send_receive_byte(struct npcm7xx_mcu *mcu, u8 data_to_send)
{
	int bit_count, delay;
	u8 spi_mosi, spi_miso;
	u8 data_received = 0;

	for (bit_count = 0; bit_count < 8; bit_count++) {
		/* New SPI MOSI */
		spi_mosi = (data_to_send >> (7 - bit_count)) & 0x01;
		if (spi_mosi == 0)
			gpiod_set_value(mcu->pins[PIN_MOSI], 0);
		else
			gpiod_set_value(mcu->pins[PIN_MOSI], 1);

		/* Read SPI MISO */
		spi_miso = gpiod_get_value(mcu->pins[PIN_MISO]);
		data_received |= spi_miso << (7 - bit_count);

		for (delay = 0; delay < DELAY; delay++)
			spi_miso = gpiod_get_value(mcu->pins[PIN_MISO]);

		gpiod_set_value(mcu->pins[PIN_CLK], 1);

		for (delay = 0; delay < DELAY; delay++)
			spi_miso = gpiod_get_value(mcu->pins[PIN_MISO]);

		gpiod_set_value(mcu->pins[PIN_CLK], 0);
	}

	return (data_received);
}

static bool npcm7xx_mcu_sync(struct npcm7xx_mcu *mcu)
{
	u8 sync;

	spi_send_receive_byte(mcu, 0xAC);
	spi_send_receive_byte(mcu, 0x53);
	sync = spi_send_receive_byte(mcu, 0x00);
	spi_send_receive_byte(mcu, 0x00);

	if (sync == 0x53)
		return true;
	else
		return false;
}

static bool npcm7xx_mcu_ready(struct npcm7xx_mcu *mcu)
{
	u8 data;

	spi_send_receive_byte(mcu, 0xF0);
	spi_send_receive_byte(mcu, 0x00);
	spi_send_receive_byte(mcu, 0x00);
	data = spi_send_receive_byte(mcu, 0x00);

	if ((data & 0x01) == 0x01)
		return false;
	else
		return true;
}

static void npcm7xx_mcu_erase(struct npcm7xx_mcu *mcu)
{
	/* Need to wait for RDY */
	spi_send_receive_byte(mcu, 0xAC);
	spi_send_receive_byte(mcu, 0x80);
	spi_send_receive_byte(mcu, 0x00);
	spi_send_receive_byte(mcu, 0x00);
}

static u8 read_flash_memory_byte(struct npcm7xx_mcu *mcu, u16 addr)
{
	u8 data;

	if ((addr & 0x01) == 0x00) {
		/* Read Program Memory, Low byte */
		spi_send_receive_byte(mcu, 0x20);
	} else {
		/* Read Program Memory, High byte */
		spi_send_receive_byte(mcu, 0x28);
	}

	spi_send_receive_byte(mcu, addr >> 9);
	spi_send_receive_byte(mcu, addr >> 1);
	data = spi_send_receive_byte(mcu, 0x00);
	return data;
}

static void read_flash_memory_block(struct npcm7xx_mcu *mcu, u16 addr,
				    u16 length_in_byte, u8 *pdata)
{
	while (length_in_byte != 0) {
		*pdata = read_flash_memory_byte(mcu, addr);
		addr++;
		pdata++;
		length_in_byte--;
	}
}

static int write_flash_memory_page(struct npcm7xx_mcu *mcu, u16 addr, u8 *pdata)
{
	int i;

	/* Address must be aline to word */
	if ((addr & 0x01) != 0)
		return -1;

	addr &= 0xFFFE;

	/* Load Flash Memory Page */
	for (i = 0; i < 16; i++) {
		/* Load Program Memory Page, Low byte of word */
		spi_send_receive_byte(mcu, 0x40);
		spi_send_receive_byte(mcu, 0x00);
		spi_send_receive_byte(mcu, i);
		spi_send_receive_byte(mcu, *pdata);
		pdata++;

		/* Load Program Memory Page, High byte of word */
		spi_send_receive_byte(mcu, 0x48);
		spi_send_receive_byte(mcu, 0x00);
		spi_send_receive_byte(mcu, i);
		spi_send_receive_byte(mcu, *pdata);
		pdata++;
	}

	/* Write Program Memory Page */
	spi_send_receive_byte(mcu, 0x4C);
	spi_send_receive_byte(mcu, addr >> 9);
	spi_send_receive_byte(mcu, addr >> 1);
	spi_send_receive_byte(mcu, 0x00);
	while (npcm7xx_mcu_ready(mcu) == false)
		;

	return 0;
}

static int write_flash_memory_block(struct npcm7xx_mcu *mcu,
				    u16 addr,
				    u16 length_in_byte,
				    u8 *pdata)
{
	/* Address must be aline to word */
	if ((addr & 0x01) != 0)
		return -1;

	/* length_in_byte must be aline to Page size 16 words / 32 bytes */
	if ((length_in_byte & 0x1F) != 0)
		return -1;

	addr &= 0xFFFE;
	length_in_byte &= 0xFFE0;

	while (length_in_byte != 0) {
		write_flash_memory_page(mcu, addr, pdata);
		addr += 32;
		pdata += 32;
		length_in_byte -= 32;
	}

	return 0;
}

static int memory_compare(const void *ptr1, const void *ptr2, u32 size_in_byte)
{
	u32 offset = 0;

	while (size_in_byte != 0) {
		if (*((u8 *)ptr1) != *((u8 *)ptr2))
			return (offset);

		ptr1 = (u8 *)ptr1 + 1;
		ptr2 = (u8 *)ptr2 + 1;
		offset++;
		size_in_byte--;
	}
	return 0;
}

static void memory_dump_byte(u32 addr, u32 display_address, u16 num_of_lines)
{
	u16 line;
	u8 *pdata8 = (u8 *)addr;
	u8 i, data8;

	for (line = 0; line < num_of_lines; line++) {
		display_address += 16;
		for (i = 0; i < 16; i++)
			data8 = *pdata8++;
	}
}

static u8 read_rom_table(struct npcm7xx_mcu *mcu, u8 addr)
{
	u8 data;

	if ((addr & 0x01) == 0x00) {
		/* Read Signature Byte a.k.a. ROM Table, Low byte */
		spi_send_receive_byte(mcu, 0x30);
	} else {
		/* Read Calibration Byte a.k.a. ROM Table, High byte */
		spi_send_receive_byte(mcu, 0x38);
	}

	spi_send_receive_byte(mcu, 0x00);
	spi_send_receive_byte(mcu, addr >> 1);
	data = spi_send_receive_byte(mcu, 0x00);
	return data;
}

static bool npcm7xx_mcu_signature(struct npcm7xx_mcu *mcu)
{
	u32 device_signature = 0;

	device_signature |= (u32)read_rom_table(mcu, 0) << 0;
	device_signature |= (u32)read_rom_table(mcu, 2) << 8;
	device_signature |= (u32)read_rom_table(mcu, 4) << 16;
	if (device_signature == MCU_SIGNATURE)
		return true;
	else
		return false;
}

/* Configure mcu pins as GPIO function */
static inline void npcm7xx_mcu_config_gpio(struct npcm7xx_mcu *mcu)
{
	int val;

	val = SMBSEL_GPIO;
	regmap_update_bits(mcu->gcr_regmap,
			mcu->mfsel_offset,
			(SMBSEL_MASK << mcu->smb_offset),
			(val << mcu->smb_offset));
}

/* Configure mcu pins as SMB function */
static inline void npcm7xx_mcu_config_smb(struct npcm7xx_mcu *mcu)
{
	int val;

	val = SMBSEL_SMB;
	regmap_update_bits(mcu->gcr_regmap,
			mcu->mfsel_offset,
			(SMBSEL_MASK << mcu->smb_offset),
			(val << mcu->smb_offset));
}

static int npcm7xx_mcu_init(struct npcm7xx_mcu *mcu)
{
	int i = 0;

	/* Initialize pins to gpio function */
	npcm7xx_mcu_config_gpio(mcu);

	/* Keep SCL and SDA low */
	gpiod_direction_output(mcu->pins[PIN_CLK], 0);
	udelay(1);
	gpiod_direction_output(mcu->pins[PIN_MOSI], 0);

	gpiod_direction_output(mcu->pins[PIN_CLK], 0);

	/* Set RESET high */
	gpiod_direction_output(mcu->pins[PIN_RESET], 1);
	mdelay(5);

	/* Set RESET low */
	gpiod_direction_output(mcu->pins[PIN_RESET], 0);
	mdelay(20);

	if (npcm7xx_mcu_sync(mcu)) {
		dev_info(mcu->dev, "MCU F/W Check Sync ............... DONE\n");
	} else {
		dev_info(mcu->dev, "MCU F/W Check Sync ............... FAIL\n");
		return -1;
	}

	for (i = 0; i < 128; i++)
		eeprom_data[i] = read_rom_table(mcu, i);
	//memory_dump_byte((u32)eeprom_data, 0, 128/16);

	if (npcm7xx_mcu_signature(mcu) == true) {
		dev_info(mcu->dev, "MCU F/W Check Device Signature ... DONE\n");
	} else {
		dev_info(mcu->dev, "MCU F/W Check Device Signature ... FAIL\n");
		return -1;
	}

	dev_info(mcu->dev, "MCU F/W Chip Erase ..");
	npcm7xx_mcu_erase(mcu);
	while (npcm7xx_mcu_ready(mcu) == false)
		dev_info(mcu->dev, ".");
	dev_info(mcu->dev, "DONE\n");

	return 0;
}

static ssize_t npcm7xx_mcu_write(struct file *file,
				 const char __user *buf,
				 size_t count,
				 loff_t *ppos)
{
	u16 offset = 0;

	struct npcm7xx_mcu *mcu = file->private_data;
	void __user *argp = (void __user *)buf;

	if (copy_from_user(flash_data_1, argp, count)) {
		dev_info(mcu->dev, "copy_from_user FAIL");
		return -EFAULT;
	}

	if (npcm7xx_mcu_init(mcu) != 0)
		return -EFAULT;

	dev_info(mcu->dev, "MCU F/W Program Flash Memory ..... ");
	if (write_flash_memory_block(mcu, offset, count, flash_data_1) != 0)
		return -EFAULT;
	dev_info(mcu->dev, "DONE\n");

	dev_info(mcu->dev, "MCU F/W Read Flash Memory ........ ");
	read_flash_memory_block(mcu, offset, count, flash_data_0);
	dev_info(mcu->dev, "DONE\n");
	//memory_dump_byte((u32)flash_data_0, offset, count/16);

	dev_info(mcu->dev, "MCU F/W Compare Flash Memory ..... ");
	if (memory_compare(flash_data_1, flash_data_0, count) != 0)
		dev_info(mcu->dev, "FAIL\n");
	else
		dev_info(mcu->dev, "DONE\n");

	npcm7xx_mcu_config_smb(mcu);

	// Set RESET high
	gpiod_direction_output(mcu->pins[PIN_RESET], 1);

	return count;
}

static int npcm7xx_mcu_open(struct inode *inode, struct file *file)
{
	struct npcm7xx_mcu *mcu;

	mcu = container_of(file->private_data, struct npcm7xx_mcu, miscdev);

	spin_lock(&mcu_file_lock);
	if (mcu->is_open) {
		spin_unlock(&mcu_file_lock);
		dev_info(mcu->dev, "EBUSY");
		return -EBUSY;
	}

	mcu->is_open = true;
	file->private_data = mcu;

	spin_unlock(&mcu_file_lock);
	return 0;
}

static int npcm7xx_mcu_release(struct inode *inode, struct file *file)
{
	struct npcm7xx_mcu *mcu = file->private_data;

	spin_lock(&mcu_file_lock);
	mcu->is_open = false;
	spin_unlock(&mcu_file_lock);

	return 0;
}

const struct file_operations npcm7xx_mcu_fops = {
	.open              = npcm7xx_mcu_open,
	.write             = npcm7xx_mcu_write,
	.release           = npcm7xx_mcu_release,
};

static int npcm7xx_mcu_register_device(struct npcm7xx_mcu *mcu)
{
	struct device *dev = mcu->dev;
	int err;

	if (!dev)
		return -ENODEV;

	/* register miscdev */
	mcu->miscdev.parent = dev;
	mcu->miscdev.fops = &npcm7xx_mcu_fops;
	mcu->miscdev.minor = MISC_DYNAMIC_MINOR;
	mcu->miscdev.name = kasprintf(GFP_KERNEL, "mcu%d", mcu->dev_num);
	if (!mcu->miscdev.name)
		return -ENOMEM;

	err = misc_register(&mcu->miscdev);
	if (err) {
		dev_err(mcu->miscdev.parent,
			"Unable to register device, err %d\n", err);
		kfree(mcu->miscdev.name);
		return err;
	}

	return 0;
}

static int npcm7xx_mcu_probe(struct platform_device *pdev)
{
	struct npcm7xx_mcu *mcu;
	struct gpio_desc *gpiod;
	struct device_node *np = pdev->dev.of_node;
	u32 value;
	int i, ret;

	enum gpiod_flags pin_flags[PIN_TOTAL] = {
		GPIOD_OUT_HIGH, GPIOD_OUT_HIGH, GPIOD_IN, GPIOD_OUT_HIGH,
	};

	dev_info(&pdev->dev, "%s", __func__);

	mcu = kzalloc(sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;
	mcu->dev = &pdev->dev;

	if (of_device_is_compatible(pdev->dev.of_node, "nuvoton,npcm750-mcu-flash"))
		mcu->gcr_regmap =
			syscon_regmap_lookup_by_compatible("nuvoton,npcm750-gcr");
	if (of_device_is_compatible(pdev->dev.of_node, "nuvoton,npcm845-mcu-flash"))
		mcu->gcr_regmap =
			syscon_regmap_lookup_by_compatible("nuvoton,npcm845-gcr");
	if (IS_ERR(mcu->gcr_regmap)) {
		dev_err(&pdev->dev, "Failed to find gcr\n");
		ret = PTR_ERR(mcu->gcr_regmap);
		goto err;
	}

	/* mcu pins */
	for (i = 0; i < PIN_TOTAL; i++) {
		gpiod = gpiod_get_index(&pdev->dev, "mcu", i, pin_flags[i]);
		if (IS_ERR(gpiod)) {
			dev_err(&pdev->dev, "No mcu pin: %d", i);
			return PTR_ERR(gpiod);
		}
		mcu->pins[i] = gpiod;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "dev-num", &value);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not read dev-num\n");
		value = 0;
	}
	mcu->dev_num = value;

	ret = of_property_read_u32(pdev->dev.of_node, "mfsel-offset", &value);
	if (ret < 0) {
		dev_info(&pdev->dev, "use MFSEL3\n");
		value = MFSEL3_OFFSET;
	}
	mcu->mfsel_offset = value;

	ret = of_property_read_u32(pdev->dev.of_node, "smb-offset", &value);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not read smb-offset\n");
		value = 0;
	}
	mcu->smb_offset = value;

	ret = npcm7xx_mcu_register_device(mcu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create device\n");
		goto err;
	}
	platform_set_drvdata(pdev, mcu);

	return 0;

err:
	kfree(mcu);
	return ret;
}

static int npcm7xx_mcu_remove(struct platform_device *pdev)
{
	struct npcm7xx_mcu *mcu = platform_get_drvdata(pdev);
	int i;

	if (!mcu)
		return 0;

	misc_deregister(&mcu->miscdev);
	kfree(mcu->miscdev.name);
	for (i = 0; i < PIN_TOTAL; i++)
		gpiod_put(mcu->pins[i]);
	kfree(mcu);

	return 0;
}

static const struct of_device_id npcm7xx_mcu_match[] = {
	{ .compatible = "nuvoton,npcm750-mcu-flash", },
	{ .compatible = "nuvoton,npcm845-mcu-flash", },
	{},
};

static struct platform_driver npcm7xx_mcu_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.owner		= THIS_MODULE,
		.of_match_table = npcm7xx_mcu_match,
	},
	.probe = npcm7xx_mcu_probe,
	.remove = npcm7xx_mcu_remove,
};

module_platform_driver(npcm7xx_mcu_driver);

MODULE_DEVICE_TABLE(of, npcm7xx_mcu_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tim Lee <chli30@nuvoton.com>");
MODULE_DESCRIPTION("NPCM7xx MCU Flash Driver");

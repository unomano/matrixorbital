/*
 * Driver for the Matrix Orbital GLK19264 LCD controller
 *
 * Copyright 2019 Viktar Palstsiuk
 *
 * Licensed under the GPLv2 or later.
 *
 */

#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input-polldev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define MATRIXORBITAL_POLL_KEY_PRESS	0x26
#define MATRIXORBITAL_READ_MODULE_TYPE 0x37
#define MATRIXORBITAL_AUTO_TX_KEY_PRESS_OFF 0x4F
#define MATRIXORBITAL_CLEAR_SCREEN 0x58
#define MATRIXORBITAL_DRAW_BITMAP_DIRECTLY 0x64
#define MATRIXORBITAL_TX_PROTOCOL_SELECT 0xA0

static u_int refreshrate = 5;
module_param(refreshrate, uint, 0);

struct matrixorbital_par {
	struct i2c_client *client;
	u32 width;
	u32 height;
	struct fb_info *info;
	struct input_polled_dev	*idev;
};

static const struct fb_fix_screeninfo matrixorbitalfb_fix = {
	.id		= "MatOrb GLK19264",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_MONO10,
	.xpanstep	= 0,
	.ypanstep	= 0,
	.ywrapstep	= 0,
	.accel		= FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo matrixorbitalfb_var = {
	.bits_per_pixel	= 1,
};

static int matrixorbital_write_array(struct i2c_client *client, u8 *buf, u32 len)
{
	int ret;

	ret = i2c_master_send(client, buf, len);
	if (ret != len) {
		dev_err(&client->dev, "Couldn't send I2C command.\n");
		return -1;
	}

	return 0;
}

static int matrixorbital_write_cmd(struct i2c_client *client, u8 cmd)
{
	u8 data[2];
	data[0] = 0xFE;
	data[1] = cmd;
	return matrixorbital_write_array(client, data, sizeof(data));
}

static int matrixorbital_write_param(struct i2c_client *client, u8 cmd, u8 value)
{
	u8 data[3];
	data[0] = 0xFE;
	data[1] = cmd;
	data[2] = value;
	return matrixorbital_write_array(client, data, sizeof(data));
}

static int matrixorbital_read_param(struct i2c_client *client, u8 cmd)
{
	u8 data;

	int ret;
	matrixorbital_write_cmd(client, cmd);
	msleep(5);
	ret = i2c_master_recv(client, &data, sizeof(data));
	return (ret == 1) ? data : ret;
}

static unsigned char reverse_bits_in_byte(unsigned char b)
{
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

static void matrixorbitalfb_update_display(struct matrixorbital_par *par)
{
	u8 *vmem = par->info->screen_base;
	int i;
	int len = 6 + par->width * par->height / 8;

	u8 *data = kzalloc(len, GFP_KERNEL);
	if (!data)
		return;

	data[0] = 0xFE;
	data[1] = MATRIXORBITAL_DRAW_BITMAP_DIRECTLY;
	data[2] = 0;
	data[3] = 0;
	data[4] = par->width;
	data[5] = par->height;

	for (i = 0; i < (par->height * par->width / 8); i++)
		data[i+6] = reverse_bits_in_byte(*(vmem + i));

	matrixorbital_write_array(par->client, data, len);
	kfree(data);
}

static ssize_t matrixorbitalfb_write(struct fb_info *info, const char __user *buf,
									 size_t count, loff_t *ppos)
{
	struct matrixorbital_par *par = info->par;
	unsigned long total_size;
	unsigned long p = *ppos;
	u8 __iomem *dst;

	total_size = info->fix.smem_len;

	if (p > total_size)
		return -EINVAL;

	if (count + p > total_size)
		count = total_size - p;

	if (!count)
		return -EINVAL;

	dst = (void __force *) (info->screen_base + p);

	if (copy_from_user(dst, buf, count))
		return -EFAULT;

	matrixorbitalfb_update_display(par);

	*ppos += count;

	return count;
}

static int matrixorbitalfb_blank(int blank_mode, struct fb_info *info)
{
	return 0;
}

static void matrixorbitalfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct matrixorbital_par *par = info->par;
	sys_fillrect(info, rect);
	matrixorbitalfb_update_display(par);
}

static void matrixorbitalfb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	struct matrixorbital_par *par = info->par;
	sys_copyarea(info, area);
	matrixorbitalfb_update_display(par);
}

static void matrixorbitalfb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	struct matrixorbital_par *par = info->par;
	sys_imageblit(info, image);
	matrixorbitalfb_update_display(par);
}

static struct fb_ops matrixorbitalfb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= matrixorbitalfb_write,
	.fb_blank	= matrixorbitalfb_blank,
	.fb_fillrect	= matrixorbitalfb_fillrect,
	.fb_copyarea	= matrixorbitalfb_copyarea,
	.fb_imageblit	= matrixorbitalfb_imageblit,
};

static void matrixorbitalfb_deferred_io(struct fb_info *info,
										struct list_head *pagelist)
{
	matrixorbitalfb_update_display(info->par);
}

static int matrixorbital_init(struct matrixorbital_par *par)
{
	int ret;

	/* Use I2C for TX */
	matrixorbital_write_param(par->client, MATRIXORBITAL_TX_PROTOCOL_SELECT, 0);

	/* Read model */
	ret = matrixorbital_read_param(par->client, MATRIXORBITAL_READ_MODULE_TYPE);
	dev_err(&par->client->dev, "Module type 0x%02x\n", ret);

	/* Enable keypad poll mode */
	ret = matrixorbital_write_cmd(par->client, MATRIXORBITAL_AUTO_TX_KEY_PRESS_OFF);

	/* Clear the screen */
	ret = matrixorbital_write_cmd(par->client, MATRIXORBITAL_CLEAR_SCREEN);

	if (ret < 0)
		return ret;

	return 0;
}

static void matrixorbital_report_key(struct input_dev *input, unsigned matrixorbital_keycode)
{
	u8 keycode = 0;

	switch(matrixorbital_keycode) {
	case 0x41:
		keycode = KEY_ESC;
		break;
	case 0x42:
		keycode = KEY_UP;
		break;
	case 0x43:
		keycode = KEY_RIGHT;
		break;
	case 0x44:
		keycode = KEY_LEFT;
		break;
	case 0x45:
		keycode = KEY_ENTER;
		break;
	case 0x47:
		keycode = KEY_BACKSPACE;
		break;
	case 0x48:
		keycode = KEY_DOWN;
		break;
	default:
		dev_err(&input->dev, "Unknown keycode 0x%x\n", matrixorbital_keycode);
		break;
	}

	dev_err(&input->dev, "Report key %d [0x%x]\n", keycode, matrixorbital_keycode);

	input_report_key(input, keycode, 1);
	input_sync(input);

	input_report_key(input, keycode, 0);
	input_sync(input);
}

static void matrixorbital_keypad_poll(struct input_polled_dev *ipdev)
{
	struct matrixorbital_par *par = ipdev->private;
	int ret;

	do {
		ret = matrixorbital_read_param(par->client, MATRIXORBITAL_POLL_KEY_PRESS);
		if (ret < 0)
			return;

		if (!ret)
			return;

		matrixorbital_report_key(par->idev->input, ret & 0x7F);
	} while (ret & 0x80);
}

static int matrixorbital_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct fb_info *info;
	struct fb_deferred_io *matrixorbitalfb_defio;
	u32 vmem_size;
	struct matrixorbital_par *par;
	u8 *vmem;
	int ret;
	struct input_polled_dev *keypad_dev;

	info = framebuffer_alloc(sizeof(struct matrixorbital_par), &client->dev);
	if (!info) {
		dev_err(&client->dev, "Couldn't allocate framebuffer.\n");
		ret = -ENOMEM;
		return ret;
	}

	par = info->par; // info->par was allocated by framebuffer_alloc()

	par->client = client;
	par->info = info;
	par->width = 192;
	par->height = 64;

	vmem_size = par->width * par->height / 8;

	vmem = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
									get_order(vmem_size));
	if (!vmem) {
		dev_err(&client->dev, "Couldn't allocate graphical memory.\n");
		ret = -ENOMEM;
		goto fb_alloc_error;
	}

	matrixorbitalfb_defio = devm_kzalloc(&client->dev, sizeof(*matrixorbitalfb_defio),
										 GFP_KERNEL);
	if (!matrixorbitalfb_defio) {
		dev_err(&client->dev, "Couldn't allocate deferred io.\n");
		ret = -ENOMEM;
		goto fb_alloc_error;
	}

	matrixorbitalfb_defio->delay = HZ / refreshrate;
	matrixorbitalfb_defio->deferred_io = matrixorbitalfb_deferred_io;

	info->fbops = &matrixorbitalfb_ops;
	info->fix = matrixorbitalfb_fix;
	info->fix.line_length = par->width / 8;
	info->fbdefio = matrixorbitalfb_defio;

	info->var = matrixorbitalfb_var;
	info->var.xres = par->width;
	info->var.xres_virtual = par->width;
	info->var.yres = par->height;
	info->var.yres_virtual = par->height;

	info->var.red.length = 1;
	info->var.red.offset = 0;
	info->var.green.length = 1;
	info->var.green.offset = 0;
	info->var.blue.length = 1;
	info->var.blue.offset = 0;

	info->screen_base = (u8 __force __iomem *)vmem;
	info->fix.smem_start = __pa(vmem);
	info->fix.smem_len = vmem_size;

	fb_deferred_io_init(info);

	i2c_set_clientdata(client, info);

	ret = matrixorbital_init(par);
	if (ret)
		goto panel_init_error;

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(&client->dev, "Couldn't register the framebuffer\n");
		goto panel_init_error;
	}

	/* Keypad */
	keypad_dev = devm_input_allocate_polled_device(&client->dev);
	if (!keypad_dev) {
		printk(KERN_ERR "Not enough memory\n");
		return -ENOMEM;
	}

	keypad_dev->input->evbit[0] = BIT_MASK(EV_KEY);
	keypad_dev->input->keybit[BIT_WORD(KEY_ESC)] |= BIT_MASK(KEY_ESC);
	keypad_dev->input->keybit[BIT_WORD(KEY_BACKSPACE)] |= BIT_MASK(KEY_BACKSPACE);
	keypad_dev->input->keybit[BIT_WORD(KEY_ENTER)] |= BIT_MASK(KEY_ENTER);
	keypad_dev->input->keybit[BIT_WORD(KEY_UP)] |= BIT_MASK(KEY_UP);
	keypad_dev->input->keybit[BIT_WORD(KEY_LEFT)] |= BIT_MASK(KEY_LEFT);
	keypad_dev->input->keybit[BIT_WORD(KEY_RIGHT)] |= BIT_MASK(KEY_RIGHT);
	keypad_dev->input->keybit[BIT_WORD(KEY_DOWN)] |= BIT_MASK(KEY_DOWN);

	keypad_dev->input->name = "matorb-keypad";
	keypad_dev->input->id.bustype = BUS_I2C;
	keypad_dev->poll = matrixorbital_keypad_poll;
	keypad_dev->poll_interval = 500;
	keypad_dev->poll_interval_max = 1000;

	keypad_dev->private	= par;
	par->idev = keypad_dev;

	ret = input_register_polled_device(keypad_dev);
	if (ret) {
		dev_err(&client->dev, "failed to register polled input device\n");
		goto err_free_dev;
	}

	dev_info(&client->dev, "fb%d: %s framebuffer device registered, using %d bytes of video memory\n", info->node, info->fix.id, vmem_size);

	return 0;

err_free_dev:
	input_free_polled_device(keypad_dev);
panel_init_error:
	fb_deferred_io_cleanup(info);
fb_alloc_error:
	framebuffer_release(info);
	return ret;
}

static int matrixorbital_remove(struct i2c_client *client)
{
	struct fb_info *info = i2c_get_clientdata(client);
	struct matrixorbital_par *par = info->par;

	input_unregister_polled_device(par->idev);
	input_free_polled_device(par->idev);

	matrixorbital_write_cmd(par->client, MATRIXORBITAL_CLEAR_SCREEN);

	unregister_framebuffer(info);

	fb_deferred_io_cleanup(info);
	__free_pages(__va(info->fix.smem_start), get_order(info->fix.smem_len));
	framebuffer_release(info);

	return 0;
}

static const struct i2c_device_id matrixorbital_i2c_id[] = {
	{ "matrixorbital", 0 },
	{ }
};

static struct i2c_driver matrixorbital_driver = {
	.probe = matrixorbital_probe,
	.remove = matrixorbital_remove,
	.id_table = matrixorbital_i2c_id,
	.driver = {
		.name = "matrixorbital",
	},
};

module_i2c_driver(matrixorbital_driver);

MODULE_DESCRIPTION("FB driver for the Matrix Orbital GLK19264 LCD controller");
MODULE_AUTHOR("Viktar Palstsiuk <viktar.palstsiuk@promwad.com>");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu display as a framebuffer device (docs/display.md in the ShaderEmu repository).
 * Opening the device or setting a mode switches the display to 32-bit pixels in RAM.
 */
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>

#define DISP_PHYS	0x87000000UL
#define DISP_PIXELS	0x1000UL
#define DISP_LEN	0x003ff000UL	/* pixel memory, up to the GPU's buffers */
#define DISP_MODE_RGB32	1

static void __iomem *disp_regs;
static u32 pseudo_palette[16];

static int shaderemu_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres < 16 || var->yres < 16 || var->xres > 2048 || var->yres > 2048 ||
	    var->xres * var->yres * 4 > DISP_LEN)
		return -EINVAL;
	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;
	var->xoffset = var->yoffset = 0;
	var->bits_per_pixel = 32;
	var->red = (struct fb_bitfield){ .offset = 16, .length = 8 };
	var->green = (struct fb_bitfield){ .offset = 8, .length = 8 };
	var->blue = (struct fb_bitfield){ .offset = 0, .length = 8 };
	var->transp = (struct fb_bitfield){ .offset = 24, .length = 0 };
	return 0;
}

static int shaderemu_fb_set_par(struct fb_info *info)
{
	info->fix.line_length = info->var.xres * 4;
	writel(info->var.xres, disp_regs + 4);
	writel(info->var.yres, disp_regs + 8);
	writel(DISP_MODE_RGB32, disp_regs);
	return 0;
}

static int shaderemu_fb_open(struct fb_info *info, int user)
{
	return shaderemu_fb_set_par(info);
}

static int shaderemu_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
				  unsigned int blue, unsigned int transp, struct fb_info *info)
{
	if (regno >= 16)
		return -EINVAL;
	pseudo_palette[regno] = ((red >> 8) << 16) | ((green >> 8) << 8) | (blue >> 8);
	return 0;
}

static const struct fb_ops shaderemu_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_open	= shaderemu_fb_open,
	.fb_check_var	= shaderemu_fb_check_var,
	.fb_set_par	= shaderemu_fb_set_par,
	.fb_setcolreg	= shaderemu_fb_setcolreg,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

static int __init shaderemu_fb_init(void)
{
	struct fb_info *info;
	int ret;

	if (pfn_valid(DISP_PHYS >> PAGE_SHIFT))
		return -ENODEV;	/* this range is RAM here: not our device tree */
	disp_regs = ioremap(DISP_PHYS, PAGE_SIZE);
	info = framebuffer_alloc(0, NULL);
	if (!disp_regs || !info)
		return -ENOMEM;
	info->screen_base = ioremap(DISP_PHYS + DISP_PIXELS, DISP_LEN);
	if (!info->screen_base)
		return -ENOMEM;
	strscpy(info->fix.id, "shaderemu", sizeof(info->fix.id));
	info->fix.smem_start = DISP_PHYS + DISP_PIXELS;
	info->fix.smem_len = DISP_LEN;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->var.xres = 640;
	info->var.yres = 480;
	shaderemu_fb_check_var(&info->var, info);
	info->fix.line_length = info->var.xres * 4;
	info->fbops = &shaderemu_fb_ops;
	info->pseudo_palette = pseudo_palette;
	info->flags = FBINFO_FLAG_DEFAULT;
	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		return ret;
	ret = register_framebuffer(info);
	if (!ret)
		pr_info("shaderemu_fb: fb%d, pixels at 0x%lx\n", info->node, info->fix.smem_start);
	return ret;
}
device_initcall(shaderemu_fb_init);

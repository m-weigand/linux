// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/iio/consumer.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/firmware.h>
#include <linux/delay.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/rockchip_ebc_drm.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_framebuffer.h>

#define EBC_DSP_START			0x0000
#define EBC_DSP_START_DSP_OUT_LOW		BIT(31)
#define EBC_DSP_START_DSP_SDCE_WIDTH(x)		((x) << 16)
#define EBC_DSP_START_DSP_EINK_MODE		BIT(13)
#define EBC_DSP_START_SW_BURST_CTRL		BIT(12)
#define EBC_DSP_START_DSP_FRM_TOTAL(x)		((x) << 2)
#define EBC_DSP_START_DSP_RST			BIT(1)
#define EBC_DSP_START_DSP_FRM_START		BIT(0)
#define EBC_EPD_CTRL			0x0004
#define EBC_EPD_CTRL_EINK_MODE_SWAP		BIT(31)
#define EBC_EPD_CTRL_DSP_GD_END(x)		((x) << 16)
#define EBC_EPD_CTRL_DSP_GD_ST(x)		((x) << 8)
#define EBC_EPD_CTRL_DSP_THREE_WIN_MODE		BIT(7)
#define EBC_EPD_CTRL_DSP_SDDW_MODE		BIT(6)
#define EBC_EPD_CTRL_EPD_AUO			BIT(5)
#define EBC_EPD_CTRL_EPD_PWR(x)			((x) << 2)
#define EBC_EPD_CTRL_EPD_GDRL			BIT(1)
#define EBC_EPD_CTRL_EPD_SDSHR			BIT(0)
#define EBC_DSP_CTRL			0x0008
#define EBC_DSP_CTRL_DSP_SWAP_MODE(x)		((x) << 30)
#define EBC_DSP_CTRL_DSP_DIFF_MODE		BIT(29)
#define EBC_DSP_CTRL_DSP_LUT_MODE		BIT(28)
#define EBC_DSP_CTRL_DSP_VCOM_MODE		BIT(27)
#define EBC_DSP_CTRL_DSP_GDOE_POL		BIT(26)
#define EBC_DSP_CTRL_DSP_GDSP_POL		BIT(25)
#define EBC_DSP_CTRL_DSP_GDCLK_POL		BIT(24)
#define EBC_DSP_CTRL_DSP_SDCE_POL		BIT(23)
#define EBC_DSP_CTRL_DSP_SDOE_POL		BIT(22)
#define EBC_DSP_CTRL_DSP_SDLE_POL		BIT(21)
#define EBC_DSP_CTRL_DSP_SDCLK_POL		BIT(20)
#define EBC_DSP_CTRL_DSP_SDCLK_DIV(x)		((x) << 16)
#define EBC_DSP_CTRL_DSP_BACKGROUND(x)		((x) << 0)
#define EBC_DSP_HTIMING0		0x000c
#define EBC_DSP_HTIMING0_DSP_HTOTAL(x)		((x) << 16)
#define EBC_DSP_HTIMING0_DSP_HS_END(x)		((x) << 0)
#define EBC_DSP_HTIMING1		0x0010
#define EBC_DSP_HTIMING1_DSP_HACT_END(x)	((x) << 16)
#define EBC_DSP_HTIMING1_DSP_HACT_ST(x)		((x) << 0)
#define EBC_DSP_VTIMING0		0x0014
#define EBC_DSP_VTIMING0_DSP_VTOTAL(x)		((x) << 16)
#define EBC_DSP_VTIMING0_DSP_VS_END(x)		((x) << 0)
#define EBC_DSP_VTIMING1		0x0018
#define EBC_DSP_VTIMING1_DSP_VACT_END(x)	((x) << 16)
#define EBC_DSP_VTIMING1_DSP_VACT_ST(x)		((x) << 0)
#define EBC_DSP_ACT_INFO		0x001c
#define EBC_DSP_ACT_INFO_DSP_HEIGHT(x)		((x) << 16)
#define EBC_DSP_ACT_INFO_DSP_WIDTH(x)		((x) << 0)
#define EBC_WIN_CTRL			0x0020
#define EBC_WIN_CTRL_WIN2_FIFO_THRESHOLD(x)	((x) << 19)
#define EBC_WIN_CTRL_WIN_EN			BIT(18)
#define EBC_WIN_CTRL_AHB_INCR_NUM_REG(x)	((x) << 13)
#define EBC_WIN_CTRL_AHB_BURST_REG(x)		((x) << 10)
#define EBC_WIN_CTRL_WIN_FIFO_THRESHOLD(x)	((x) << 2)
#define EBC_WIN_CTRL_WIN_FMT_Y4			(0x0 << 0)
#define EBC_WIN_CTRL_WIN_FMT_Y8			(0x1 << 0)
#define EBC_WIN_CTRL_WIN_FMT_XRGB8888		(0x2 << 0)
#define EBC_WIN_CTRL_WIN_FMT_RGB565		(0x3 << 0)
#define EBC_WIN_MST0			0x0024
#define EBC_WIN_MST1			0x0028
#define EBC_WIN_VIR			0x002c
#define EBC_WIN_VIR_WIN_VIR_HEIGHT(x)		((x) << 16)
#define EBC_WIN_VIR_WIN_VIR_WIDTH(x)		((x) << 0)
#define EBC_WIN_ACT			0x0030
#define EBC_WIN_ACT_WIN_ACT_HEIGHT(x)		((x) << 16)
#define EBC_WIN_ACT_WIN_ACT_WIDTH(x)		((x) << 0)
#define EBC_WIN_DSP			0x0034
#define EBC_WIN_DSP_WIN_DSP_HEIGHT(x)		((x) << 16)
#define EBC_WIN_DSP_WIN_DSP_WIDTH(x)		((x) << 0)
#define EBC_WIN_DSP_ST			0x0038
#define EBC_WIN_DSP_ST_WIN_DSP_YST(x)		((x) << 16)
#define EBC_WIN_DSP_ST_WIN_DSP_XST(x)		((x) << 0)
#define EBC_INT_STATUS			0x003c
#define EBC_INT_STATUS_DSP_FRM_INT_NUM(x)	((x) << 12)
#define EBC_INT_STATUS_LINE_FLAG_INT_CLR	BIT(11)
#define EBC_INT_STATUS_DSP_FRM_INT_CLR		BIT(10)
#define EBC_INT_STATUS_DSP_END_INT_CLR		BIT(9)
#define EBC_INT_STATUS_FRM_END_INT_CLR		BIT(8)
#define EBC_INT_STATUS_LINE_FLAG_INT_MSK	BIT(7)
#define EBC_INT_STATUS_DSP_FRM_INT_MSK		BIT(6)
#define EBC_INT_STATUS_DSP_END_INT_MSK		BIT(5)
#define EBC_INT_STATUS_FRM_END_INT_MSK		BIT(4)
#define EBC_INT_STATUS_LINE_FLAG_INT_ST		BIT(3)
#define EBC_INT_STATUS_DSP_FRM_INT_ST		BIT(2)
#define EBC_INT_STATUS_DSP_END_INT_ST		BIT(1)
#define EBC_INT_STATUS_FRM_END_INT_ST		BIT(0)
#define EBC_VCOM0			0x0040
#define EBC_VCOM1			0x0044
#define EBC_VCOM2			0x0048
#define EBC_VCOM3			0x004c
#define EBC_CONFIG_DONE			0x0050
#define EBC_CONFIG_DONE_REG_CONFIG_DONE		BIT(0)
#define EBC_VNUM			0x0054
#define EBC_VNUM_DSP_VCNT(x)			((x) << 16)
#define EBC_VNUM_LINE_FLAG_NUM(x)		((x) << 0)
#define EBC_WIN_MST2			0x0058
#define EBC_LUT_DATA			0x1000

#define EBC_FRAME_PENDING		(-1U)

#define EBC_MAX_PHASES			256

#define EBC_NUM_LUT_REGS		0x1000
#define EBC_NUM_SUPPLIES		3

#define EBC_FRAME_TIMEOUT		msecs_to_jiffies(25)
#define EBC_REFRESH_TIMEOUT		msecs_to_jiffies(3000)
#define EBC_SUSPEND_DELAY_MS		2000

#define EBC_FIRMWARE		"rockchip/ebc.wbf"
MODULE_FIRMWARE(EBC_FIRMWARE);

struct rockchip_ebc {
	struct clk			*dclk;
	struct clk			*hclk;
	struct completion		display_end;
	struct drm_crtc			crtc;
	struct drm_device		drm;
	struct drm_encoder		encoder;
	struct drm_epd_lut		lut;
	struct drm_epd_lut_file		lut_file;
	struct drm_plane		plane;
	struct iio_channel		*temperature_channel;
	struct regmap			*regmap;
	struct regulator_bulk_data	supplies[EBC_NUM_SUPPLIES];
	struct task_struct		*refresh_thread;
	u32				dsp_start;
	bool				lut_changed;
	bool				reset_complete;
	// one screen content: 1872 * 1404 / 2
	// the array size should probably be set dynamically...
	char off_screen[1314144];
	// before suspend we need to save the screen content so we can restore the
	// prev buffer after resuming
	char suspend_prev[1314144];
	char suspend_next[1314144];
	spinlock_t			refresh_once_lock;
	// should this go into the ctx?
	bool do_one_full_refresh;
	int waveform_at_beggining_of_update;
	// used to detect when we are suspending so we can do different things to
	// the ebc display depending on whether we are sleeping or suspending
	int suspend_was_requested;
};

static int default_waveform = DRM_EPD_WF_GC16;
module_param(default_waveform, int, 0644);
MODULE_PARM_DESC(default_waveform, "waveform to use for display updates");

static bool diff_mode = true;
module_param(diff_mode, bool, 0644);
MODULE_PARM_DESC(diff_mode, "only compute waveforms for changed pixels");

static bool direct_mode = false;
module_param(direct_mode, bool, 0444);
MODULE_PARM_DESC(direct_mode, "compute waveforms in software (software LUT)");

static bool panel_reflection = true;
module_param(panel_reflection, bool, 0644);
MODULE_PARM_DESC(panel_reflection, "reflect the image horizontally");

static bool skip_reset = false;
module_param(skip_reset, bool, 0444);
MODULE_PARM_DESC(skip_reset, "skip the initial display reset");

static bool auto_refresh = false;
module_param(auto_refresh, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(auto_refresh, "auto refresh the screen based on partial refreshed area");

static int refresh_threshold = 20;
module_param(refresh_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(refresh_threshold, "refresh threshold in screen area multiples");

static int refresh_waveform = DRM_EPD_WF_GC16;
module_param(refresh_waveform, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(refresh_waveform, "refresh waveform to use");

static int split_area_limit = 12;
module_param(split_area_limit, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(split_area_limit, "how many areas to split in each scheduling call");

static int limit_fb_blits = -1;
module_param(limit_fb_blits, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(split_area_limit, "how many fb blits to allow. -1 does not limit");

/* delay parameters used to delay the return of plane_atomic_atomic */
/* see plane_atomic_update function for specific usage of these parameters */
static int delay_a = 2000;
module_param(delay_a, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_a, "delay_a");

static int delay_b = 100000;
module_param(delay_b, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_b, "delay_b");

static int delay_c = 1000;
module_param(delay_c, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(delay_c, "delay_c");

// mode = 0: 16-level gray scale
// mode = 1: 2-level black&white with dithering enabled
// mode = 2: 2-level black&white, uses bw_threshold
static int bw_mode = 0;
module_param(bw_mode, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_mode, "black & white mode");

static int bw_threshold = 7;
module_param(bw_threshold, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_threshold, "black and white threshold");

static int bw_dither_invert = 0;
module_param(bw_dither_invert, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(bw_dither_invert, "invert dither colors in bw mode");

static bool prepare_prev_before_a2 = false;
module_param(prepare_prev_before_a2, bool, 0644);
MODULE_PARM_DESC(prepare_prev_before_a2, "Convert prev buffer to bw when switchting to the A2 waveform");

static int dclk_select = 0;
module_param(dclk_select, int, 0644);
MODULE_PARM_DESC(dclk_select, "-1: use dclk from mode, 0: 200 MHz (default), 1: 250");

static int temp_override = 0;
module_param(temp_override, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(temp_override, "Values > 0 override the temperature");

static int temp_offset = 0;
module_param(temp_offset, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(temp_offset, "Values > 0 is subtracted from the temperature to compensate for the pcb sensor being hotter than the display");


DEFINE_DRM_GEM_FOPS(rockchip_ebc_fops);

static int ioctl_trigger_global_refresh(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_trigger_global_refresh *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);

	if (args->trigger_global_refresh){
		/* printk(KERN_INFO "[rockchip_ebc] ioctl_trigger_global_refresh"); */
		spin_lock(&ebc->refresh_once_lock);
		ebc->do_one_full_refresh = true;
		spin_unlock(&ebc->refresh_once_lock);
		// try to trigger the refresh immediately
		wake_up_process(ebc->refresh_thread);
	}

	return 0;
}

static int ioctl_set_off_screen(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_off_screen *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);
	int copy_result;

	copy_result = copy_from_user(&ebc->off_screen, args->ptr_screen_content, 1313144);

	return 0;
}


/**
 * struct rockchip_ebc_ctx - context for performing display refreshes
 *
 * @kref: Reference count, maintained as part of the CRTC's atomic state
 * @queue: Queue of damaged areas to be refreshed
 * @queue_lock: Lock protecting access to @queue
 * @prev: Display contents (Y4) before this refresh
 * @next: Display contents (Y4) after this refresh
 * @final: Display contents (Y4) after all pending refreshes
 * @phase: Buffers for selecting a phase from the EBC's LUT, 1 byte/pixel
 * @gray4_pitch: Horizontal line length of a Y4 pixel buffer in bytes
 * @gray4_size: Size of a Y4 pixel buffer in bytes
 * @phase_pitch: Horizontal line length of a phase buffer in bytes
 * @phase_size: Size of a phase buffer in bytes
 */
struct rockchip_ebc_ctx {
	struct kref			kref;
	struct list_head		queue;
	spinlock_t			queue_lock;
	u8				*prev;
	u8				*next;
	u8				*final;
	u8				*phase[2];
	u32				gray4_pitch;
	u32				gray4_size;
	u32				phase_pitch;
	u32				phase_size;
	u64 area_count;
};

struct ebc_crtc_state {
	struct drm_crtc_state		base;
	struct rockchip_ebc_ctx		*ctx;
};

static inline struct ebc_crtc_state *
to_ebc_crtc_state(struct drm_crtc_state *crtc_state)
{
	return container_of(crtc_state, struct ebc_crtc_state, base);
}
static int ioctl_extract_fbs(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct drm_rockchip_ebc_extract_fbs *args = data;
	struct rockchip_ebc *ebc = dev_get_drvdata(dev->dev);
	int copy_result = 0;
	struct rockchip_ebc_ctx * ctx;

	// todo: use access_ok here
	access_ok(args->ptr_next, 1313144);
	ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;
	copy_result |= copy_to_user(args->ptr_prev, ctx->prev, 1313144);
	copy_result |= copy_to_user(args->ptr_next, ctx->next, 1313144);
	copy_result |= copy_to_user(args->ptr_final, ctx->final, 1313144);

	copy_result |= copy_to_user(args->ptr_phase1, ctx->phase[0], 2 * 1313144);
	copy_result |= copy_to_user(args->ptr_phase2, ctx->phase[1], 2 * 1313144);

	return copy_result;
}

static const struct drm_ioctl_desc ioctls[DRM_COMMAND_END - DRM_COMMAND_BASE] = {
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_GLOBAL_REFRESH, ioctl_trigger_global_refresh,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_OFF_SCREEN, ioctl_set_off_screen,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_EBC_EXTRACT_FBS, ioctl_extract_fbs,
			  DRM_RENDER_ALLOW),
};

static const struct drm_driver rockchip_ebc_drm_driver = {
	.lastclose		= drm_fb_helper_lastclose,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.major			= 0,
	.minor			= 3,
	.name			= "rockchip-ebc",
	.desc			= "Rockchip E-Book Controller",
	.date			= "20220303",
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &rockchip_ebc_fops,
	.ioctls = ioctls,
	.num_ioctls = DRM_ROCKCHIP_EBC_NUM_IOCTLS,
};

static const struct drm_mode_config_funcs rockchip_ebc_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create_with_dirty,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

/**
 * struct rockchip_ebc_area - describes a damaged area of the display
 *
 * @list: Used to put this area in the state/context/refresh thread list
 * @clip: The rectangular clip of this damage area
 * @frame_begin: The frame number when this damage area starts being refreshed
 */
struct rockchip_ebc_area {
	struct list_head		list;
	struct drm_rect			clip;
	u32				frame_begin;
};

static void rockchip_ebc_ctx_free(struct rockchip_ebc_ctx *ctx)
{
	struct rockchip_ebc_area *area;

	list_for_each_entry(area, &ctx->queue, list)
		kfree(area);
	kfree(ctx->prev);
	kfree(ctx->next);
	kfree(ctx->final);
	kfree(ctx->phase[0]);
	kfree(ctx->phase[1]);
	kfree(ctx);
}

static struct rockchip_ebc_ctx *rockchip_ebc_ctx_alloc(u32 width, u32 height)
{
	u32 gray4_size = width * height / 2;
	u32 phase_size = width * height;
	struct rockchip_ebc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->prev = kmalloc(gray4_size, GFP_KERNEL | GFP_DMA);
	ctx->next = kmalloc(gray4_size, GFP_KERNEL | GFP_DMA);
	ctx->final = kmalloc(gray4_size, GFP_KERNEL | GFP_DMA);
	ctx->phase[0] = kmalloc(phase_size, GFP_KERNEL | GFP_DMA);
	// todo: investigate if we would need something like __GFP_DMA32 (which
	// apparently is not allowed...)
	// https://lkml.org/lkml/2022/4/5/2876
	ctx->phase[1] = kmalloc(phase_size, GFP_KERNEL | GFP_DMA);
	if (!ctx->prev || !ctx->next || !ctx->final ||
	    !ctx->phase[0] || !ctx->phase[1]) {
		rockchip_ebc_ctx_free(ctx);
		return NULL;
	}

	kref_init(&ctx->kref);
	INIT_LIST_HEAD(&ctx->queue);
	spin_lock_init(&ctx->queue_lock);
	ctx->gray4_pitch = width / 2;
	ctx->gray4_size  = gray4_size;
	ctx->phase_pitch = width;
	ctx->phase_size  = phase_size;

	// we keep track of the updated area and use this value to trigger global
	// refreshes if auto_refresh is enabled
	ctx->area_count = 0;

	return ctx;
}

static void rockchip_ebc_ctx_release(struct kref *kref)
{
	struct rockchip_ebc_ctx *ctx =
		container_of(kref, struct rockchip_ebc_ctx, kref);

	return rockchip_ebc_ctx_free(ctx);
}

/*
 * CRTC
 */

static void rockchip_ebc_global_refresh(struct rockchip_ebc *ebc,
					struct rockchip_ebc_ctx *ctx,
					 dma_addr_t next_handle,
					 dma_addr_t prev_handle
					)
{
	struct drm_device *drm = &ebc->drm;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;

	struct rockchip_ebc_area *area, *next_area;
	LIST_HEAD(areas);

	spin_lock(&ctx->queue_lock);
	list_splice_tail_init(&ctx->queue, &areas);
	memcpy(ctx->next, ctx->final, gray4_size);
	spin_unlock(&ctx->queue_lock);

	dma_sync_single_for_device(dev, next_handle, gray4_size, DMA_TO_DEVICE);
	dma_sync_single_for_device(dev, prev_handle, gray4_size, DMA_TO_DEVICE);

	reinit_completion(&ebc->display_end);
	regmap_write(ebc->regmap, EBC_CONFIG_DONE,
		     EBC_CONFIG_DONE_REG_CONFIG_DONE);
	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start |
		     EBC_DSP_START_DSP_FRM_TOTAL(ebc->lut.num_phases - 1) |
		     EBC_DSP_START_DSP_FRM_START);
	// while we wait for the refresh, delete all scheduled areas
	list_for_each_entry_safe(area, next_area, &areas, list) {
		list_del(&area->list);
		kfree(area);
	}

	if (!wait_for_completion_timeout(&ebc->display_end,
					 EBC_REFRESH_TIMEOUT))
		drm_err(drm, "Refresh timed out!\n");

	memcpy(ctx->prev, ctx->next, gray4_size);
}

/*
 * Returns true if the area was split, false otherwise
 */
static int try_to_split_area(
		struct list_head *areas,
	    struct rockchip_ebc_area *area,
	    struct rockchip_ebc_area *other,
	    int * split_counter,
	    struct rockchip_ebc_area **p_next_area,
		struct drm_rect * intersection
	    ){
	int xmin, xmax, ymin, ymax, xcenter, ycenter;

	bool no_xsplit = false;
	bool no_ysplit = false;
	bool split_both = true;

	struct rockchip_ebc_area * item1;
	struct rockchip_ebc_area * item2;
	struct rockchip_ebc_area * item3;
	struct rockchip_ebc_area * item4;

	// we do not want to overhelm the refresh thread and limit us to a
	// certain number of splits. The rest needs to wait
	if (*split_counter >= split_area_limit)
		return 0;


	// for now, min size if 2x2
	if ((area->clip.x2 - area->clip.x1 < 2) | (area->clip.y2 - area->clip.y1 < 2))
		return 0;

	// ok, we want to split this area and start with any partial areas
	// that are not overlapping (well, let this be decided upon at the
	// next outer loop - we delete this area so we need not to juggle
	// around the four areas until we found the one that is actually
	// overlapping)
	xmin = area->clip.x1;
	if (intersection->x1 > xmin)
		xcenter = intersection->x1;
	else
		xcenter = intersection->x2;
	xmax = area->clip.x2;

	ymin = area->clip.y1;
	if (intersection->y1 > ymin)
		ycenter = intersection->y1;
	else
		ycenter = intersection->y2;
	ymax = area->clip.y2;

	if ((xmin == xcenter) | (xcenter == xmax)){
		no_xsplit = true;
		split_both = false;
	}
	if ((ymin == ycenter) | (ycenter == ymax)){
		no_ysplit = true;
		split_both = false;
	}

	// can we land here at all???
	if (no_xsplit && no_ysplit)
		return 0;

	// we need four new rokchip_ebc_area entries that we splice into
	// the list. Note that the currently next item shall be copied
	// backwards because to prevent the outer list iteration from
	// skipping over our newly created items.

	item1 = kmalloc(sizeof(*item1), GFP_KERNEL);
	if (split_both || no_xsplit)
		item2 = kmalloc(sizeof(*item2), GFP_KERNEL);
	if (split_both || no_ysplit)
		item3 = kmalloc(sizeof(*item3), GFP_KERNEL);
	if (split_both)
		item4 = kmalloc(sizeof(*item4), GFP_KERNEL);

	// TODO: Error checking!!!!
	/* if (!area) */
	/* 	return -ENOMEM; */

	if (no_xsplit)
		xcenter = xmax;

	if (no_ysplit)
		ycenter = ymax;

	if (list_is_last(&area->list, areas)){
		list_add_tail(&item1->list, areas);
		if (split_both || no_xsplit)
			list_add_tail(&item2->list, areas);
		if (split_both || no_ysplit)
			list_add_tail(&item3->list, areas);
		if (split_both)
			list_add_tail(&item4->list, areas);
	}
	else{
		if (split_both)
			__list_add(&item4->list, &area->list, area->list.next);
		if (split_both || no_ysplit)
			__list_add(&item3->list, &area->list, area->list.next);
		if (split_both || no_xsplit)
			__list_add(&item2->list, &area->list, area->list.next);
		__list_add(&item1->list, &area->list, area->list.next);
	}
	*p_next_area = item1;

	// now fill the areas

	// always
	item1->frame_begin = EBC_FRAME_PENDING;
	item1->clip.x1 = xmin;
	item1->clip.x2 = xcenter;
	item1->clip.y1 = ymin;
	item1->clip.y2 = ycenter;

	if (split_both || no_xsplit){
		// no xsplit
		item2->frame_begin = EBC_FRAME_PENDING;
		item2->clip.x1 = xmin;
		item2->clip.x2 = xcenter;
		item2->clip.y1 = ycenter;
		item2->clip.y2 = ymax;
	}

	if (split_both || no_ysplit){
		// no ysplit
		item3->frame_begin = EBC_FRAME_PENDING;
		item3->clip.x1 = xcenter;
		item3->clip.x2 = xmax;
		item3->clip.y1 = ymin;
		item3->clip.y2 = ycenter;
	}

	if (split_both){
		// both splits
		item4->frame_begin = EBC_FRAME_PENDING;
		item4->clip.x1 = xcenter;
		item4->clip.x2 = xmax;
		item4->clip.y1 = ycenter;
		item4->clip.y2 = ymax;
	}

	(*split_counter)++;
	return 1;
}

static bool rockchip_ebc_schedule_area(struct list_head *areas,
				       struct rockchip_ebc_area *area,
				       struct drm_device *drm,
				       u32 current_frame, u32 num_phases,
				       struct rockchip_ebc_area **p_next_area,
					   int * split_counter
					   )
{
	struct rockchip_ebc_area *other;
	// by default, begin now
	u32 frame_begin = current_frame;
	/* printk(KERN_INFO "scheduling area: %i-%i %i-%i (current frame: %i)\n", area->clip.x1, area->clip.x2, area->clip.y1, area->clip.y2, current_frame); */

	list_for_each_entry(other, areas, list) {
		struct drm_rect intersection;
		u32 other_end;
		/* printk(KERN_INFO "    test other area: %i-%i %i-%i (beginning at: %i)\n", other->clip.x1, other->clip.x2, other->clip.y1, other->clip.y2, other->frame_begin); */

		/* Only consider areas before this one in the list. */
		if (other == area){
			/* printk(KERN_INFO "        other==area\n"); */
			break;
		}

		/* Skip areas that finish refresh before this area begins. */
		other_end = other->frame_begin + num_phases;
		if (other_end <= frame_begin){
			/* printk(KERN_INFO "        other finishes before: %i %i\n", other_end, frame_begin); */
			continue;
		}

		/* If there is no collision, the areas are independent. */
		intersection = area->clip;
		if (!drm_rect_intersect(&intersection, &other->clip)){
			/* printk(KERN_INFO "        no collision\n"); */
			continue;
		}

		/* If the other area already started, wait until it finishes. */
		if (other->frame_begin < current_frame) {
			frame_begin = max(frame_begin, other_end);
			/* printk(KERN_INFO "        other already started, setting to %i (%i, %i)\n", frame_begin, num_phases, other_end); */

			// so here we would optimally want to split the new area into three
			// parts that do not overlap with the already-started area, and one
			// which is overlapping. The overlapping one will be scheduled for
			// later, but the other three should start immediately.

			// if the area is equal to the clip, continue
			if (drm_rect_equals(&area->clip, &intersection)){
				//printk(KERN_INFO "        intersection completely contains area\n");
				continue;
			}

			if (try_to_split_area(areas, area, other, split_counter, p_next_area, &intersection))
			{
				// let the outer loop delete this area
				/* printk(KERN_INFO "        dropping after trying to split\n"); */
				return false;
			} else {
				continue;
			}
		}

		/*
		 * If the other area has not started yet, and completely
		 * contains this area, then this area is redundant.
		 */
		if (drm_rect_equals(&area->clip, &intersection)) {
			drm_dbg(drm, "area %p (" DRM_RECT_FMT ") dropped, inside " DRM_RECT_FMT "\n",
				area, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&other->clip));
			/* printk(KERN_INFO "    dropping\n"); */
			return false;
		}

		/* They do overlap but are are not equal and both not started yet, so
		 * they can potentially start together */
		if (frame_begin > other->frame_begin){
			// for some reason we need to begin later than the other region,
			// which forces us to wait for the region
			frame_begin = other_end;
		} else {
			/* frame_begin = max(frame_begin, other->frame_begin); */
			// they can begin together
			frame_begin = other->frame_begin;
		}

		/* printk(KERN_INFO "    setting to: %i\n", frame_begin); */

		// try to split, otherwise continue
		if (try_to_split_area(areas, area, other, split_counter, p_next_area, &intersection))
		{
			// let the outer loop delete this area
			/* printk(KERN_INFO "    dropping after trying to split\n"); */
			return false;
		} else {
			/* printk(KERN_INFO "    split not successful\n"); */
			continue;
		}
	}

	area->frame_begin = frame_begin;
	/* printk(KERN_INFO "    area scheduled to start at frame: %i (current: %i)\n", frame_begin, current_frame); */

	return true;
}

static void rockchip_ebc_blit_direct(const struct rockchip_ebc_ctx *ctx,
				     u8 *dst, u8 phase,
				     const struct drm_epd_lut *lut,
				     const struct drm_rect *clip)
{
	const u32 *phase_lut = (const u32 *)lut->buf + 16 * phase;
	unsigned int dst_pitch = ctx->phase_pitch / 4;
	unsigned int src_pitch = ctx->gray4_pitch;
	unsigned int x, y;
	u8 *dst_line;
	u32 src_line;

	dst_line = dst + clip->y1 * dst_pitch + clip->x1 / 4;
	src_line = clip->y1 * src_pitch + clip->x1 / 2;

	for (y = clip->y1; y < clip->y2; y++) {
		u32 src_offset = src_line;
		u8 *dbuf = dst_line;

		for (x = clip->x1; x < clip->x2; x += 4) {
			u8 prev0 = ctx->prev[src_offset];
			u8 next0 = ctx->next[src_offset++];
			u8 prev1 = ctx->prev[src_offset];
			u8 next1 = ctx->next[src_offset++];

			/*
			 * The LUT is 256 phases * 16 next * 16 previous levels.
			 * Each value is two bits, so the last dimension neatly
			 * fits in a 32-bit word.
			 */
			u8 data = ((phase_lut[next0 & 0xf] >> ((prev0 & 0xf) << 1)) & 0x3) << 0 |
				  ((phase_lut[next0 >>  4] >> ((prev0 >>  4) << 1)) & 0x3) << 2 |
				  ((phase_lut[next1 & 0xf] >> ((prev1 & 0xf) << 1)) & 0x3) << 4 |
				  ((phase_lut[next1 >>  4] >> ((prev1 >>  4) << 1)) & 0x3) << 6;

			/* Diff mode ignores pixels that did not change brightness. */
			if (diff_mode) {
				u8 mask = ((next0 ^ prev0) & 0x0f ? 0x03 : 0) |
					  ((next0 ^ prev0) & 0xf0 ? 0x0c : 0) |
					  ((next1 ^ prev1) & 0x0f ? 0x30 : 0) |
					  ((next1 ^ prev1) & 0xf0 ? 0xc0 : 0);

				data &= mask;
			}

			*dbuf++ = data;
		}

		dst_line += dst_pitch;
		src_line += src_pitch;
	}
}

static void rockchip_ebc_blit_phase(const struct rockchip_ebc_ctx *ctx,
				    u8 *dst, u8 phase,
				    const struct drm_rect *clip)
{
	unsigned int pitch = ctx->phase_pitch;
	unsigned int width = clip->x2 - clip->x1;
	unsigned int y;
	u8 *dst_line;

	dst_line = dst + clip->y1 * pitch + clip->x1;

	for (y = clip->y1; y < clip->y2; y++) {
		memset(dst_line, phase, width);

		dst_line += pitch;
	}
}

static void rockchip_ebc_blit_pixels(const struct rockchip_ebc_ctx *ctx,
				     u8 *dst, const u8 *src,
				     const struct drm_rect *clip)
{
	bool start_x_is_odd = clip->x1 & 1;
	bool end_x_is_odd = clip->x2 & 1;
	u8 first_odd;
	u8 last_odd;

	unsigned int x1_bytes = clip->x1 / 2;
	unsigned int x2_bytes = clip->x2 / 2;

	unsigned int pitch = ctx->gray4_pitch;
	unsigned int width;
	const u8 *src_line;
	unsigned int y;
	u8 *dst_line;

	// the integer division floors by default, but we want to include the last
	// byte (partially)
	if (end_x_is_odd)
		x2_bytes++;

	width = x2_bytes - x1_bytes;

	dst_line = dst + clip->y1 * pitch + x1_bytes;
	src_line = src + clip->y1 * pitch + x1_bytes;

	for (y = clip->y1; y < clip->y2; y++) {
		if (start_x_is_odd)
			// keep only lower bit to restore it after the blitting
			first_odd = *src_line & 0b00001111;
		if (end_x_is_odd){
			dst_line += pitch - 1;
			// keep only the upper bit for restoring later
			last_odd = *dst_line & 0b11110000;
			dst_line -= pitch - 1;
		}

		memcpy(dst_line, src_line, width);

		if (start_x_is_odd){
			// write back the first 4 saved bits
			*dst_line = first_odd | (*dst_line & 0b11110000);
		}
		if (end_x_is_odd){
			// write back the last 4 saved bits
			dst_line += pitch -1;
			*dst_line = (*dst_line & 0b00001111) | last_odd;
			dst_line -= pitch -1;
		}

		dst_line += pitch;
		src_line += pitch;
	}
}

static void rockchip_ebc_partial_refresh(struct rockchip_ebc *ebc,
					 struct rockchip_ebc_ctx *ctx,
					 dma_addr_t next_handle,
					 dma_addr_t prev_handle
					 )
{
	struct rockchip_ebc_area *area, *next_area;
	u32 last_phase = ebc->lut.num_phases - 1;
	struct drm_device *drm = &ebc->drm;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;
	LIST_HEAD(areas);
	u32 frame;
	u64 local_area_count = 0;

	dma_addr_t phase_handles[2];
	phase_handles[0] = dma_map_single(dev, ctx->phase[0], ctx->gray4_size, DMA_TO_DEVICE);
	phase_handles[1] = dma_map_single(dev, ctx->phase[1], ctx->gray4_size, DMA_TO_DEVICE);

	for (frame = 0;; frame++) {
		/* Swap phase buffers to minimize latency between frames. */
		u8 *phase_buffer = ctx->phase[frame % 2];
		dma_addr_t phase_handle = phase_handles[frame % 2];
		bool sync_next = false;
		bool sync_prev = false;
		int split_counter = 0;
		int gotlock;

		// now the CPU is allowed to change the phase buffer
		dma_sync_single_for_cpu(dev, phase_handle, ctx->phase_size, DMA_TO_DEVICE);

		/* Move the queued damage areas to the local list. */
		/* spin_lock(&ctx->queue_lock); */
		/* list_splice_tail_init(&ctx->queue, &areas); */
		/* gotlock = spin_trylock(&ctx->queue_lock); */
		gotlock = true;
		spin_lock(&ctx->queue_lock);
        if (gotlock)
			list_splice_tail_init(&ctx->queue, &areas);
		/* spin_unlock(&ctx->queue_lock); */

		list_for_each_entry_safe(area, next_area, &areas, list) {
			s32 frame_delta;
			u32 phase;

			/*
			 * Determine when this area can start its refresh.
			 * If the area is redundant, drop it immediately.
			 */
			if (area->frame_begin == EBC_FRAME_PENDING &&
			    !rockchip_ebc_schedule_area(&areas, area, drm, frame,
							ebc->lut.num_phases, &next_area, &split_counter)) {
				list_del(&area->list);
				kfree(area);
				continue;
			}

			// we wait a little bit longer to start
			frame_delta = frame - area->frame_begin;
			if (frame_delta < 0)
				continue;

			/* Copy ctx->final to ctx->next on the first frame. */
			if (frame_delta == 0) {
				/* printk(KERN_INFO "[rockchip-ebc] partial refresh starting area on frame %i (%i/%i %i/%i) (end: %i)\n", frame, area->clip.x1, area->clip.x2, area->clip.y1, area->clip.y2, area->frame_begin + last_phase); */
				local_area_count += (u64) (
					area->clip.x2 - area->clip.x1) *
					(area->clip.y2 - area->clip.y1);
				dma_sync_single_for_cpu(dev, next_handle, gray4_size, DMA_TO_DEVICE);
				rockchip_ebc_blit_pixels(ctx, ctx->next,
							 ctx->final,
							 &area->clip);
				sync_next = true;

				drm_dbg(drm, "area %p (" DRM_RECT_FMT ") started on %u\n",
					area, DRM_RECT_ARG(&area->clip), frame);
			}

			/*
			 * Take advantage of the fact that the last phase in a
			 * waveform is always zero (neutral polarity). Instead
			 * of writing the actual phase number, write 0xff (the
			 * last possible phase number), which is guaranteed to
			 * be neutral for every waveform.
			 */
			phase = frame_delta >= last_phase ? 0xff : frame_delta;
			if (direct_mode)
				rockchip_ebc_blit_direct(ctx, phase_buffer,
							 phase, &ebc->lut,
							 &area->clip);
			else
				rockchip_ebc_blit_phase(ctx, phase_buffer,
							phase, &area->clip);

			/*
			 * Copy ctx->next to ctx->prev after the last phase.
			 * Technically, this races with the hardware computing
			 * the last phase, but the last phase is all zeroes
			 * anyway, regardless of prev/next (see above).
			 *
			 * Keeping the area in the list for one extra frame
			 * also ensures both phase buffers get set to 0xff.
			 */
			if (frame_delta > last_phase) {
				dma_sync_single_for_cpu(dev, prev_handle, gray4_size, DMA_TO_DEVICE);
				dma_sync_single_for_cpu(dev, next_handle, gray4_size, DMA_TO_DEVICE);
				rockchip_ebc_blit_pixels(ctx, ctx->prev,
							 ctx->next,
							 &area->clip);
				sync_prev = true;
				sync_prev = true;

				drm_dbg(drm, "area %p (" DRM_RECT_FMT ") finished on %u\n",
					area, DRM_RECT_ARG(&area->clip), frame);

				/* printk(KERN_INFO "[rockchip-ebc]     partial refresh stopping area on frame %i (%i/%i %i/%i)\n", frame, area->clip.x1, area->clip.x2, area->clip.y1, area->clip.y2); */
				list_del(&area->list);
				kfree(area);
			}
		}

		if (sync_next)
			dma_sync_single_for_device(dev, next_handle,
						   gray4_size, DMA_TO_DEVICE);
		if (sync_prev)
			dma_sync_single_for_device(dev, prev_handle,
						   gray4_size, DMA_TO_DEVICE);
		dma_sync_single_for_device(dev, phase_handle,
					   ctx->phase_size, DMA_TO_DEVICE);

		if (gotlock)
			spin_unlock(&ctx->queue_lock);
		/* spin_unlock(&ctx->queue_lock); */

		/* if (frame) { */
		/* 	if (!wait_for_completion_timeout(&ebc->display_end, */
		/* 					 EBC_FRAME_TIMEOUT)) */
		/* 		drm_err(drm, "Frame %d timed out!\n", frame); */
		/* } */

		if (list_empty(&areas))
			break;

		regmap_write(ebc->regmap,
			     direct_mode ? EBC_WIN_MST0 : EBC_WIN_MST2,
			     phase_handle);
		regmap_write(ebc->regmap, EBC_CONFIG_DONE,
			     EBC_CONFIG_DONE_REG_CONFIG_DONE);
		regmap_write(ebc->regmap, EBC_DSP_START,
			     ebc->dsp_start |
			     EBC_DSP_START_DSP_FRM_START);
		/* if (frame) { */
		/* 	if (!wait_for_completion_timeout(&ebc->display_end, */
		/* 					 EBC_FRAME_TIMEOUT)) */
		/* 		drm_err(drm, "Frame %d timed out!\n", frame); */
		/* } */
		if (!wait_for_completion_timeout(&ebc->display_end,
						 EBC_FRAME_TIMEOUT))
			drm_err(drm, "Frame %d timed out!\n", frame);


		if (kthread_should_stop()) {
			break;
		};
	}
	dma_unmap_single(dev, phase_handles[0], ctx->gray4_size, DMA_TO_DEVICE);
	dma_unmap_single(dev, phase_handles[1], ctx->gray4_size, DMA_TO_DEVICE);
	ctx->area_count += local_area_count;
}

static void rockchip_ebc_refresh(struct rockchip_ebc *ebc,
				 struct rockchip_ebc_ctx *ctx,
				 bool global_refresh,
				 enum drm_epd_waveform waveform)
{
	struct drm_device *drm = &ebc->drm;
	u32 dsp_ctrl = 0, epd_ctrl = 0;
	struct device *dev = drm->dev;
	int ret, temperature;
	dma_addr_t next_handle;
	dma_addr_t prev_handle;
	int one_screen_area = 1314144;
	/* printk(KERN_INFO "[rockchip_ebc] rockchip_ebc_refresh"); */

	/* Resume asynchronously while preparing to refresh. */
	ret = pm_runtime_get(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to request resume: %d\n", ret);
		return;
	}

	ret = iio_read_channel_processed(ebc->temperature_channel, &temperature);
	if (ret < 0) {
		drm_err(drm, "Failed to get temperature: %d\n", ret);
	} else {
		/* Convert from millicelsius to celsius. */
		temperature /= 1000;

		if (temp_override > 0){
			printk(KERN_INFO "rockchip-ebc: override temperature from %i to %i\n", temp_override, temperature);
            temperature = temp_override;
        }
        	} else if (temp_offset > 0){
			int old_val = temperature;
			if (temperature > temp_offset)
				temperature -= temp_offset;
			else
				temperature = 0;
			printk(KERN_INFO "rockchip-ebc: temp offset from %i to %i\n", old_val, temperature);
		}

		ret = drm_epd_lut_set_temperature(&ebc->lut, temperature);
		if (ret < 0)
			drm_err(drm, "Failed to set LUT temperature: %d\n", ret);
		else if (ret)
			ebc->lut_changed = true;
	}

	ret = drm_epd_lut_set_waveform(&ebc->lut, waveform);
	if (ret < 0)
		drm_err(drm, "Failed to set LUT waveform: %d\n", ret);
	else if (ret)
		ebc->lut_changed = true;

	/* if we change to A2 in bw mode, then make sure that the prev-buffer is
	 * converted to bw so the A2 waveform can actually do anything
	 * */
	// todo: make optional
	if (prepare_prev_before_a2){
		if(ebc->lut_changed && waveform == 1){
			u8 pixel1, pixel2;
			void *src = ctx->prev;
			u8 *sbuf = src;
			int index;
			printk(KERN_INFO "Change to A2 waveform detected, converting prev to bw");

			for (index=0; index < ctx->gray4_size; index++){
				pixel1 = *sbuf & 0b00001111;
				pixel2 = (*sbuf & 0b11110000) >> 4;

				// convert to bw
				if (pixel1 > 7)
					pixel1 = 15;
				else
					pixel1 = 0;
				if (pixel2 > 7)
					pixel2 = 15;
				else
					pixel2 = 0;

				*sbuf++ = pixel1 | pixel2 << 4;
			}
		}
	}

	/* Wait for the resume to complete before writing any registers. */
	ret = pm_runtime_resume(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to resume: %d\n", ret);
		pm_runtime_put(dev);
		return;
	}

	/* This flag may have been set above, or by the runtime PM callback. */
	if (ebc->lut_changed) {
		ebc->lut_changed = false;
		regmap_bulk_write(ebc->regmap, EBC_LUT_DATA,
				  ebc->lut.buf, EBC_NUM_LUT_REGS);
	}

	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start);

	/*
	 * The hardware has a separate bit for each mode, with some priority
	 * scheme between them. For clarity, only set one bit at a time.
	 *
	 * NOTE: In direct mode, no mode bits are set.
	 */
	if (global_refresh) {
		dsp_ctrl |= EBC_DSP_CTRL_DSP_LUT_MODE;
	} else if (!direct_mode) {
		epd_ctrl |= EBC_EPD_CTRL_DSP_THREE_WIN_MODE;
		if (diff_mode)
			dsp_ctrl |= EBC_DSP_CTRL_DSP_DIFF_MODE;
	}
	regmap_update_bits(ebc->regmap, EBC_EPD_CTRL,
			   EBC_EPD_CTRL_DSP_THREE_WIN_MODE,
			   epd_ctrl);
	regmap_update_bits(ebc->regmap, EBC_DSP_CTRL,
			   EBC_DSP_CTRL_DSP_DIFF_MODE |
			   EBC_DSP_CTRL_DSP_LUT_MODE,
			   dsp_ctrl);

	next_handle = dma_map_single(dev, ctx->next, ctx->gray4_size, DMA_TO_DEVICE);
	prev_handle = dma_map_single(dev, ctx->prev, ctx->gray4_size, DMA_TO_DEVICE);

	regmap_write(ebc->regmap, EBC_WIN_MST0,
		     next_handle);
	regmap_write(ebc->regmap, EBC_WIN_MST1,
		     prev_handle);

	/* printk(KERN_INFO "[rockchip_ebc] ebc_refresh"); */
	if (global_refresh)
		rockchip_ebc_global_refresh(ebc, ctx, next_handle, prev_handle);
	else
		rockchip_ebc_partial_refresh(ebc, ctx, next_handle, prev_handle);

	dma_unmap_single(dev, next_handle, ctx->gray4_size, DMA_TO_DEVICE);
	dma_unmap_single(dev, prev_handle, ctx->gray4_size, DMA_TO_DEVICE);

	// do we need a full refresh
	if (auto_refresh){
		if (ctx->area_count >= refresh_threshold * one_screen_area){
			spin_lock(&ebc->refresh_once_lock);
			ebc->do_one_full_refresh = true;
			spin_unlock(&ebc->refresh_once_lock);
			ctx->area_count = 0;
		}
	} else {
		ctx->area_count = 0;
	}

	/* Drive the output pins low once the refresh is complete. */
	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start |
		     EBC_DSP_START_DSP_OUT_LOW);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int rockchip_ebc_refresh_thread(void *data)
{
	struct rockchip_ebc *ebc = data;
	struct rockchip_ebc_ctx *ctx;
	bool one_full_refresh;
	/* printk(KERN_INFO "[rockchip_ebc] rockchip_ebc_refresh_thread"); */

	while (!kthread_should_stop()) {
		/* printk(KERN_INFO "[rockchip_ebc] just started"); */
		/* The context will change each time the thread is unparked. */
		ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;

		/*
		 * Initialize the buffers before use. This is deferred to the
		 * kthread to avoid slowing down atomic_check.
		 *
		 * ctx->prev and ctx->next are set to 0xff, all white, because:
		 *  1) the display is set to white by the reset waveform, and
		 *  2) the driver maintains the invariant that the display is
		 *     all white whenever the CRTC is disabled.
		 *
		 * ctx->final is initialized by the first plane update.
		 *
		 * ctx->phase is set to 0xff, the number of the last possible
		 * phase, because the LUT for that phase is known to be all
		 * zeroes. (The last real phase in a waveform is zero in order
		 * to discharge the display, and unused phases in the LUT are
		 * zeroed out.) This prevents undesired driving of the display
		 * in 3-window mode between when the framebuffer is blitted
		 * (and thus prev != next) and when the refresh thread starts
		 * counting phases for that region.
		 */
		if(ebc->suspend_was_requested == 1){
			// this means we are coming out from suspend. Reset the buffers to
			// the before-suspend state
			memcpy(ctx->prev, ebc->suspend_prev, ctx->gray4_size);
			memcpy(ctx->final, ebc->suspend_next, ctx->gray4_size);
			/* memset(ctx->prev, 0xff, ctx->gray4_size); */
			memset(ctx->next, 0xff, ctx->gray4_size);
			/* memset(ctx->final, 0xff, ctx->gray4_size); */
			ebc->do_one_full_refresh = 1;
		} else {
			memset(ctx->prev, 0xff, ctx->gray4_size);
			memset(ctx->next, 0xff, ctx->gray4_size);
			memset(ctx->final, 0xff, ctx->gray4_size);
		}

		/* NOTE: In direct mode, the phase buffers are repurposed for
		 * source driver polarity data, where the no-op value is 0. */
		memset(ctx->phase[0], direct_mode ? 0 : 0xff, ctx->phase_size);
		memset(ctx->phase[1], direct_mode ? 0 : 0xff, ctx->phase_size);

		/*
		 * LUTs use both the old and the new pixel values as inputs.
		 * However, the initial contents of the display are unknown.
		 * The special RESET waveform will initialize the display to
		 * known contents (white) regardless of its current contents.
		 */
		if (!ebc->reset_complete) {
			ebc->reset_complete = true;
			rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_RESET);
		}

		while ((!kthread_should_park()) && (!kthread_should_stop())) {
			/* printk(KERN_INFO "[rockchip_ebc] inner loop"); */
			spin_lock(&ebc->refresh_once_lock);
			one_full_refresh = ebc->do_one_full_refresh;
			spin_unlock(&ebc->refresh_once_lock);

			if (one_full_refresh) {
				/* printk(KERN_INFO "[rockchip_ebc] got one full refresh"); */
				spin_lock(&ebc->refresh_once_lock);
				ebc->do_one_full_refresh = false;
				spin_unlock(&ebc->refresh_once_lock);
/* 				 * @DRM_EPD_WF_A2: Fast transitions between black and white only */
/* 				 * @DRM_EPD_WF_DU: Transitions 16-level grayscale to monochrome */
/* 				 * @DRM_EPD_WF_DU4: Transitions 16-level grayscale to 4-level grayscale */
/* 				 * @DRM_EPD_WF_GC16: High-quality but flashy 16-level grayscale */
/* 				 * @DRM_EPD_WF_GCC16: Less flashy 16-level grayscale */
/* 				 * @DRM_EPD_WF_GL16: Less flashy 16-level grayscale */
/* 				 * @DRM_EPD_WF_GLR16: Less flashy 16-level grayscale, plus anti-ghosting */
/* 				 * @DRM_EPD_WF_GLD16: Less flashy 16-level grayscale, plus anti-ghosting */
				// Not sure why only the GC16 is able to clear the ghosts from A2
				// rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_GC16);
				rockchip_ebc_refresh(ebc, ctx, true, refresh_waveform);
				/* printk(KERN_INFO "[rockchip_ebc] got one full refresh done"); */
			} else {
				rockchip_ebc_refresh(ebc, ctx, false, default_waveform);
			}

			if (ebc->do_one_full_refresh)
				continue;

			set_current_state(TASK_IDLE);
			if (list_empty(&ctx->queue) && (!kthread_should_stop()) && (!kthread_should_park())){
				/* printk(KERN_INFO "[rockchip_ebc] scheduling"); */
				schedule();
				/* printk(KERN_INFO "[rockchip_ebc] scheduling done"); */
			}
			__set_current_state(TASK_RUNNING);
		}

		/*
		 * Clear the display before disabling the CRTC. Use the
		 * highest-quality waveform to minimize visible artifacts.
		 */
		memcpy(ebc->suspend_next, ctx->prev, ctx->gray4_size);
		// WARNING: This check here does not work. if the ebc device was in
		// runtime suspend at the time of suspending, we get the
		// suspend_was_requested == 1 too late ...
		// Therefore, for now do not differ in the way we treat the screen
		// content. Would be nice to improve this in the future
		if(ebc->suspend_was_requested){
			/* printk(KERN_INFO "[rockchip_ebc] we want to suspend, do something"); */
			memcpy(ctx->final, ebc->off_screen, ctx->gray4_size);
		} else {
			// shutdown/module remove
			/* printk(KERN_INFO "[rockchip_ebc] normal shutdown/module unload"); */
			memcpy(ctx->final, ebc->off_screen, ctx->gray4_size);
		}
		/* memcpy(ctx->final, ebc->off_screen, ctx->gray4_size); */
		rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_GC16);

		// save the prev buffer in case we need it after resuming
		memcpy(ebc->suspend_prev, ctx->prev, ctx->gray4_size);

		if (!kthread_should_stop()){
			kthread_parkme();
		}
	}

	return 0;
}

static inline struct rockchip_ebc *crtc_to_ebc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct rockchip_ebc, crtc);
}

static void rockchip_ebc_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_display_mode mode = crtc->state->adjusted_mode;
	struct drm_display_mode sdck;
	u16 hsync_width, vsync_width;
	u16 hact_start, vact_start;
	u16 pixels_per_sdck;
	bool bus_16bit;

	/*
	 * Hardware needs horizontal timings in SDCK (source driver clock)
	 * cycles, not pixels. Bus width is either 8 bits (normal) or 16 bits
	 * (DRM_MODE_FLAG_CLKDIV2), and each pixel uses two data bits.
	 */
	bus_16bit = !!(mode.flags & DRM_MODE_FLAG_CLKDIV2);
	pixels_per_sdck = bus_16bit ? 8 : 4;
	sdck.hdisplay = mode.hdisplay / pixels_per_sdck;
	sdck.hsync_start = mode.hsync_start / pixels_per_sdck;
	sdck.hsync_end = mode.hsync_end / pixels_per_sdck;
	sdck.htotal = mode.htotal / pixels_per_sdck;
	sdck.hskew = mode.hskew / pixels_per_sdck;

	/*
	 * Linux timing order is display/fp/sync/bp. Hardware timing order is
	 * sync/bp/display/fp, aka sync/start/display/end.
	 */
	hact_start = sdck.htotal - sdck.hsync_start;
	vact_start = mode.vtotal - mode.vsync_start;

	hsync_width = sdck.hsync_end - sdck.hsync_start;
	vsync_width = mode.vsync_end - mode.vsync_start;

	if (dclk_select == -1)
		clk_set_rate(ebc->dclk, mode.clock * 1000);
	else if (dclk_select == 0)
		clk_set_rate(ebc->dclk, 200000000);
	else if (dclk_select == 1)
		clk_set_rate(ebc->dclk, 250000000);

	ebc->dsp_start = EBC_DSP_START_DSP_SDCE_WIDTH(sdck.hdisplay) |
			 EBC_DSP_START_SW_BURST_CTRL;
	regmap_write(ebc->regmap, EBC_EPD_CTRL,
		     EBC_EPD_CTRL_DSP_GD_END(sdck.htotal - sdck.hskew) |
		     EBC_EPD_CTRL_DSP_GD_ST(hsync_width + sdck.hskew) |
		     EBC_EPD_CTRL_DSP_SDDW_MODE * bus_16bit);
	regmap_write(ebc->regmap, EBC_DSP_CTRL,
		     /* no swap */
		     EBC_DSP_CTRL_DSP_SWAP_MODE(bus_16bit ? 2 : 3) |
		     EBC_DSP_CTRL_DSP_SDCLK_DIV(pixels_per_sdck - 1));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING0,
		     EBC_DSP_HTIMING0_DSP_HTOTAL(sdck.htotal) |
		     /* sync end == sync width */
		     EBC_DSP_HTIMING0_DSP_HS_END(hsync_width));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING1,
		     EBC_DSP_HTIMING1_DSP_HACT_END(hact_start + sdck.hdisplay) |
		     /* minus 1 for fixed delay in timing sequence */
		     EBC_DSP_HTIMING1_DSP_HACT_ST(hact_start - 1));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING0,
		     EBC_DSP_VTIMING0_DSP_VTOTAL(mode.vtotal) |
		     /* sync end == sync width */
		     EBC_DSP_VTIMING0_DSP_VS_END(vsync_width));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING1,
		     EBC_DSP_VTIMING1_DSP_VACT_END(vact_start + mode.vdisplay) |
		     EBC_DSP_VTIMING1_DSP_VACT_ST(vact_start));
	regmap_write(ebc->regmap, EBC_DSP_ACT_INFO,
		     EBC_DSP_ACT_INFO_DSP_HEIGHT(mode.vdisplay) |
		     EBC_DSP_ACT_INFO_DSP_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_CTRL,
		     /* FIFO depth - 16 */
		     EBC_WIN_CTRL_WIN2_FIFO_THRESHOLD(496) |
		     EBC_WIN_CTRL_WIN_EN |
		     /* INCR16 */
		     EBC_WIN_CTRL_AHB_BURST_REG(7) |
		     /* FIFO depth - 16 */
		     EBC_WIN_CTRL_WIN_FIFO_THRESHOLD(240) |
		     EBC_WIN_CTRL_WIN_FMT_Y4);

	/* To keep things simple, always use a window size matching the CRTC. */
	regmap_write(ebc->regmap, EBC_WIN_VIR,
		     EBC_WIN_VIR_WIN_VIR_HEIGHT(mode.vdisplay) |
		     EBC_WIN_VIR_WIN_VIR_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_ACT,
		     EBC_WIN_ACT_WIN_ACT_HEIGHT(mode.vdisplay) |
		     EBC_WIN_ACT_WIN_ACT_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_DSP,
		     EBC_WIN_DSP_WIN_DSP_HEIGHT(mode.vdisplay) |
		     EBC_WIN_DSP_WIN_DSP_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_DSP_ST,
		     EBC_WIN_DSP_ST_WIN_DSP_YST(vact_start) |
		     EBC_WIN_DSP_ST_WIN_DSP_XST(hact_start));
}

static int rockchip_ebc_crtc_atomic_check(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct ebc_crtc_state *ebc_crtc_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state->mode_changed)
		return 0;

	if (crtc_state->enable) {
		struct drm_display_mode *mode = &crtc_state->adjusted_mode;

		long rate = 200000000;
		if (dclk_select == -1)
			rate = mode->clock * 1000;
		else if (dclk_select == 0)
			rate = 200000000;
		else if (dclk_select == 1)
			rate = 250000000;

		rate = clk_round_rate(ebc->dclk, rate);
		if (rate < 0)
			return rate;
		mode->clock = rate / 1000;

		ctx = rockchip_ebc_ctx_alloc(mode->hdisplay, mode->vdisplay);
		if (!ctx)
			return -ENOMEM;
	} else {
		ctx = NULL;
	}

	ebc_crtc_state = to_ebc_crtc_state(crtc_state);
	if (ebc_crtc_state->ctx)
		kref_put(&ebc_crtc_state->ctx->kref, rockchip_ebc_ctx_release);
	ebc_crtc_state->ctx = ctx;

	return 0;
}

static void rockchip_ebc_crtc_atomic_flush(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
}

static void rockchip_ebc_crtc_atomic_enable(struct drm_crtc *crtc,
					    struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed)
			kthread_unpark(ebc->refresh_thread);
}

static void rockchip_ebc_crtc_atomic_disable(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed){
		if (! ((ebc->refresh_thread->__state) & (TASK_DEAD))){
			kthread_park(ebc->refresh_thread);
		}
	}
}

static const struct drm_crtc_helper_funcs rockchip_ebc_crtc_helper_funcs = {
	.mode_set_nofb		= rockchip_ebc_crtc_mode_set_nofb,
	.atomic_check		= rockchip_ebc_crtc_atomic_check,
	.atomic_flush		= rockchip_ebc_crtc_atomic_flush,
	.atomic_enable		= rockchip_ebc_crtc_atomic_enable,
	.atomic_disable		= rockchip_ebc_crtc_atomic_disable,
};

static void rockchip_ebc_crtc_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state);

static void rockchip_ebc_crtc_reset(struct drm_crtc *crtc)
{
	struct ebc_crtc_state *ebc_crtc_state;

	if (crtc->state)
		rockchip_ebc_crtc_destroy_state(crtc, crtc->state);

	ebc_crtc_state = kzalloc(sizeof(*ebc_crtc_state), GFP_KERNEL);
	if (!ebc_crtc_state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, &ebc_crtc_state->base);
}

static struct drm_crtc_state *
rockchip_ebc_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct ebc_crtc_state *ebc_crtc_state;

	if (!crtc->state)
		return NULL;

	ebc_crtc_state = kzalloc(sizeof(*ebc_crtc_state), GFP_KERNEL);
	if (!ebc_crtc_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &ebc_crtc_state->base);

	ebc_crtc_state->ctx = to_ebc_crtc_state(crtc->state)->ctx;
	if (ebc_crtc_state->ctx)
		kref_get(&ebc_crtc_state->ctx->kref);

	return &ebc_crtc_state->base;
}

static void rockchip_ebc_crtc_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state)
{
	struct ebc_crtc_state *ebc_crtc_state = to_ebc_crtc_state(crtc_state);

	if (ebc_crtc_state->ctx)
		kref_put(&ebc_crtc_state->ctx->kref, rockchip_ebc_ctx_release);

	__drm_atomic_helper_crtc_destroy_state(&ebc_crtc_state->base);

	kfree(ebc_crtc_state);
}

static const struct drm_crtc_funcs rockchip_ebc_crtc_funcs = {
	.reset			= rockchip_ebc_crtc_reset,
	.destroy		= drm_crtc_cleanup,
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.atomic_duplicate_state	= rockchip_ebc_crtc_duplicate_state,
	.atomic_destroy_state	= rockchip_ebc_crtc_destroy_state,
};

/*
 * Plane
 */

struct ebc_plane_state {
	struct drm_shadow_plane_state	base;
	struct list_head		areas;
};

static inline struct ebc_plane_state *
to_ebc_plane_state(struct drm_plane_state *plane_state)
{
	return container_of(plane_state, struct ebc_plane_state, base.base);
}

static inline struct rockchip_ebc *plane_to_ebc(struct drm_plane *plane)
{
	return container_of(plane, struct rockchip_ebc, plane);
}

static int rockchip_ebc_plane_atomic_check(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_atomic_helper_damage_iter iter;
	struct ebc_plane_state *ebc_plane_state;
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_area *area;
	struct drm_rect clip;
	int ret;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
	if (ret)
		return ret;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		area = kmalloc(sizeof(*area), GFP_KERNEL);
		if (!area)
			return -ENOMEM;

		area->frame_begin = EBC_FRAME_PENDING;
		area->clip = clip;

		drm_dbg(plane->dev, "area %p (" DRM_RECT_FMT ") allocated\n",
			area, DRM_RECT_ARG(&area->clip));

		list_add_tail(&area->list, &ebc_plane_state->areas);
	}

	return 0;
}

static bool rockchip_ebc_blit_fb_r4(const struct rockchip_ebc_ctx *ctx,
				 const struct drm_rect *dst_clip,
				 const void *vaddr,
				 const struct drm_framebuffer *fb,
				 const struct drm_rect *src_clip,
				 int adjust_x1,
				 int adjust_x2
				 )
{
	unsigned int dst_pitch = ctx->gray4_pitch;
	unsigned int src_pitch = fb->pitches[0];
	unsigned int y;
	const void *src;
	void *dst;

	unsigned width = src_clip->x2 - src_clip->x1;
	unsigned int x1_bytes = src_clip->x1 / 2;
	unsigned int x2_bytes = src_clip->x2 / 2;
	width = x2_bytes - x1_bytes;

	src = vaddr + src_clip->y1 * src_pitch + x1_bytes;
	dst = ctx->final + dst_clip->y1 * dst_pitch + dst_clip->x1 / 2;

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		memcpy(dst, src, width);
		dst += dst_pitch;
		src += src_pitch;
	}

	return true;
}

static bool rockchip_ebc_blit_fb_xrgb8888(const struct rockchip_ebc_ctx *ctx,
				 const struct drm_rect *dst_clip,
				 const void *vaddr,
				 const struct drm_framebuffer *fb,
				 const struct drm_rect *src_clip,
				 int adjust_x1,
				 int adjust_x2
				 )
{
	unsigned int dst_pitch = ctx->gray4_pitch;
	unsigned int src_pitch = fb->pitches[0];
	unsigned int start_x, x, y;
	const void *src;
	u8 changed = 0;
	int delta_x;
	void *dst;
	int test1, test2;

	unsigned int delta_y;
	unsigned int start_y;
	unsigned int end_y2;

	// original pattern
	/* int pattern[4][4] = { */
	/* 	{0, 8, 2, 10}, */
	/* 	{12, 4, 14, 6}, */
	/* 	{3, 11, 1,  9}, */
	/* 	{15, 7, 13, 5}, */
	/* }; */
	int pattern[4][4] = {
		{7, 8, 2, 10},
		{12, 4, 14, 6},
		{3, 11, 1,  9},
		{15, 7, 13, 5},
	};

	u8 dither_low = bw_dither_invert ? 15 : 0;
	u8 dither_high = bw_dither_invert ? 0 : 15;
	/* printk(KERN_INFO "dither low/high: %u %u bw_mode: %i\n", dither_low, dither_high, bw_mode); */

	// -2 because we need to go to the beginning of the last line
	start_y = panel_reflection ? src_clip->y1 : src_clip->y2 - 2;
	delta_y = panel_reflection ? 1: -1;

	if (panel_reflection)
		end_y2 = src_clip->y2;
	else
		end_y2 = src_clip->y2 - 1;

	delta_x = panel_reflection ? -1 : 1;
	start_x = panel_reflection ? src_clip->x2 - 1 : src_clip->x1;
	// depending on the direction we must either save the first or the last bit
	test1 = panel_reflection ? adjust_x1 : adjust_x2;
	test2 = panel_reflection ? adjust_x2 : adjust_x1;

	dst = ctx->final + dst_clip->y1 * dst_pitch + dst_clip->x1 / 2;
	src = vaddr + start_y * src_pitch + start_x * fb->format->cpp[0];

	for (y = src_clip->y1; y < end_y2; y++) {
		const u32 *sbuf = src;
		u8 *dbuf = dst;

		for (x = src_clip->x1; x < src_clip->x2; x += 2) {
			u32 rgb0, rgb1;
			u8 gray;
			u8 tmp_pixel;

			rgb0 = *sbuf; sbuf += delta_x;
			rgb1 = *sbuf; sbuf += delta_x;

			/* Truncate the RGB values to 5 bits each. */
			rgb0 &= 0x00f8f8f8U; rgb1 &= 0x00f8f8f8U;
			/* Put the sum 2R+5G+B in bits 24-31. */
			rgb0 *= 0x0020a040U; rgb1 *= 0x0020a040U;
			/* Unbias the value for rounding to 4 bits. */
			rgb0 += 0x07000000U; rgb1 += 0x07000000U;

			rgb0 >>= 28;
			rgb1 >>= 28;

			if (x == src_clip->x1 && (test1 == 1)) {
				// rgb0 should be filled with the content of the src pixel here
				// keep lower 4 bits
				// I'm not sure how to directly read only one byte from the u32
				// pointer dbuf ...
				tmp_pixel = *dbuf & 0b00001111;
				rgb0 = tmp_pixel;
			}
			if (x == src_clip->x2 && (test2 == 1)) {
				// rgb1 should be filled with the content of the dst pixel we
				// want to keep here
				// keep 4 higher bits
				tmp_pixel = *dbuf & 0b11110000;
				// shift by four bits to the lower bits
				rgb1 = tmp_pixel >> 4;
			}

			switch (bw_mode){
				// do nothing for case 0
				case 1:
					/* if (y >= 1800){ */
					/* 	printk(KERN_INFO "bw+dither, before, rgb0 : %i, rgb1: %i\n", rgb0, rgb1); */
					/* } */
					// bw + dithering
					// convert to black and white
					if (rgb0 >= pattern[x & 3][y & 3]){
						rgb0 = dither_high;
					} else {
						rgb0 = dither_low;
					}

					if (rgb1 >= pattern[(x + 1) & 3][y & 3]){
						rgb1 = dither_high;
					} else {
						rgb1 = dither_low;
					}
					/* printk(KERN_INFO "bw+dither, after, rgb0 : %i, rgb1: %i\n", rgb0, rgb1); */
					break;
				case 2:
					// bw
					// convert to black and white
					if (rgb0 >= bw_threshold){
						rgb0 = dither_high;
					} else {
						rgb0 = dither_low;
					}

					if (rgb1 >= bw_threshold){
						rgb1 = dither_high;
					} else {
						rgb1 = dither_low;
					}

					break;
				case 3:
					// downsample to 4 bw values corresponding to the DU4
					// transitions: 0, 5, 10, 15
					if (rgb0 < 4){
						rgb0 = 0;
					} else if (rgb0  < 8){
						rgb0 = 5;
					} else if (rgb0  < 12){
						rgb0 = 10;
					} else {
						rgb0 = 15;
					}

					if (rgb1 < 4){
						rgb1 = 0;
					} else if (rgb1  < 8){
						rgb1 = 5;
					} else if (rgb1  < 12){
						rgb1 = 10;
					} else {
						rgb1 = 15;
					}
			}

			gray = rgb0 | rgb1 << 4;
			changed |= gray ^ *dbuf;
			*dbuf++ = gray;
		}

		dst += dst_pitch;
		if (panel_reflection)
			src += src_pitch;
		else
			src -= src_pitch;
	}

	return !!changed;
}

static void rockchip_ebc_plane_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = plane_to_ebc(plane);
	struct rockchip_ebc_area *area, *next_area;
	struct ebc_plane_state *ebc_plane_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;
	int translate_x, translate_y;
	struct drm_rect src;
	const void *vaddr;
	u64 blit_area = 0;
	int delay;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ctx = to_ebc_crtc_state(crtc_state)->ctx;

	spin_lock(&ctx->queue_lock);
	drm_rect_fp_to_int(&src, &plane_state->src);
	translate_x = plane_state->dst.x1 - src.x1;
	translate_y = plane_state->dst.y1 - src.y1;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	vaddr = ebc_plane_state->base.data[0].vaddr;

	/* printk(KERN_INFO "[rockchip-ebc] new fb clips\n"); */
	list_for_each_entry_safe(area, next_area, &ebc_plane_state->areas, list) {
		struct drm_rect *dst_clip = &area->clip;
		struct drm_rect src_clip = area->clip;
		int adjust_x1;
		int adjust_x2;
		bool clip_changed_fb;
		/* printk(KERN_INFO "[rockchip-ebc]    checking from list: (" DRM_RECT_FMT ") \n", */
		/* 	DRM_RECT_ARG(&area->clip)); */

		/* Convert from plane coordinates to CRTC coordinates. */
		drm_rect_translate(dst_clip, translate_x, translate_y);

		/* Adjust the clips to always process full bytes (2 pixels). */
		/* NOTE: in direct mode, the minimum block size is 4 pixels. */
		if (direct_mode)
			adjust_x1 = dst_clip->x1 & 3;
		else
			adjust_x1 = dst_clip->x1 & 1;

		dst_clip->x1 -= adjust_x1;
		src_clip.x1  -= adjust_x1;

		if (direct_mode)
			adjust_x2 = ((dst_clip->x2 + 3) ^ 3) & 3;
		else
			adjust_x2 = dst_clip->x2 & 1;

		dst_clip->x2 += adjust_x2;
		src_clip.x2  += adjust_x2;

		if (panel_reflection) {
			int x1 = dst_clip->x1, x2 = dst_clip->x2;

			dst_clip->x1 = plane_state->dst.x2 - x2;
			dst_clip->x2 = plane_state->dst.x2 - x1;
		}
		else
		{
			// "normal" mode
			// flip y coordinates
			int y1 = dst_clip->y1, y2 = dst_clip->y2;

			dst_clip->y1 = plane_state->dst.y2 - y2;
			dst_clip->y2 = plane_state->dst.y2 - y1;
		}

		if (limit_fb_blits != 0){
			switch(plane_state->fb->format->format){
				case DRM_FORMAT_XRGB8888:
					clip_changed_fb = rockchip_ebc_blit_fb_xrgb8888(
							ctx, dst_clip, vaddr, plane_state->fb, &src_clip,
							adjust_x1, adjust_x2);
					break;
				case DRM_FORMAT_R4:
					clip_changed_fb = rockchip_ebc_blit_fb_r4(
							ctx, dst_clip, vaddr, plane_state->fb, &src_clip,
							adjust_x1, adjust_x2);
					break;
			}
			// the counter should only reach 0 here, -1 can only be externally set
			limit_fb_blits -= (limit_fb_blits > 0) ? 1 : 0;

			blit_area += (u64) (src_clip.x2 - src_clip.x1) *
				(src_clip.y2 - src_clip.y1);
		} else {
			// we do not want to blit anything
			/* printk(KERN_INFO "[rockchip-ebc] atomic update: not blitting: %i\n", limit_fb_blits); */
			clip_changed_fb = false;
		}

		// reverse coordinates
		dst_clip->x1 += adjust_x1;
		src_clip.x1  += adjust_x1;
		dst_clip->x2 -= adjust_x2;
		src_clip.x2  -= adjust_x2;

		if (!clip_changed_fb) {
			drm_dbg(plane->dev, "area %p (" DRM_RECT_FMT ") <= (" DRM_RECT_FMT ") skipped\n",
				area, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&src_clip));

			/* printk(KERN_INFO "[rockchip-ebc]       clip skipped"); */
			/* Drop the area if the FB didn't actually change. */
			list_del(&area->list);
			kfree(area);
		} else {
			drm_dbg(plane->dev, "area %p (" DRM_RECT_FMT ") <= (" DRM_RECT_FMT ") blitted\n",
				area, DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&src_clip));
			/* printk(KERN_INFO "[rockchip-ebc]        adding to list: (" DRM_RECT_FMT ") <= (" DRM_RECT_FMT ") blitted\n", */
			/* 	DRM_RECT_ARG(&area->clip), DRM_RECT_ARG(&src_clip)); */
		}
	}

	/* uncomment to set the delay depending on the updated area, using a
	 * polynomial of second degree */
	/* delay = (int) (blit_area * blit_area * delay_a / 10000000000 + blit_area * delay_b / 10000 + delay_c); */
	/* a simple threshold function: below a certain updated area, delay by
	 * delay_s [mu s], otherwise delay by delay_b [mu s] */
	delay = delay_a;
	if (blit_area > 100000)
		delay = delay_b;
	/* printk(KERN_INFO "area update, for area %llu we compute a delay of: %i (a,b: %i, %i)", */
	/* 	blit_area, */
	/* 	delay, */
	/* 	delay_a, */
	/* 	delay_b */
	/* ); */

	if (list_empty(&ebc_plane_state->areas)){
		spin_unlock(&ctx->queue_lock);
		// the idea here: give the refresh thread time to acquire the lock
		// before new clips arrive
		/* usleep_range(delay, delay + 500); */
		return;
	}

	/* spin_lock(&ctx->queue_lock); */
	list_splice_tail_init(&ebc_plane_state->areas, &ctx->queue);
	spin_unlock(&ctx->queue_lock);
	// the idea here: give the refresh thread time to acquire the lock
	// before new clips arrive
	/* usleep_range(delay, delay + 100); */

	wake_up_process(ebc->refresh_thread);
}

static const struct drm_plane_helper_funcs rockchip_ebc_plane_helper_funcs = {
	/* .prepare_fb		= drm_gem_prepare_shadow_fb, */
	/* .cleanup_fb		= drm_gem_cleanup_shadow_fb, */
	.begin_fb_access = drm_gem_begin_shadow_fb_access,
	.end_fb_access = drm_gem_end_shadow_fb_access,
	.atomic_check		= rockchip_ebc_plane_atomic_check,
	.atomic_update		= rockchip_ebc_plane_atomic_update,
};

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state);

static void rockchip_ebc_plane_reset(struct drm_plane *plane)
{
	struct ebc_plane_state *ebc_plane_state;

	if (plane->state)
		rockchip_ebc_plane_destroy_state(plane, plane->state);

	ebc_plane_state = kzalloc(sizeof(*ebc_plane_state), GFP_KERNEL);
	if (!ebc_plane_state)
		return;

	__drm_gem_reset_shadow_plane(plane, &ebc_plane_state->base);

	INIT_LIST_HEAD(&ebc_plane_state->areas);
}

static struct drm_plane_state *
rockchip_ebc_plane_duplicate_state(struct drm_plane *plane)
{
	struct ebc_plane_state *ebc_plane_state;

	if (!plane->state)
		return NULL;

	ebc_plane_state = kzalloc(sizeof(*ebc_plane_state), GFP_KERNEL);
	if (!ebc_plane_state)
		return NULL;

	__drm_gem_duplicate_shadow_plane_state(plane, &ebc_plane_state->base);

	INIT_LIST_HEAD(&ebc_plane_state->areas);

	return &ebc_plane_state->base.base;
}

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state)
{
	struct ebc_plane_state *ebc_plane_state = to_ebc_plane_state(plane_state);
	struct rockchip_ebc_area *area, *next_area;

	list_for_each_entry_safe(area, next_area, &ebc_plane_state->areas, list)
		kfree(area);

	__drm_gem_destroy_shadow_plane_state(&ebc_plane_state->base);

	kfree(ebc_plane_state);
}

static const struct drm_plane_funcs rockchip_ebc_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= rockchip_ebc_plane_reset,
	.atomic_duplicate_state	= rockchip_ebc_plane_duplicate_state,
	.atomic_destroy_state	= rockchip_ebc_plane_destroy_state,
};

static const u32 rockchip_ebc_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_R4,
};

static const u64 rockchip_ebc_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int rockchip_ebc_drm_init(struct rockchip_ebc *ebc)
{
	struct drm_device *drm = &ebc->drm;
	struct drm_bridge *bridge;
	int ret;
	const struct firmware * default_offscreen;

	ret = drmm_epd_lut_file_init(drm, &ebc->lut_file, "rockchip/ebc.wbf");
	if (ret)
		return ret;

	ret = drmm_epd_lut_init(&ebc->lut_file, &ebc->lut,
				DRM_EPD_LUT_4BIT_PACKED, EBC_MAX_PHASES);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.max_width = DRM_SHADOW_PLANE_MAX_WIDTH;
	drm->mode_config.max_height = DRM_SHADOW_PLANE_MAX_HEIGHT;
	drm->mode_config.funcs = &rockchip_ebc_mode_config_funcs;
	drm->mode_config.quirk_addfb_prefer_host_byte_order = true;

	drm_plane_helper_add(&ebc->plane, &rockchip_ebc_plane_helper_funcs);
	ret = drm_universal_plane_init(drm, &ebc->plane, 0,
				       &rockchip_ebc_plane_funcs,
				       rockchip_ebc_plane_formats,
				       ARRAY_SIZE(rockchip_ebc_plane_formats),
				       rockchip_ebc_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&ebc->plane);

	drm_crtc_helper_add(&ebc->crtc, &rockchip_ebc_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(drm, &ebc->crtc, &ebc->plane, NULL,
					&rockchip_ebc_crtc_funcs, NULL);
	if (ret)
		return ret;

	ebc->encoder.possible_crtcs = drm_crtc_mask(&ebc->crtc);
	ret = drm_simple_encoder_init(drm, &ebc->encoder, DRM_MODE_ENCODER_NONE);
	if (ret)
		return ret;

	bridge = devm_drm_of_get_bridge(drm->dev, drm->dev->of_node, 0, 0);
	if (IS_ERR(bridge))
		return PTR_ERR(bridge);

	ret = drm_bridge_attach(&ebc->encoder, bridge, NULL, 0);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(drm, 0);

	// check if there is a default off-screen
	if (!request_firmware(&default_offscreen, "rockchip/rockchip_ebc_default_screen.bin", drm->dev))
	{
		if (default_offscreen->size != 1314144)
			drm_err(drm, "Size of default offscreen data file is not 1314144\n");
		else {
			memcpy(ebc->off_screen, default_offscreen->data, 1314144);
		}
	} else {
		// fill the off-screen with some values
		memset(ebc->off_screen, 0xff, 1314144);
		/* memset(ebc->off_screen, 0x00, 556144); */
	}
	release_firmware(default_offscreen);

	return 0;
}

static int __maybe_unused rockchip_ebc_suspend(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;

	ebc->suspend_was_requested = 1;

	ret = drm_mode_config_helper_suspend(&ebc->drm);
	if (ret)
		return ret;

	return pm_runtime_force_suspend(dev);
}

static int __maybe_unused rockchip_ebc_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

	pm_runtime_force_resume(dev);

	return drm_mode_config_helper_resume(&ebc->drm);
}

static int rockchip_ebc_runtime_suspend(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

	regcache_cache_only(ebc->regmap, true);

	clk_disable_unprepare(ebc->dclk);
	clk_disable_unprepare(ebc->hclk);
	regulator_bulk_disable(EBC_NUM_SUPPLIES, ebc->supplies);

	return 0;
}

static int rockchip_ebc_runtime_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;

	ret = regulator_bulk_enable(EBC_NUM_SUPPLIES, ebc->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ebc->hclk);
	if (ret)
		goto err_disable_supplies;

	ret = clk_prepare_enable(ebc->dclk);
	if (ret)
		goto err_disable_hclk;

	/*
	 * Do not restore the LUT registers here, because the temperature or
	 * waveform may have changed since the last refresh. Instead, have the
	 * refresh thread program the LUT during the next refresh.
	 */
	ebc->lut_changed = true;

	regcache_cache_only(ebc->regmap, false);
	regcache_mark_dirty(ebc->regmap);
	regcache_sync(ebc->regmap);

	regmap_write(ebc->regmap, EBC_INT_STATUS,
		     EBC_INT_STATUS_DSP_END_INT_CLR |
		     EBC_INT_STATUS_LINE_FLAG_INT_MSK |
		     EBC_INT_STATUS_DSP_FRM_INT_MSK |
		     EBC_INT_STATUS_FRM_END_INT_MSK);

	return 0;

err_disable_hclk:
	clk_disable_unprepare(ebc->hclk);
err_disable_supplies:
	regulator_bulk_disable(EBC_NUM_SUPPLIES, ebc->supplies);

	return ret;
}

static const struct dev_pm_ops rockchip_ebc_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rockchip_ebc_suspend, rockchip_ebc_resume)
	SET_RUNTIME_PM_OPS(rockchip_ebc_runtime_suspend,
			   rockchip_ebc_runtime_resume, NULL)
};

static bool rockchip_ebc_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EBC_DSP_START:
	case EBC_INT_STATUS:
	case EBC_CONFIG_DONE:
	case EBC_VNUM:
		return true;
	default:
		/* Do not cache the LUT registers. */
		return reg > EBC_WIN_MST2;
	}
}

static const struct regmap_config rockchip_ebc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.volatile_reg	= rockchip_ebc_volatile_reg,
	.max_register	= 0x4ffc, /* end of EBC_LUT_DATA */
	.cache_type	= REGCACHE_FLAT,
};

static const char *const rockchip_ebc_supplies[EBC_NUM_SUPPLIES] = {
	"panel",
	"vcom",
	"vdrive",
};

static irqreturn_t rockchip_ebc_irq(int irq, void *dev_id)
{
	struct rockchip_ebc *ebc = dev_id;
	unsigned int status;

	regmap_read(ebc->regmap, EBC_INT_STATUS, &status);

	if (status & EBC_INT_STATUS_DSP_END_INT_ST) {
		status |= EBC_INT_STATUS_DSP_END_INT_CLR;
		complete(&ebc->display_end);
	}

	regmap_write(ebc->regmap, EBC_INT_STATUS, status);

	return IRQ_HANDLED;
}

static int rockchip_ebc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_ebc *ebc;
	void __iomem *base;
	int i, ret;

	ebc = devm_drm_dev_alloc(dev, &rockchip_ebc_drm_driver,
				 struct rockchip_ebc, drm);
	ebc->do_one_full_refresh = true;
	ebc->suspend_was_requested = 0;

	spin_lock_init(&ebc->refresh_once_lock);

	if (IS_ERR(ebc))
		return PTR_ERR(ebc);

	platform_set_drvdata(pdev, ebc);
	init_completion(&ebc->display_end);
	ebc->reset_complete = skip_reset;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ebc->regmap = devm_regmap_init_mmio(dev, base,
					     &rockchip_ebc_regmap_config);
	if (IS_ERR(ebc->regmap))
		return PTR_ERR(ebc->regmap);

	regcache_cache_only(ebc->regmap, true);

	ebc->dclk = devm_clk_get(dev, "dclk");
	if (IS_ERR(ebc->dclk))
		return dev_err_probe(dev, PTR_ERR(ebc->dclk),
				     "Failed to get dclk\n");

	ebc->hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(ebc->hclk))
		return dev_err_probe(dev, PTR_ERR(ebc->hclk),
				     "Failed to get hclk\n");

	ebc->temperature_channel = devm_iio_channel_get(dev, NULL);
	if (IS_ERR(ebc->temperature_channel))
		return dev_err_probe(dev, PTR_ERR(ebc->temperature_channel),
				     "Failed to get temperature I/O channel\n");

	for (i = 0; i < EBC_NUM_SUPPLIES; i++)
		ebc->supplies[i].supply = rockchip_ebc_supplies[i];

	ret = devm_regulator_bulk_get(dev, EBC_NUM_SUPPLIES, ebc->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       rockchip_ebc_irq, 0, dev_name(dev), ebc);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	pm_runtime_set_autosuspend_delay(dev, EBC_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		ret = rockchip_ebc_runtime_resume(&pdev->dev);
		if (ret)
			return ret;
	}

	ebc->refresh_thread = kthread_create(rockchip_ebc_refresh_thread,
					     ebc, "ebc-refresh/%s",
					     dev_name(dev));
	if (IS_ERR(ebc->refresh_thread)) {
		ret = dev_err_probe(dev, PTR_ERR(ebc->refresh_thread),
				    "Failed to start refresh thread\n");
		goto err_disable_pm;
	}

	kthread_park(ebc->refresh_thread);
	sched_set_fifo(ebc->refresh_thread);

	ret = rockchip_ebc_drm_init(ebc);
	if (ret)
		goto err_stop_kthread;

	return 0;

err_stop_kthread:
	kthread_stop(ebc->refresh_thread);
err_disable_pm:
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);

	return ret;
}

static int rockchip_ebc_remove(struct platform_device *pdev)
{
	struct rockchip_ebc *ebc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	drm_dev_unregister(&ebc->drm);
	kthread_stop(ebc->refresh_thread);
	drm_atomic_helper_shutdown(&ebc->drm);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);

	return 0;
}

static void rockchip_ebc_shutdown(struct platform_device *pdev)
{
	struct rockchip_ebc *ebc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	kthread_stop(ebc->refresh_thread);
	drm_atomic_helper_shutdown(&ebc->drm);

	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);
}

static const struct of_device_id rockchip_ebc_of_match[] = {
	{ .compatible = "rockchip,rk3568-ebc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rockchip_ebc_of_match);

static struct platform_driver rockchip_ebc_driver = {
	.probe		= rockchip_ebc_probe,
	.remove		= rockchip_ebc_remove,
	.shutdown	= rockchip_ebc_shutdown,
	.driver		= {
		.name		= "rockchip-ebc",
		.of_match_table	= rockchip_ebc_of_match,
		.pm		= &rockchip_ebc_dev_pm_ops,
	},
};
module_platform_driver(rockchip_ebc_driver);

MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>");
MODULE_DESCRIPTION("Rockchip EBC driver");
MODULE_LICENSE("GPL v2");

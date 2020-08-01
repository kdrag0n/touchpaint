// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Danny Lin <danny@kdrag0n.dev>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/slab.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

#define MAX_FINGERS 10

struct point {
	int x;
	int y;
};

enum tp_mode {
	MODE_PAINT,
	MODE_FILL,
	MODE_BOX,
	MODE_FOLLOW,
	MODE_MAX
};

/* Config */
static phys_addr_t fb_phys_addr = 0x9c000000;
static size_t fb_max_size = 0x02400000;
/* Pixel format is assumed to be ARGB_8888 */
static int fb_width = 1080;
static int fb_height = 2340;
static enum tp_mode mode = MODE_PAINT;
module_param(mode, int, 0644);
/* Brush size in pixels - odd = slower but centered, even = faster but not centered */
static int brush_size = 2;
module_param(brush_size, int, 0644);
static int follow_box_size = 301;
module_param(follow_box_size, int, 0644);
/* Paint clear delay in ms. 0 = on next touch, -1 = never */
static int paint_clear_delay = 0;

/* State */
static u32 __iomem *fb_mem;
static size_t fb_size;
static bool init_done;
static unsigned int fingers;
static struct point slots[MAX_FINGERS];
static bool finger_down[MAX_FINGERS];
static struct point last_point[MAX_FINGERS];
static struct task_struct *box_thread;

static void blank_screen(void)
{
	memset(fb_mem, 0, fb_size);
}

static void blank_callback(unsigned long data)
{
	blank_screen();
}
static DEFINE_TIMER(blank_timer, blank_callback, 0, 0);

static void fill_screen_white(void)
{
	memset(fb_mem, 0xffffffff, fb_size);
}

static size_t point_to_offset(int x, int y)
{
	return x + (y * fb_width);
}

static u32 rgb_to_pixel(u8 r, u8 g, u8 b)
{
	u32 pixel = 0xff000000;
	pixel |= r << 16;
	pixel |= g << 8;
	pixel |= b;

	return pixel;
}

static void set_pixel(size_t offset_px, u32 pixel)
{
	*(volatile u32 *)(fb_mem + offset_px) = pixel;
}

static void set_2pixels(size_t offset_px, u32 pixel)
{
	u64 pixels = ((u64)pixel << 32) | pixel;
	*(volatile u64 *)(fb_mem + offset_px) = pixels;
}

#if defined(CONFIG_ARCH_SUPPORTS_INT128) && defined(__SIZEOF_INT128__)
static void set_4pixels(size_t offset_px, u32 pixel32)
{
	unsigned __int128 pixel128 = (unsigned __int128)pixel32;
	unsigned __int128 pixels = (pixel128 << 96) | (pixel128 << 64) |
		(pixel128 << 32) | pixel128;

	*(volatile unsigned __int128 *)(fb_mem + offset_px) = pixels;
}
#endif

static int draw_pixels(int x, int y, int count, u8 r, u8 g, u8 b)
{
	size_t offset_px = point_to_offset(x, y);
	u32 pixel = rgb_to_pixel(r, g, b);

	pr_debug("draw pixels: x=%d y=%d offset=%zupx count=%d color=(%d, %d, %d)\n",
		 x, y, offset_px, count, r, g, b);

#if defined(CONFIG_ARCH_SUPPORTS_INT128) && defined(__SIZEOF_INT128__)
	if (count >= 4) {
		set_4pixels(offset_px, pixel);
		return 4;
	}
#endif

	if (count >= 2) {
		set_2pixels(offset_px, pixel);
		return 2;
	}

	if (count >= 1) {
		set_pixel(offset_px, pixel);
		return 1;
	}

	return 0;
}

static void draw_h_line(int x, int y, int length, u8 r, u8 g, u8 b)
{
	int target_x = min(x + length, fb_width);
	int cur_x = x;

	pr_debug("draw horizontal line: x=%d y=%d length=%d r=%d g=%d b=%d\n",
		 x, y, length, r, g, b);
	while (cur_x < target_x) {
		int remaining_px = target_x - cur_x;
		cur_x += draw_pixels(cur_x, y, remaining_px, r, g, b);
	}
}

static void draw_point(int x, int y, int size, u8 r, u8 g, u8 b)
{
	int radius = max(1, (size - 1) / 2);
	int base_x = clamp(x - radius, 0, fb_width);
	int base_y = clamp(y - radius, 0, fb_height);
	int off_y;

	pr_debug("draw point: x=%d y=%d size=%d r=%d g=%d b=%d\n", x, y, size, r, g, b);
	for (off_y = 0; off_y < size; off_y++) {
		draw_h_line(base_x, base_y + off_y, size, r, g, b);
	}
}

static void fill_screen(u8 r, u8 g, u8 b)
{
	int y;

	for (y = 0; y < fb_height; y++) {
		int x = 0;

		while (x < fb_width) {
			x += draw_pixels(x, y, fb_width - x, r, g, b);
		}
	}
}

static void draw_vert_point_damage(int size, int x1, int y1, int x2, int y2,
			      u8 fg_r, u8 fg_g, u8 fg_b,
			      u8 bg_r, u8 bg_g, u8 bg_b)
{
	int radius = max(1, (size - 1) / 2);
	int base_x = clamp(x1 - radius, 0, fb_width);
	int dx = x2 - x1;
	int dy = y2 - y1;
	int off_x = 0;
	int off_y;

	do {
		for (off_y = 0; off_y < abs(dy); off_y++) {
			if (dy < 0) {
				/* Going up */
				draw_h_line(base_x + off_x, y1 + radius + off_y, size,
					    bg_r, bg_g, bg_b);
				draw_h_line(base_x + off_x, y1 - radius - off_y, size,
					    fg_r, fg_g, fg_b);
			} else {
				/* Going down */
				draw_h_line(base_x + off_x, y1 - radius - off_y, size,
					    bg_r, bg_g, bg_b);
				draw_h_line(base_x + off_x, y1 + radius + off_y, size,
					    fg_r, fg_g, fg_b);
			}
		}

		off_x++;
	} while (off_x < abs(dx));
}

static int box_thread_func(void *data)
{
	static const struct sched_param rt_prio = { .sched_priority = 1 };
	int x = fb_width / 2;
	int y = fb_height / 12;
	int step = 7;
	int size = 301;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &rt_prio);

	fill_screen(64, 0, 128);
	draw_point(x, y, size, 255, 255, 0);

	while (!kthread_should_stop()) {
		if (y > fb_height - (fb_height / 12) || y < fb_height / 12)
			step *= -1;

		/* Draw damage rather than redrawing the entire box */
		draw_vert_point_damage(size, x, y, x, y + step, 255, 255, 0, 64, 0, 128);

		y += step;
		usleep_range(8000, 8000);
	}

	return 0;
}

static void start_box_thread(void)
{
	if (box_thread)
		return;

	box_thread = kthread_run(box_thread_func, NULL, "touchpaint_box");
	if (IS_ERR(box_thread)) {
		pr_err("failed to start box thread! err=%d\n", PTR_ERR(box_thread));
		box_thread = NULL;
	}
}

static void stop_box_thread(void)
{
	int ret;

	if (!box_thread)
		return;

	ret = kthread_stop(box_thread);
	if (ret)
		pr_err("failed to stop box thread! err=%d\n", ret);

	box_thread = NULL;
}

static void touchpaint_finger_down(int slot)
{
	if (!init_done || finger_down[slot])
		return;

	pr_debug("finger %d down\n", slot);
	finger_down[slot] = true;

	if (++fingers == 1) {
		switch (mode) {
		case MODE_PAINT:
			if (paint_clear_delay > 0)
				del_timer(&blank_timer);
			else if (paint_clear_delay == 0)
				blank_screen();

			break;
		case MODE_FILL:
			del_timer(&blank_timer);
			fill_screen_white();
			break;
		case MODE_BOX:
			if (box_thread) {
				stop_box_thread();
				blank_screen();
			} else {
				start_box_thread();
			}

			break;
		default:
			break;
		}
	}
}

static void touchpaint_finger_up(int slot)
{
	if (!init_done || !finger_down[slot])
		return;

	pr_debug("finger %d up\n", slot);

	if (--fingers == 0) {
		if (mode == MODE_FILL)
			mod_timer(&blank_timer, jiffies + msecs_to_jiffies(250));
		else if (mode == MODE_PAINT && paint_clear_delay > 0)
			mod_timer(&blank_timer,
				  jiffies + msecs_to_jiffies(paint_clear_delay));
	}

	if (mode == MODE_FOLLOW) {
		draw_point(last_point[slot].x, last_point[slot].y, follow_box_size,
			   0, 0, 0);
	}

	finger_down[slot] = false;
	last_point[slot].x = 0;
	last_point[slot].y = 0;
}

/*
 * Bresenham's line drawing algorithm
 * Source: https://rosettacode.org/wiki/Bitmap/Bresenham%27s_line_algorithm#C
 */
static void draw_line(int x1, int y1, int x2, int y2, u8 r, u8 g, u8 b)
{
	int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
	int dy = abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
	int err = (dx > dy ? dx : -dy) / 2;
	int err2;
	int x = x1, y = y1;

	while (true) {
		draw_point(x, y, brush_size, r, g, b);

		if (x == x2 && y == y2)
			break;

		err2 = err;
		if (err2 > -dx) {
			err -= dy;
			x += sx;
		}

		if (err2 < dy) {
			err += dx;
			y += sy;
		}
	}
}

static void touchpaint_finger_point(int slot, int x, int y)
{
	if (!init_done || !finger_down[slot])
		return;

	switch (mode) {
	case MODE_PAINT:
		draw_point(x, y, brush_size, 255, 255, 255);

		if (last_point[slot].x && last_point[slot].y)
			draw_line(last_point[slot].x, last_point[slot].y, x, y,
				  255, 255, 255);

		break;
	case MODE_FOLLOW:
		/* Just draw a box for the first point */
		if (!last_point[slot].x && !last_point[slot].y) {
			draw_point(x, y, follow_box_size, 255, 255, 255);
			break;
		}

		/* Clear old point and draw new point */
		draw_point(last_point[slot].x, last_point[slot].y, follow_box_size,
			   0, 0, 0);
		draw_point(x, y, follow_box_size, 255, 255, 255);
		break;
	default:
		break;
	}

	last_point[slot].x = x;
	last_point[slot].y = y;
}

static void touchpaint_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	static int slot = 0;

	pr_debug("input event: type=%u code=%u val=%d\n", type, code, value);

	if (type == EV_KEY && code == KEY_VOLUMEUP && value == 1) {
		/* Box needs to be stopped before cycling to prevent artifacts */
		if (mode == MODE_BOX)
			stop_box_thread();

		/* Cycle mode */
		if (++mode == MODE_MAX)
			mode = 0;

		blank_screen();
	} else if (type == EV_ABS) {
		switch (code) {
		case ABS_MT_SLOT:
			slot = value;
			break;
		case ABS_MT_POSITION_X:
			slots[slot].x = value;
			break;
		case ABS_MT_POSITION_Y:
			slots[slot].y = value;
			break;
		case ABS_MT_TRACKING_ID:
			if (value == -1) {
				touchpaint_finger_up(slot);
				slots[slot].x = -1;
				slots[slot].y = -1;
			}

			break;
		default:
			break;
		}
	}

	if ((type == EV_ABS && code == ABS_MT_SLOT) ||
	    (type == EV_SYN && code == SYN_REPORT)) {
		if (slots[slot].x != -1 && slots[slot].y != -1) {
			touchpaint_finger_down(slot);
			touchpaint_finger_point(slot, slots[slot].x, slots[slot].y);
		}
	}
}

static int touchpaint_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = KBUILD_MODNAME;

	ret = input_register_handle(handle);
	if (ret)
		goto err2;

	ret = input_open_device(handle);
	if (ret)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return ret;
}

static void touchpaint_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id touchpaint_ids[] = {
	/* Volume-up key */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_VOLUMEUP)] = BIT_MASK(KEY_VOLUMEUP) },
	},
	/* Touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{ }
};

static struct input_handler touchpaint_input_handler = {
	.event          = touchpaint_input_event,
	.connect        = touchpaint_input_connect,
	.disconnect     = touchpaint_input_disconnect,
	.name           = KBUILD_MODNAME,
	.id_table       = touchpaint_ids,
};

static int __init touchpaint_init(void)
{
	int ret;
	int i;

	fb_mem = ioremap_wc(fb_phys_addr, fb_max_size);
	if (!fb_mem) {
		pr_err("failed to map %zu-byte framebuffer at %pa!\n", fb_max_size,
		       &fb_phys_addr);
		return -ENOMEM;
	}

	fb_size = min((size_t)(fb_width * fb_height * 4), fb_max_size);

	pr_info("using %dx%d framebuffer spanning %zu bytes at %pa (mapped to %px)\n",
		fb_width, fb_height, fb_size, &fb_phys_addr, fb_mem);
	blank_screen();

	for (i = 0; i < MAX_FINGERS; i++) {
		slots[i].x = -1;
		slots[i].y = -1;
	}

	ret = input_register_handler(&touchpaint_input_handler);
	if (ret)
		pr_err("failed to register input handler! err=%d\n", ret);

	init_done = 1;
	return 0;
}
late_initcall_sync(touchpaint_init);

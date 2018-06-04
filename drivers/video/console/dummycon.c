/*
 *  linux/drivers/video/dummycon.c -- A dummy console driver
 *
 *  To be used if there's no other console driver (e.g. for plain VGA text)
 *  available, usually until fbcon takes console over.
 */

#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/console.h>
#include <linux/vt_kern.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <asm/sbi.h>
#include <asm/lowrisc.h>

/*
 *  Dummy console driver
 */

#if defined(__arm__)
#define DUMMY_COLUMNS	screen_info.orig_video_cols
#define DUMMY_ROWS	screen_info.orig_video_lines
#else
/* set by Kconfig. Use 80x25 for 640x480 and 160x64 for 1280x1024 */
#define DUMMY_COLUMNS	CONFIG_DUMMY_CONSOLE_COLUMNS
#define DUMMY_ROWS	CONFIG_DUMMY_CONSOLE_ROWS
#endif

static int oldxpos, oldypos;
static uint8_t *hid_vga_ptr;

static const char *dummycon_startup(void)
{
    return "dummy device";
}

static void dummycon_init(struct vc_data *vc, int init)
{
    vc->vc_can_do_color = 0;
    if (init) {
	vc->vc_cols = DUMMY_COLUMNS;
	vc->vc_rows = DUMMY_ROWS;
	hid_vga_ptr = ioremap(vga_base_addr, 0x1000);
    } else
	vc_resize(vc, DUMMY_COLUMNS, DUMMY_ROWS);
}

static void dummycon_deinit(struct vc_data *vc) { }

static void dummycon_clear(struct vc_data *vc, int sy, int sx, int height, int width)
{
  oldxpos = 0;
  oldypos = 0;
  sbi_console_putchar('\f');
}

static void dummycon_putc(struct vc_data *vc, int c, int ypos, int xpos)
{
  if (xpos < oldxpos)
    {
      sbi_console_putchar('\r');
    }
  if (ypos > oldypos)
    {
      sbi_console_putchar('\n');
    }
  sbi_console_putchar(0x7f & c);
  hid_vga_ptr[128*ypos+xpos] = c;
  oldxpos = xpos;
  oldypos = ypos;
}

static void dummycon_putcs(struct vc_data *vc, const unsigned short *s, int count, int ypos, int xpos)
{
  while (count--) dummycon_putc(vc, *s++, ypos, xpos++);
}

static void dummycon_cursor(struct vc_data *vc, int mode) { }

static bool dummycon_scroll(struct vc_data *vc, unsigned int top,
			    unsigned int bottom, enum con_scroll dir,
			    unsigned int lines)
{
  oldxpos = 0;
  oldypos = 0;
  memcpy(hid_vga_ptr, hid_vga_ptr+128, 4096-128);
  memset(hid_vga_ptr+4096-128, ' ', 128);
  return true;
}

static int dummycon_switch(struct vc_data *vc)
{
	return 0;
}

static int dummycon_blank(struct vc_data *vc, int blank, int mode_switch)
{
	return 0;
}

static int dummycon_font_set(struct vc_data *vc, struct console_font *font,
			     unsigned int flags)
{
	return 0;
}

static int dummycon_font_default(struct vc_data *vc,
				 struct console_font *font, char *name)
{
	return 0;
}

static int dummycon_font_copy(struct vc_data *vc, int con)
{
	return 0;
}

/*
 *  The console `switch' structure for the dummy console
 *
 *  Most of the operations are dummies.
 */

const struct consw dummy_con = {
	.owner =		THIS_MODULE,
	.con_startup =	dummycon_startup,
	.con_init =		dummycon_init,
	.con_deinit =	dummycon_deinit,
	.con_clear =	dummycon_clear,
	.con_putc =		dummycon_putc,
	.con_putcs =	dummycon_putcs,
	.con_cursor =	dummycon_cursor,
	.con_scroll =	dummycon_scroll,
	.con_switch =	dummycon_switch,
	.con_blank =	dummycon_blank,
	.con_font_set =	dummycon_font_set,
	.con_font_default =	dummycon_font_default,
	.con_font_copy =	dummycon_font_copy,
};
EXPORT_SYMBOL_GPL(dummy_con);

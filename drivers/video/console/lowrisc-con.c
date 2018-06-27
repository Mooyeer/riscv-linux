/*
 *  linux/drivers/video/lowrisc_con.c -- A lowrisc console driver
 *
 *  Based on dummycon.c (for plain VGA text)
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

#define DRIVER_NAME     "lowrisc-vga"

/*
 *  Lowrisc console driver
 */

#define LOWRISC_COLUMNS	128
#define LOWRISC_ROWS	32

static int oldxpos, oldypos;
static uint16_t *hid_vga_ptr;

static const char *lowrisc_con_startup(void)
{
    return "lowrisc device";
}

static void lowrisc_con_init(struct vc_data *vc, int init)
{
    vc->vc_can_do_color = 1;
    if (init) {
	vc->vc_cols = LOWRISC_COLUMNS;
	vc->vc_rows = LOWRISC_ROWS;
    } else
	vc_resize(vc, LOWRISC_COLUMNS, LOWRISC_ROWS);
}

static void lowrisc_con_deinit(struct vc_data *vc) { }

static void lowrisc_con_clear(struct vc_data *vc, int sy, int sx, int height, int width)
{
  oldxpos = 0;
  oldypos = 0;
  sbi_console_putchar('\f');
}

static void lowrisc_con_putc(struct vc_data *vc, int c, int ypos, int xpos)
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

static void lowrisc_con_putcs(struct vc_data *vc, const unsigned short *s, int count, int ypos, int xpos)
{
  while (count--) lowrisc_con_putc(vc, *s++, ypos, xpos++);
}

static void lowrisc_con_cursor(struct vc_data *vc, int mode) { }

static bool lowrisc_con_scroll(struct vc_data *vc, unsigned int top,
			    unsigned int bottom, enum con_scroll dir,
			    unsigned int lines)
{
  oldxpos = 0;
  oldypos = 0;
  memcpy(hid_vga_ptr, hid_vga_ptr+128, 4096-128);
  memset(hid_vga_ptr+4096-128, ' ', 128);
  return true;
}

static int lowrisc_con_switch(struct vc_data *vc)
{
	return 0;
}

static int lowrisc_con_blank(struct vc_data *vc, int blank, int mode_switch)
{
	return 0;
}

static int lowrisc_con_font_set(struct vc_data *vc, struct console_font *font,
			     unsigned int flags)
{
	return 0;
}

static int lowrisc_con_font_default(struct vc_data *vc,
				 struct console_font *font, char *name)
{
	return 0;
}

static int lowrisc_con_font_copy(struct vc_data *vc, int con)
{
	return 0;
}

/*
 *  The console `switch' structure for the lowrisc console
 *
 *  Most of the operations are dummies.
 */

const struct consw lowrisc_con = {
	.owner =		THIS_MODULE,
	.con_startup =	lowrisc_con_startup,
	.con_init =		lowrisc_con_init,
	.con_deinit =	lowrisc_con_deinit,
	.con_clear =	lowrisc_con_clear,
	.con_putc =		lowrisc_con_putc,
	.con_putcs =	lowrisc_con_putcs,
	.con_cursor =	lowrisc_con_cursor,
	.con_scroll =	lowrisc_con_scroll,
	.con_switch =	lowrisc_con_switch,
	.con_blank =	lowrisc_con_blank,
	.con_font_set =	lowrisc_con_font_set,
	.con_font_default =	lowrisc_con_font_default,
	.con_font_copy =	lowrisc_con_font_copy,
};

static int lowrisc_con_probe(struct platform_device *ofdev)
{
        struct resource *lowrisc_vga;
	int rc = 0;

        lowrisc_vga = platform_get_resource(ofdev, IORESOURCE_MEM, 0);

        hid_vga_ptr = devm_ioremap_resource(&ofdev->dev, lowrisc_vga);

	printk(DRIVER_NAME": Lowrisc VGA console (%llX-%llX) mapped to %lx\n",
               lowrisc_vga[0].start,
               lowrisc_vga[0].end,
               (size_t)(hid_vga_ptr));

        console_lock();
        rc = do_take_over_console(&lowrisc_con, 0, MAX_NR_CONSOLES - 1, 1);
        console_unlock();
	
	return rc;
}

/* Match table for OF platform binding */
static const struct of_device_id lowrisc_con_of_match[] = {
	{ .compatible = DRIVER_NAME },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, lowrisc_con_of_match);

void lowrisc_con_free(struct platform_device *of_dev)
{
        struct resource *iomem = platform_get_resource(of_dev, IORESOURCE_MEM, 0);
        release_mem_region(iomem->start, resource_size(iomem));
}

int lowrisc_con_unregister(struct platform_device *of_dev)
{
        lowrisc_con_free(of_dev);
        return 0;
}

static struct platform_driver lowrisc_con_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = lowrisc_con_of_match,
	},
	.probe = lowrisc_con_probe,
	.remove = lowrisc_con_unregister,
};

module_platform_driver(lowrisc_con_driver);

MODULE_AUTHOR("Jonathan Kimmitt");
MODULE_DESCRIPTION("Lowrisc VGA-compatible console driver");
MODULE_LICENSE("GPL");

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/console.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <asm/sbi.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/spinlock.h>

struct lowrisc_sbi_con {
  struct platform_device *pdev;
  struct resource *keyb, *vid, *uart;
  spinlock_t lock;
  volatile uint32_t *keyb_base;
  volatile uint32_t *vid_base;
  volatile uint32_t *uart_base;
  void __iomem *ioaddr; /* mapped address */
  int keyb_irq;
  int int_en;
};

static DEFINE_SPINLOCK(xuart_tty_port_lock);
static struct tty_port xuart_tty_port;
static struct tty_driver *xuart_tty_driver;
static volatile uint32_t xuart_ref_cnt;
static int keyb_irq;

void xuart_putchar(int data)
{
  sbi_console_putchar(data&0x7f);
}

static void minion_console_putchar(unsigned char ch)
{
  xuart_putchar(ch);
}

static int xuart_tty_open(struct tty_struct *tty, struct file *filp)
{
  xuart_ref_cnt++;
  return 0;
}

static void xuart_tty_close(struct tty_struct *tty, struct file *filp)
{
  if (xuart_ref_cnt)
      xuart_ref_cnt--;
}

static void xuart_console_poll(struct lowrisc_sbi_con *con)
{
  int ch = sbi_console_getchar();
  if (ch && xuart_ref_cnt)
    {
      //      sbi_console_putchar(ch);
      spin_lock(&xuart_tty_port_lock);
      tty_insert_flip_char(&xuart_tty_port, ch, TTY_NORMAL);
      tty_flip_buffer_push(&xuart_tty_port);
      spin_unlock(&xuart_tty_port_lock);      
    }
}

static irqreturn_t xuart_console_irq(int irq, void *dev_id)
{
  struct lowrisc_sbi_con *con = (struct lowrisc_sbi_con *)dev_id;
  irqreturn_t ret;
  ret = IRQ_NONE;
  return ret;
}

#if 0
/* Timer callback */
void timer_callback(struct timer_list *unused)
{
  sbi_console_puts("keyb_timer callback\n");
  mod_timer(&keyb_timer, jiffies + 6); /* restarting timer */
}

/* Init the timer */
static void keyb_init_timer(void)
{
    timer_setup(&keyb_timer, timer_callback, jiffies + 6); /* Starting the timer */ 
    sbi_console_puts("keyb_timer is started");
}
#endif

static int xuart_tty_write(struct tty_struct *tty,
	const unsigned char *buf, int count)
{
	const unsigned char *end;

	for (end = buf + count; buf < end; buf++) {
		minion_console_putchar(*buf);
	}
	return count;
}

static int xuart_tty_write_room(struct tty_struct *tty)
{
	return 1024; /* arbitrary */
}

static const struct tty_operations xuart_tty_ops = {
	.open		= xuart_tty_open,
	.close		= xuart_tty_close,
	.write		= xuart_tty_write,
	.write_room	= xuart_tty_write_room,
};

static int lowrisc_sbi_remove(struct platform_device *pdev)
{
  return 0;
}

static const struct of_device_id lowrisc_sbi_of_match[] = {
  { .compatible = "riscv,lowrisc" },
  { }
};

MODULE_DEVICE_TABLE(of, lowrisc_sbi_of_match);

static int lowrisc_sbi_probe(struct platform_device *pdev)
{
  struct lowrisc_sbi_con *con;
  printk("SBI console probe beginning\n");
  con = kzalloc(sizeof(struct lowrisc_sbi_con), GFP_KERNEL);
  if (!con) {
    return -ENOMEM;
  }
  printk("SBI console probed and mapped\n");
  return 0;
}

static struct platform_driver lowrisc_sbi_driver = {
  .driver = {
    .name = "lowrisc_sbi_console",
    .of_match_table = lowrisc_sbi_of_match,
  },
  .probe = lowrisc_sbi_probe,
  .remove = lowrisc_sbi_remove,
};

module_platform_driver(lowrisc_sbi_driver);

static struct resource lowrisc_hid[] = {
};

static struct platform_device xuart_device = {
        .name = "lowrisc_sbi_console",
        .id = -1, /* Bus number */
        .num_resources = ARRAY_SIZE(lowrisc_hid),
        .resource = lowrisc_hid,
};

static int __init xuart_console_init(void)
{
  return platform_device_register(&xuart_device);
}

static void __exit xuart_console_exit(void)
{
  tty_unregister_driver(xuart_tty_driver);
  put_tty_driver(xuart_tty_driver);
}

module_init(xuart_console_init);
module_exit(xuart_console_exit);

MODULE_DESCRIPTION("RISC-V SBI console driver");
MODULE_LICENSE("GPL");


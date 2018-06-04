/*
 * Lowrisc Keyboard Controller Driver
 * http://www.lowrisc.org/
 *
 * based on opencores Javier Herrero <jherrero@hvsistemas.es>
 * Copyright 2007-2009 HV Sistemas S.L.
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input-polldev.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <linux/uaccess.h>
#include <asm/lowrisc.h>
#include <asm/sbi.h>

struct lowrisc_kbd {
  struct platform_device *pdev;
  struct resource *keyb;
  spinlock_t lock;
  volatile uint32_t *keyb_base;
  struct input_dev *input;
  unsigned short keycodes[128];
};

static void lowrisc_keys_poll(struct input_polled_dev *dev)
{
  struct lowrisc_kbd *lowrisc_kbd = dev->private;
  struct input_dev *input = dev->input;
  unsigned char c;
  uint32_t key = *lowrisc_kbd->keyb_base;
  if ((1<<16) & ~key)
    {
      *lowrisc_kbd->keyb_base = 0; // bump FIFO location
      key = *lowrisc_kbd->keyb_base;
      c = key & 0x7F; /* strip off the scan code (default ascii code is UK) */
      input_report_key(input, c, 1);
      input_sync(input);
      input_report_key(input, c, 0);
      input_sync(input);
      //      printk("input_report_key %x entered as %c\n", c, key >> 8);
      //      sbi_console_putchar(key >> 8);
    }
}

static int lowrisc_kbd_probe(struct platform_device *pdev)
{
  struct input_dev *input;
  struct lowrisc_kbd *lowrisc_kbd;
  int i, error;
  struct input_polled_dev *poll_dev;
  struct device *dev = &pdev->dev;

  printk("lowrisc_kbd_probe\n");
  lowrisc_kbd = devm_kzalloc(&pdev->dev, sizeof(struct lowrisc_kbd), GFP_KERNEL);
  if (!lowrisc_kbd) {
    return -ENOMEM;
  }
  lowrisc_kbd->keyb = platform_get_resource(pdev, IORESOURCE_MEM, 0);
  if (!request_mem_region(lowrisc_kbd->keyb->start, resource_size(lowrisc_kbd->keyb), "lowrisc_kbd"))
    {
    dev_err(&pdev->dev, "cannot request LowRISC keyboard region\n");
    return -EBUSY;
    }
  lowrisc_kbd->keyb_base = (volatile uint32_t *)ioremap(lowrisc_kbd->keyb->start, resource_size(lowrisc_kbd->keyb));
  printk("hid_keyboard address %llx, remapped to %lx\n", lowrisc_kbd->keyb->start, (size_t)lowrisc_kbd->keyb_base);

  poll_dev = devm_input_allocate_polled_device(dev);
  if (!poll_dev) {
    dev_err(dev, "failed to allocate input device\n");
    return -ENOMEM;
  }
  
  poll_dev->poll_interval = 100;
  
  poll_dev->poll = lowrisc_keys_poll;
  poll_dev->private = lowrisc_kbd;
  
  input = poll_dev->input;

  lowrisc_kbd->input = input;

  input->name = pdev->name;
  input->phys = "lowrisc-kbd/input0";
  
  input->id.bustype = BUS_HOST;
  input->id.vendor = 0x0001;
  input->id.product = 0x0001;
  input->id.version = 0x0100;
  
  input->keycode = lowrisc_kbd->keycodes;
  input->keycodesize = sizeof(lowrisc_kbd->keycodes[0]);
  input->keycodemax = ARRAY_SIZE(lowrisc_kbd->keycodes);
  
  __set_bit(EV_KEY, input->evbit);
  
  for (i = 0; i < ARRAY_SIZE(lowrisc_kbd->keycodes); i++) {
    /*
     * Lowrisc lowrisc_kbdtroller happens to have scancodes match
     * our KEY_* definitions.
     */
    lowrisc_kbd->keycodes[i] = i;
    __set_bit(lowrisc_kbd->keycodes[i], input->keybit);
  }
  __clear_bit(KEY_RESERVED, input->keybit);
  
  error = input_register_polled_device(poll_dev);
  if (error) {
    dev_err(dev, "Unable to register input device: %d\n", error);
    return error;
  }
 
  return 0;
}

static struct platform_driver lowrisc_kbd_device_driver = {
	.probe    = lowrisc_kbd_probe,
	.driver   = {
		.name = "lowrisc-kbd",
	},
};
module_platform_driver(lowrisc_kbd_device_driver);

static struct resource lowrisc_keyboard[] = {
        [0] = {
          .start = keyb_base_addr,
          .end   = keyb_base_addr+0xFFF,
          .flags = IORESOURCE_MEM,
        },
        [1] = {
          .name  = "kbd_irq",
          .start = INTERRUPT_CAUSE_SOFTWARE,
          .end   = INTERRUPT_CAUSE_SOFTWARE,
          .flags = IORESOURCE_IRQ,
        },
};

static struct platform_device lowrisc_keyboard_device = {
                .name = "lowrisc-kbd",
                .id = -1,
                .num_resources = ARRAY_SIZE(lowrisc_keyboard),
                .resource = lowrisc_keyboard,
        };

static int __init lowrisc_kbd_device_init(void)
{
  int ret;
  ret = platform_device_register(&lowrisc_keyboard_device);
  printk("platform_device_register(&lowrisc_keyboard_device) returned %d\n", ret);
  return ret;
}

subsys_initcall_sync(lowrisc_kbd_device_init);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonathan Kimmitt <jonathan@kimmitt.uk>");
MODULE_DESCRIPTION("Keyboard driver for Lowrisc Keyboard Lowrisc_controller");

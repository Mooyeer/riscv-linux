/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation, version 2.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include <linux/clocksource.h>
#include <linux/delay.h>
#include <asm/sbi.h>

unsigned long riscv_timebase;

void __init time_init(void)
{
	timer_probe();
	lpj_fine = riscv_timebase / HZ;
}

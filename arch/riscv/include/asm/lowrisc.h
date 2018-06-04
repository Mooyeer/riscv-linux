#ifndef LOWRISC_HEADER_H
#define LOWRISC_HEADER_H

enum {  intc_base_addr = 0x0c000000,
        bram_base_addr = 0x40000000,
        sd_base_addr   = 0x40010000,        
        sd_bram_addr   = 0x40018000,
        eth_base_addr  = 0x40020000,
        keyb_base_addr = 0x40030000, // These have been relocated
        uart_base_addr = 0x40034000,
        vga_base_addr  = 0x40038000,
        ddr_base_addr  = 0x80000000
      };
      
#endif

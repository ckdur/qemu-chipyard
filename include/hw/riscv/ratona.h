/*
 * SiFive U series machine interface
 *
 * Copyright (c) 2017 SiFive, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_RATONA_H
#define HW_RATONA_H

#include "hw/boards.h"
#include "hw/cpu/cluster.h"
#include "hw/dma/sifive_pdma.h"
#include "hw/net/cadence_gem.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/sifive_cpu.h"
#include "hw/ssi/sifive_spi.h"
#include "hw/timer/sifive_pwm.h"

#define TYPE_RATONA_SOC "riscv.ratona.fpga.soc"
#define RATONA_SOC(obj) \
    OBJECT_CHECK(RatonaSoCState, (obj), TYPE_RATONA_SOC)

typedef struct RatonaSoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    RISCVHartArrayState cpus;
    DeviceState *plic;
    SiFiveSPIState spi0;

    uint32_t serial;
    char *cpu_type;
} RatonaSoCState;

#define TYPE_RATONA_MACHINE MACHINE_TYPE_NAME("ratona")
#define RATONA_MACHINE(obj) \
    OBJECT_CHECK(RatonaState, (obj), TYPE_RATONA_MACHINE)

typedef struct RatonaState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    RatonaSoCState soc;
    int fdt_size;
} RatonaState;

enum {
    RATONA_DEV_DEBUG,
    RATONA_DEV_MROM,
    RATONA_DEV_BOOTROM,
    RATONA_DEV_CLINT,
    RATONA_DEV_PLIC,
    RATONA_DEV_UART0,
    RATONA_DEV_QSPI0,
    RATONA_DEV_DRAM
};

enum {
    RATONA_UART0_IRQ = 4,
    RATONA_QSPI0_IRQ = 51
};

enum {
    RATONA_HFCLK_FREQ = 50000000,
    RATONA_RTCCLK_FREQ = 1000000
};

#define RATONA_PLIC_NUM_SOURCES 54
#define RATONA_PLIC_NUM_PRIORITIES 7
#define RATONA_PLIC_PRIORITY_BASE 0x00
#define RATONA_PLIC_PENDING_BASE 0x1000
#define RATONA_PLIC_ENABLE_BASE 0x2000
#define RATONA_PLIC_ENABLE_STRIDE 0x80
#define RATONA_PLIC_CONTEXT_BASE 0x200000
#define RATONA_PLIC_CONTEXT_STRIDE 0x1000

#define RATONA_CPU SIFIVE_U_CPU

#endif

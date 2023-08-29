/*
 * QEMU RISC-V Board Compatible with SiFive Freedom U SDK
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017 SiFive, Inc.
 * Copyright (c) 2019 Bin Meng <bmeng.cn@gmail.com>
 *
 * Provides a board compatible with the SiFive Freedom U SDK:
 *
 * 0) UART
 * 1) CLINT (Core Level Interruptor)
 * 2) PLIC (Platform Level Interrupt Controller)
 * 3) SPI0 connected to an SD card
 *
 * This board currently generates devicetree dynamically that indicates at least
 * two harts and up to five harts.
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

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "hw/cpu/cluster.h"
#include "hw/misc/unimp.h"
#include "hw/sd/sd.h"
#include "hw/ssi/ssi.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/ratona.h"
#include "hw/riscv/boot.h"
#include "hw/char/sifive_uart.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "chardev/char.h"
#include "net/eth.h"
#include "sysemu/device_tree.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"

#include <libfdt.h>

/* CLINT timebase frequency */
#define CLINT_TIMEBASE_FREQ 1000000

static const MemMapEntry ratona_memmap[] = {
    [RATONA_DEV_DEBUG] =    {        0x0,      0x100 },
    [RATONA_DEV_MROM] =     {     0x1000,     0xf000 },
    [RATONA_DEV_BOOTROM] =  {    0x10000,    0x10000 },
    [RATONA_DEV_CLINT] =    {  0x2000000,    0x10000 },
    [RATONA_DEV_PLIC] =     {  0xc000000,  0x4000000 },
    [RATONA_DEV_UART0] =    { 0x64000000,     0x1000 },
    [RATONA_DEV_QSPI0] =    { 0x64001000,     0x1000 },
    [RATONA_DEV_DRAM] =     { 0x80000000,        0x0 },
};

static void create_fdt(RatonaState *s, const MemMapEntry *memmap,
                       bool is_32_bit)
{
    MachineState *ms = MACHINE(s);
    uint64_t mem_size = ms->ram_size;
    void *fdt;
    int cpu;
    uint32_t *cells;
    char *nodename;
    uint32_t plic_phandle, phandle = 1;
    uint32_t hfclk_phandle, rtcclk_phandle;
    static const char * const clint_compat[2] = {
        "sifive,clint0", "riscv,clint0"
    };
    static const char * const plic_compat[2] = {
        "sifive,plic-1.0.0", "riscv,plic0"
    };

    fdt = ms->fdt = create_device_tree(&s->fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    qemu_fdt_setprop_string(fdt, "/", "model", "Ratona FPGA");
    qemu_fdt_setprop_string(fdt, "/", "compatible",
                            "riscv-ratona");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);

    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);

    hfclk_phandle = phandle++;
    nodename = g_strdup_printf("/hfclk");
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_cell(fdt, nodename, "phandle", hfclk_phandle);
    qemu_fdt_setprop_string(fdt, nodename, "clock-output-names", "hfclk");
    qemu_fdt_setprop_cell(fdt, nodename, "clock-frequency",
        RATONA_HFCLK_FREQ);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, nodename, "#clock-cells", 0x0);
    g_free(nodename);

    rtcclk_phandle = phandle++;
    nodename = g_strdup_printf("/rtcclk");
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_cell(fdt, nodename, "phandle", rtcclk_phandle);
    qemu_fdt_setprop_string(fdt, nodename, "clock-output-names", "rtcclk");
    qemu_fdt_setprop_cell(fdt, nodename, "clock-frequency",
        RATONA_RTCCLK_FREQ);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, nodename, "#clock-cells", 0x0);
    g_free(nodename);

    nodename = g_strdup_printf("/memory@%lx",
        (long)memmap[RATONA_DEV_DRAM].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
        memmap[RATONA_DEV_DRAM].base >> 32, memmap[RATONA_DEV_DRAM].base,
        mem_size >> 32, mem_size);
    qemu_fdt_setprop_string(fdt, nodename, "device_type", "memory");
    g_free(nodename);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
        CLINT_TIMEBASE_FREQ);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);

    for (cpu = ms->smp.cpus - 1; cpu >= 0; cpu--) {
        int cpu_phandle = phandle++;
        nodename = g_strdup_printf("/cpus/cpu@%d", cpu);
        char *intc = g_strdup_printf("/cpus/cpu@%d/interrupt-controller", cpu);
        char *isa;
        qemu_fdt_add_subnode(fdt, nodename);
        /* cpu 0 is the management hart that does not have mmu */
        if (is_32_bit) {
            qemu_fdt_setprop_string(fdt, nodename, "mmu-type", "riscv,sv32");
        } else {
            qemu_fdt_setprop_string(fdt, nodename, "mmu-type", "riscv,sv48");
        }
        isa = riscv_isa_string(&s->soc.cpus.harts[cpu]);
        qemu_fdt_setprop_string(fdt, nodename, "riscv,isa", isa);
        qemu_fdt_setprop_string(fdt, nodename, "compatible", "riscv");
        qemu_fdt_setprop_string(fdt, nodename, "status", "okay");
        qemu_fdt_setprop_cell(fdt, nodename, "reg", cpu);
        qemu_fdt_setprop_string(fdt, nodename, "device_type", "cpu");
        qemu_fdt_add_subnode(fdt, intc);
        qemu_fdt_setprop_cell(fdt, intc, "phandle", cpu_phandle);
        qemu_fdt_setprop_string(fdt, intc, "compatible", "riscv,cpu-intc");
        qemu_fdt_setprop(fdt, intc, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, intc, "#interrupt-cells", 1);
        g_free(isa);
        g_free(intc);
        g_free(nodename);
    }

    cells =  g_new0(uint32_t, ms->smp.cpus * 4);
    for (cpu = 0; cpu < ms->smp.cpus; cpu++) {
        nodename =
            g_strdup_printf("/cpus/cpu@%d/interrupt-controller", cpu);
        uint32_t intc_phandle = qemu_fdt_get_phandle(fdt, nodename);
        cells[cpu * 4 + 0] = cpu_to_be32(intc_phandle);
        cells[cpu * 4 + 1] = cpu_to_be32(IRQ_M_SOFT);
        cells[cpu * 4 + 2] = cpu_to_be32(intc_phandle);
        cells[cpu * 4 + 3] = cpu_to_be32(IRQ_M_TIMER);
        g_free(nodename);
    }
    nodename = g_strdup_printf("/soc/clint@%lx",
        (long)memmap[RATONA_DEV_CLINT].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string_array(fdt, nodename, "compatible",
        (char **)&clint_compat, ARRAY_SIZE(clint_compat));
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
        0x0, memmap[RATONA_DEV_CLINT].base,
        0x0, memmap[RATONA_DEV_CLINT].size);
    qemu_fdt_setprop(fdt, nodename, "interrupts-extended",
        cells, ms->smp.cpus * sizeof(uint32_t) * 4);
    g_free(cells);
    g_free(nodename);

    plic_phandle = phandle++;
    cells =  g_new0(uint32_t, ms->smp.cpus * 4);
    for (cpu = 0; cpu < ms->smp.cpus; cpu++) {
        nodename =
            g_strdup_printf("/cpus/cpu@%d/interrupt-controller", cpu);
        uint32_t intc_phandle = qemu_fdt_get_phandle(fdt, nodename);
        cells[cpu * 4 + 0] = cpu_to_be32(intc_phandle);
        cells[cpu * 4 + 1] = cpu_to_be32(IRQ_M_EXT);
        cells[cpu * 4 + 2] = cpu_to_be32(intc_phandle);
        cells[cpu * 4 + 3] = cpu_to_be32(IRQ_S_EXT);
        g_free(nodename);
    }
    nodename = g_strdup_printf("/soc/interrupt-controller@%lx",
        (long)memmap[RATONA_DEV_PLIC].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_cell(fdt, nodename, "#interrupt-cells", 1);
    qemu_fdt_setprop_string_array(fdt, nodename, "compatible",
        (char **)&plic_compat, ARRAY_SIZE(plic_compat));
    qemu_fdt_setprop(fdt, nodename, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop(fdt, nodename, "interrupts-extended",
        cells, (ms->smp.cpus * 4) * sizeof(uint32_t));
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
        0x0, memmap[RATONA_DEV_PLIC].base,
        0x0, memmap[RATONA_DEV_PLIC].size);
    qemu_fdt_setprop_cell(fdt, nodename, "riscv,ndev",
                          RATONA_PLIC_NUM_SOURCES - 1);
    qemu_fdt_setprop_cell(fdt, nodename, "phandle", plic_phandle);
    plic_phandle = qemu_fdt_get_phandle(fdt, nodename);
    g_free(cells);
    g_free(nodename);

    nodename = g_strdup_printf("/soc/spi@%lx",
        (long)memmap[RATONA_DEV_QSPI0].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_cell(fdt, nodename, "#size-cells", 0);
    qemu_fdt_setprop_cell(fdt, nodename, "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, nodename, "interrupts", RATONA_QSPI0_IRQ);
    qemu_fdt_setprop_cell(fdt, nodename, "interrupt-parent", plic_phandle);
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
        0x0, memmap[RATONA_DEV_QSPI0].base,
        0x0, memmap[RATONA_DEV_QSPI0].size);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "sifive,spi0");
    qemu_fdt_setprop_cell(fdt, nodename, "clocks", hfclk_phandle); // This is important
    g_free(nodename);

    nodename = g_strdup_printf("/soc/spi@%lx/mmc@0",
        (long)memmap[RATONA_DEV_QSPI0].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop(fdt, nodename, "disable-wp", NULL, 0);
    qemu_fdt_setprop_cells(fdt, nodename, "voltage-ranges", 3300, 3300);
    qemu_fdt_setprop_cell(fdt, nodename, "spi-max-frequency", 20000000);
    qemu_fdt_setprop_cell(fdt, nodename, "reg", 0);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "mmc-spi-slot");
    g_free(nodename);

    nodename = g_strdup_printf("/soc/rom@%lx",
        (long)memmap[RATONA_DEV_BOOTROM].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "sifive,rom0");
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
        0x0, memmap[RATONA_DEV_BOOTROM].base,
        0x0, memmap[RATONA_DEV_BOOTROM].size);
    qemu_fdt_setprop_string(fdt, nodename, "reg-names", "mem");
    g_free(nodename);

    nodename = g_strdup_printf("/soc/serial@%lx",
        (long)memmap[RATONA_DEV_UART0].base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "sifive,uart0");
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
        0x0, memmap[RATONA_DEV_UART0].base,
        0x0, memmap[RATONA_DEV_UART0].size);
    qemu_fdt_setprop_cell(fdt, nodename, "interrupt-parent", plic_phandle);
    qemu_fdt_setprop_cell(fdt, nodename, "interrupts", RATONA_UART0_IRQ);
    qemu_fdt_setprop_cell(fdt, nodename, "clocks", hfclk_phandle); // This is important

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", nodename);
    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0", nodename);

    g_free(nodename);
}

static void ratona_machine_init(MachineState *machine)
{
    const MemMapEntry *memmap = ratona_memmap;
    RatonaState *s = RATONA_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *bootrom = g_new(MemoryRegion, 1);
    target_ulong start_addr = memmap[RATONA_DEV_DRAM].base;
    target_ulong firmware_end_addr, kernel_start_addr;
    const char *firmware_name;
    uint32_t start_addr_hi32 = 0x00000000;
    int i;
    uint32_t fdt_load_addr;
    uint64_t kernel_entry;
    DriveInfo *dinfo;
    BlockBackend *blk;
    DeviceState *sd_dev, *card_dev;
    qemu_irq sd_cs;

    /* Initialize SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_RATONA_SOC);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* register RAM */
    memory_region_add_subregion(system_memory, memmap[RATONA_DEV_DRAM].base,
                                machine->ram);

    /* register ROM */
    memory_region_init_ram(bootrom, NULL, "sifive.rom",
                           memmap[RATONA_DEV_BOOTROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[RATONA_DEV_BOOTROM].base,
                                bootrom);

    /* load/create device tree */
    if (machine->dtb) {
        machine->fdt = load_device_tree(machine->dtb, &s->fdt_size);
        if (!machine->fdt) {
            error_report("load_device_tree() failed");
            exit(1);
        }
    } else {
        create_fdt(s, memmap, riscv_is_32bit(&s->soc.cpus));
    }

    start_addr = memmap[RATONA_DEV_DRAM].base;
    firmware_name = riscv_default_firmware_name(&s->soc.cpus);
    firmware_end_addr = riscv_find_and_load_firmware(machine, firmware_name,
                                                     start_addr, NULL);

    if (machine->kernel_filename) {
        kernel_start_addr = riscv_calc_kernel_start_addr(&s->soc.cpus,
                                                         firmware_end_addr);

        kernel_entry = riscv_load_kernel(machine, &s->soc.cpus,
                                         kernel_start_addr, true, NULL);
        start_addr = kernel_entry;
    } else {
       /*
        * If dynamic firmware is used, it doesn't know where is the next mode
        * if kernel argument is not set.
        */
        kernel_entry = 0;
    }

    fdt_load_addr = riscv_compute_fdt_addr(memmap[RATONA_DEV_DRAM].base,
                                           memmap[RATONA_DEV_DRAM].size,
                                           machine);
    riscv_load_fdt(fdt_load_addr, machine->fdt);

    if (!riscv_is_32bit(&s->soc.cpus)) {
        start_addr_hi32 = (uint64_t)start_addr >> 32;
    }

    /* reset vector */
    uint32_t reset_vec[12] = {
        0x00000000,                       /* MSEL pin state (TODO) */
        0x00000297,                    /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02c28613,                    /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                    /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                    /*     jr     t0 */
        start_addr,                    /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,                 /* fdt_laddr: .dword */
        0x00000000,
        0x00000000,
                                       /* fw_dyn: */
    };
    if (riscv_is_32bit(&s->soc.cpus)) {
        reset_vec[4] = 0x0202a583;     /*     lw     a1, 32(t0) */
        reset_vec[5] = 0x0182a283;     /*     lw     t0, 24(t0) */
    } else {
        reset_vec[4] = 0x0202b583;     /*     ld     a1, 32(t0) */
        reset_vec[5] = 0x0182b283;     /*     ld     t0, 24(t0) */
    }


    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }
    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          memmap[RATONA_DEV_MROM].base, &address_space_memory);

    riscv_rom_copy_firmware_info(machine, memmap[RATONA_DEV_MROM].base,
                                 memmap[RATONA_DEV_MROM].size,
                                 sizeof(reset_vec), kernel_entry);

    /* Connect an SD card to SPI0 */
    sd_dev = ssi_create_peripheral(s->soc.spi0.spi, "ssi-sd");

    sd_cs = qdev_get_gpio_in_named(sd_dev, SSI_GPIO_CS, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->soc.spi0), 1, sd_cs);

    dinfo = drive_get(IF_SD, 0, 0);
    blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
    card_dev = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card_dev, "drive", blk, &error_fatal);
    qdev_prop_set_bit(card_dev, "spi", true);
    qdev_realize_and_unref(card_dev,
                           qdev_get_child_bus(sd_dev, "sd-bus"),
                           &error_fatal);
}

static void ratona_machine_instance_init(Object *obj)
{
}

static void ratona_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Board RATONA (FPGA compatible)";
    mc->init = ratona_machine_init;
    mc->min_cpus = 4;
    mc->min_cpus = 1;
    mc->default_cpu_type = RATONA_CPU;
    mc->default_cpus = mc->min_cpus;
    mc->default_ram_id = "riscv.ratona.ram";
}

static const TypeInfo ratona_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("ratona"),
    .parent     = TYPE_MACHINE,
    .class_init = ratona_machine_class_init,
    .instance_init = ratona_machine_instance_init,
    .instance_size = sizeof(RatonaState),
};

static void ratona_machine_init_register_types(void)
{
    type_register_static(&ratona_machine_typeinfo);
}

type_init(ratona_machine_init_register_types)

static void ratona_soc_instance_init(Object *obj)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    RatonaSoCState *s = RATONA_SOC(obj);

    object_initialize_child(obj, "cpus", &s->cpus, TYPE_RISCV_HART_ARRAY);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", ms->smp.cpus,
                            &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec", 0x1004,
                            &error_abort);
                            
    object_initialize_child(obj, "spi0", &s->spi0, TYPE_SIFIVE_SPI);
}

static void ratona_soc_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    RatonaSoCState *s = RATONA_SOC(dev);
    const MemMapEntry *memmap = ratona_memmap;
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    char *plic_hart_config;
    
    object_property_set_str(OBJECT(&s->cpus), "cpu-type", ms->cpu_type,
                            &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_fatal);

    /* boot rom */
    memory_region_init_rom(mask_rom, OBJECT(dev), "riscv.sifive.u.mrom",
                           memmap[RATONA_DEV_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[RATONA_DEV_MROM].base,
                                mask_rom);

    /* create PLIC hart topology configuration string */
    plic_hart_config = riscv_plic_hart_config_string(ms->smp.cpus);

    /* MMIO */
    s->plic = sifive_plic_create(memmap[RATONA_DEV_PLIC].base,
        plic_hart_config, ms->smp.cpus, 0,
        RATONA_PLIC_NUM_SOURCES,
        RATONA_PLIC_NUM_PRIORITIES,
        RATONA_PLIC_PRIORITY_BASE,
        RATONA_PLIC_PENDING_BASE,
        RATONA_PLIC_ENABLE_BASE,
        RATONA_PLIC_ENABLE_STRIDE,
        RATONA_PLIC_CONTEXT_BASE,
        RATONA_PLIC_CONTEXT_STRIDE,
        memmap[RATONA_DEV_PLIC].size);
    g_free(plic_hart_config);
    sifive_uart_create(system_memory, memmap[RATONA_DEV_UART0].base,
        serial_hd(0), qdev_get_gpio_in(DEVICE(s->plic), RATONA_UART0_IRQ));
    riscv_aclint_swi_create(memmap[RATONA_DEV_CLINT].base, 0,
        ms->smp.cpus, false);
    riscv_aclint_mtimer_create(memmap[RATONA_DEV_CLINT].base +
            RISCV_ACLINT_SWI_SIZE,
        RISCV_ACLINT_DEFAULT_MTIMER_SIZE, 0, ms->smp.cpus,
        RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
        CLINT_TIMEBASE_FREQ, false);
    
    sysbus_realize(SYS_BUS_DEVICE(&s->spi0), errp);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi0), 0,
                    memmap[RATONA_DEV_QSPI0].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi0), 0,
                       qdev_get_gpio_in(DEVICE(s->plic), RATONA_QSPI0_IRQ));
}

static void ratona_soc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = ratona_soc_realize;
    /* Reason: Uses serial_hds in realize function, thus can't be used twice */
    dc->user_creatable = false;
}

static const TypeInfo ratona_soc_type_info = {
    .name = TYPE_RATONA_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(RatonaSoCState),
    .instance_init = ratona_soc_instance_init,
    .class_init = ratona_soc_class_init,
};

static void ratona_soc_register_types(void)
{
    type_register_static(&ratona_soc_type_info);
}

type_init(ratona_soc_register_types)

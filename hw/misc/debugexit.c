/*
 * debug exit port emulation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) any later version.
 */

#include "hw/hw.h"
#include "hw/isa/isa.h"

#define TYPE_ISA_DEBUG_EXIT_DEVICE "isa-debug-exit"
#define ISA_DEBUG_EXIT_DEVICE(obj) \
     OBJECT_CHECK(ISADebugExitState, (obj), TYPE_ISA_DEBUG_EXIT_DEVICE)

typedef struct ISADebugExitState {
    ISADevice parent_obj;

    uint32_t iobase;
    uint32_t iosize;
    MemoryRegion io;
} ISADebugExitState;

static void debug_exit_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned width)
{
    exit((val << 1) | 1);
}

static const MemoryRegionOps debug_exit_ops = {
    .write = debug_exit_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int debug_exit_initfn(ISADevice *dev)
{
    ISADebugExitState *isa = ISA_DEBUG_EXIT_DEVICE(dev);

    memory_region_init_io(&isa->io, &debug_exit_ops, isa,
                          TYPE_ISA_DEBUG_EXIT_DEVICE, isa->iosize);
    memory_region_add_subregion(isa_address_space_io(dev),
                                isa->iobase, &isa->io);
    return 0;
}

static Property debug_exit_properties[] = {
    DEFINE_PROP_HEX32("iobase", ISADebugExitState, iobase, 0x501),
    DEFINE_PROP_HEX32("iosize", ISADebugExitState, iosize, 0x02),
    DEFINE_PROP_END_OF_LIST(),
};

static void debug_exit_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISADeviceClass *ic = ISA_DEVICE_CLASS(klass);
    ic->init = debug_exit_initfn;
    dc->props = debug_exit_properties;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo debug_exit_info = {
    .name          = TYPE_ISA_DEBUG_EXIT_DEVICE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ISADebugExitState),
    .class_init    = debug_exit_class_initfn,
};

static void debug_exit_register_types(void)
{
    type_register_static(&debug_exit_info);
}

type_init(debug_exit_register_types)

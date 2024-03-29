/*
 * QEMU Bochs-style debug console ("port E9") emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2008 Citrix Systems, Inc.
 * Copyright (c) Intel Corporation; author: H. Peter Anvin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "hw/hw.h"
#include "sysemu/char.h"
#include "hw/isa/isa.h"
#include "hw/i386/pc.h"

#define TYPE_ISA_DEBUGCON_DEVICE "isa-debugcon"
#define ISA_DEBUGCON_DEVICE(obj) \
     OBJECT_CHECK(ISADebugconState, (obj), TYPE_ISA_DEBUGCON_DEVICE)

//#define DEBUG_DEBUGCON

typedef struct DebugconState {
    MemoryRegion io;
    CharDriverState *chr;
    uint32_t readback;
} DebugconState;

typedef struct ISADebugconState {
    ISADevice parent_obj;

    uint32_t iobase;
    DebugconState state;
} ISADebugconState;

static void debugcon_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned width)
{
    DebugconState *s = opaque;
    unsigned char ch = val;

#ifdef DEBUG_DEBUGCON
    printf("debugcon: write addr=0x%04x val=0x%02x\n", addr, val);
#endif

    qemu_chr_fe_write(s->chr, &ch, 1);
}


static uint64_t debugcon_ioport_read(void *opaque, hwaddr addr, unsigned width)
{
    DebugconState *s = opaque;

#ifdef DEBUG_DEBUGCON
    printf("debugcon: read addr=0x%04x\n", addr);
#endif

    return s->readback;
}

static const MemoryRegionOps debugcon_ops = {
    .read = debugcon_ioport_read,
    .write = debugcon_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void debugcon_init_core(DebugconState *s)
{
    if (!s->chr) {
        fprintf(stderr, "Can't create debugcon device, empty char device\n");
        exit(1);
    }

    qemu_chr_add_handlers(s->chr, NULL, NULL, NULL, s);
}

static int debugcon_isa_initfn(ISADevice *dev)
{
    ISADebugconState *isa = ISA_DEBUGCON_DEVICE(dev);
    DebugconState *s = &isa->state;

    debugcon_init_core(s);
    memory_region_init_io(&s->io, &debugcon_ops, s,
                          TYPE_ISA_DEBUGCON_DEVICE, 1);
    memory_region_add_subregion(isa_address_space_io(dev),
                                isa->iobase, &s->io);
    return 0;
}

static Property debugcon_isa_properties[] = {
    DEFINE_PROP_HEX32("iobase", ISADebugconState, iobase, 0xe9),
    DEFINE_PROP_CHR("chardev",  ISADebugconState, state.chr),
    DEFINE_PROP_HEX32("readback", ISADebugconState, state.readback, 0xe9),
    DEFINE_PROP_END_OF_LIST(),
};

static void debugcon_isa_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISADeviceClass *ic = ISA_DEVICE_CLASS(klass);
    ic->init = debugcon_isa_initfn;
    dc->props = debugcon_isa_properties;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo debugcon_isa_info = {
    .name          = TYPE_ISA_DEBUGCON_DEVICE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ISADebugconState),
    .class_init    = debugcon_isa_class_initfn,
};

static void debugcon_register_types(void)
{
    type_register_static(&debugcon_isa_info);
}

type_init(debugcon_register_types)

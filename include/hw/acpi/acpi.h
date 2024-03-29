#ifndef QEMU_HW_ACPI_H
#define QEMU_HW_ACPI_H
/*
 *  Copyright (c) 2009 Isaku Yamahata <yamahata at valinux co jp>
 *                     VA Linux Systems Japan K.K.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "qapi/error.h"
#include "qemu/typedefs.h"
#include "qemu/notify.h"
#include "qemu/option.h"
#include "exec/memory.h"

/* from linux include/acpi/actype.h */
/* Default ACPI register widths */

#define ACPI_GPE_REGISTER_WIDTH         8
#define ACPI_PM1_REGISTER_WIDTH         16
#define ACPI_PM2_REGISTER_WIDTH         8
#define ACPI_PM_TIMER_WIDTH             32

/* PM Timer ticks per second (HZ) */
#define PM_TIMER_FREQUENCY  3579545


/* ACPI fixed hardware registers */

/* from linux/drivers/acpi/acpica/aclocal.h */
/* Masks used to access the bit_registers */

/* PM1x_STS */
#define ACPI_BITMASK_TIMER_STATUS               0x0001
#define ACPI_BITMASK_BUS_MASTER_STATUS          0x0010
#define ACPI_BITMASK_GLOBAL_LOCK_STATUS         0x0020
#define ACPI_BITMASK_POWER_BUTTON_STATUS        0x0100
#define ACPI_BITMASK_SLEEP_BUTTON_STATUS        0x0200
#define ACPI_BITMASK_RT_CLOCK_STATUS            0x0400
#define ACPI_BITMASK_PCIEXP_WAKE_STATUS         0x4000	/* ACPI 3.0 */
#define ACPI_BITMASK_WAKE_STATUS                0x8000

#define ACPI_BITMASK_ALL_FIXED_STATUS           (\
	ACPI_BITMASK_TIMER_STATUS          | \
	ACPI_BITMASK_BUS_MASTER_STATUS     | \
	ACPI_BITMASK_GLOBAL_LOCK_STATUS    | \
	ACPI_BITMASK_POWER_BUTTON_STATUS   | \
	ACPI_BITMASK_SLEEP_BUTTON_STATUS   | \
	ACPI_BITMASK_RT_CLOCK_STATUS       | \
	ACPI_BITMASK_WAKE_STATUS)

/* PM1x_EN */
#define ACPI_BITMASK_TIMER_ENABLE               0x0001
#define ACPI_BITMASK_GLOBAL_LOCK_ENABLE         0x0020
#define ACPI_BITMASK_POWER_BUTTON_ENABLE        0x0100
#define ACPI_BITMASK_SLEEP_BUTTON_ENABLE        0x0200
#define ACPI_BITMASK_RT_CLOCK_ENABLE            0x0400
#define ACPI_BITMASK_PCIEXP_WAKE_DISABLE        0x4000	/* ACPI 3.0 */

/* PM1x_CNT */
#define ACPI_BITMASK_SCI_ENABLE                 0x0001
#define ACPI_BITMASK_BUS_MASTER_RLD             0x0002
#define ACPI_BITMASK_GLOBAL_LOCK_RELEASE        0x0004
#define ACPI_BITMASK_SLEEP_TYPE                 0x1C00
#define ACPI_BITMASK_SLEEP_ENABLE               0x2000

/* PM2_CNT */
#define ACPI_BITMASK_ARB_DISABLE                0x0001

/* structs */
typedef struct ACPIPMTimer ACPIPMTimer;
typedef struct ACPIPM1EVT ACPIPM1EVT;
typedef struct ACPIPM1CNT ACPIPM1CNT;
typedef struct ACPIGPE ACPIGPE;
typedef struct ACPIREGS ACPIREGS;

typedef void (*acpi_update_sci_fn)(ACPIREGS *ar);

struct ACPIPMTimer {
    QEMUTimer *timer;
    MemoryRegion io;
    int64_t overflow_time;

    acpi_update_sci_fn update_sci;
};

struct ACPIPM1EVT {
    MemoryRegion io;
    uint16_t sts;
    uint16_t en;
    acpi_update_sci_fn update_sci;
};

struct ACPIPM1CNT {
    MemoryRegion io;
    uint16_t cnt;
    uint8_t s4_val;
};

struct ACPIGPE {
    uint8_t len;

    uint8_t *sts;
    uint8_t *en;
};

struct ACPIREGS {
    ACPIPMTimer     tmr;
    ACPIGPE         gpe;
    struct {
        ACPIPM1EVT  evt;
        ACPIPM1CNT  cnt;
    } pm1;
    Notifier wakeup;
};

/* PM_TMR */
void acpi_pm_tmr_update(ACPIREGS *ar, bool enable);
void acpi_pm_tmr_calc_overflow_time(ACPIREGS *ar);
void acpi_pm_tmr_init(ACPIREGS *ar, acpi_update_sci_fn update_sci,
                      MemoryRegion *parent);
void acpi_pm_tmr_reset(ACPIREGS *ar);

#include "qemu/timer.h"
static inline int64_t acpi_pm_tmr_get_clock(void)
{
    return muldiv64(qemu_get_clock_ns(vm_clock), PM_TIMER_FREQUENCY,
                    get_ticks_per_sec());
}

/* PM1a_EVT: piix and ich9 don't implement PM1b. */
uint16_t acpi_pm1_evt_get_sts(ACPIREGS *ar);
void acpi_pm1_evt_power_down(ACPIREGS *ar);
void acpi_pm1_evt_reset(ACPIREGS *ar);
void acpi_pm1_evt_init(ACPIREGS *ar, acpi_update_sci_fn update_sci,
                       MemoryRegion *parent);

/* PM1a_CNT: piix and ich9 don't implement PM1b CNT. */
void acpi_pm1_cnt_init(ACPIREGS *ar, MemoryRegion *parent, uint8_t s4_val);
void acpi_pm1_cnt_update(ACPIREGS *ar,
                         bool sci_enable, bool sci_disable);
void acpi_pm1_cnt_reset(ACPIREGS *ar);

/* GPE0 */
void acpi_gpe_init(ACPIREGS *ar, uint8_t len);
void acpi_gpe_reset(ACPIREGS *ar);

void acpi_gpe_ioport_writeb(ACPIREGS *ar, uint32_t addr, uint32_t val);
uint32_t acpi_gpe_ioport_readb(ACPIREGS *ar, uint32_t addr);

/* acpi.c */
extern int acpi_enabled;
extern char unsigned *acpi_tables;
extern size_t acpi_tables_len;

uint8_t *acpi_table_first(void);
uint8_t *acpi_table_next(uint8_t *current);
unsigned acpi_table_len(void *current);
void acpi_table_add(const QemuOpts *opts, Error **errp);
void acpi_table_add_builtin(const QemuOpts *opts, Error **errp);

typedef struct AcpiSlicOem AcpiSlicOem;
struct AcpiSlicOem {
  char *id;
  char *table_id;
};
int acpi_get_slic_oem(AcpiSlicOem *oem);

#endif /* !QEMU_HW_ACPI_H */

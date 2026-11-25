/*
 * PC sampling and guest memory reads.
 *
 * Samples come from a timer in 86Box's own timer system, so no CPU, timer or
 * dynarec timing code changes. The timer fires between dynarec blocks, so a
 * sample is the PC at the next block boundary; with the interpreter it is the
 * exact instruction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "cpu.h"
#include "hl.h"

#define SLOTS (1u << 16) /* distinct sampled PCs; further ones are counted as dropped */

typedef struct slot_t {
    uint32_t pc;
    uint32_t count;
} slot_t;

static pc_timer_t timer;
static int        active;
static int        added;
static uint32_t   interval_us;
static uint64_t   total;
static uint64_t   dropped;
static slot_t    *table;

static void
sample(UNUSED(void *priv))
{
    const uint32_t pc = cpu_state.seg_cs.base + cpu_state.pc;
    uint32_t       h  = (pc * 2654435761u) >> 16;

    total++;
    for (unsigned n = 0; n < SLOTS; n++, h++) {
        slot_t *s = &table[h & (SLOTS - 1)];

        if (s->count && s->pc != pc)
            continue;
        s->pc = pc;
        s->count++;
        break;
    }
    timer_advance_u64(&timer, (uint64_t) interval_us * TIMER_USEC);
}

static void
arm(void)
{
    timer_add(&timer, sample, NULL, 0);
    timer_on_auto(&timer, (double) interval_us);
}

/* The timer list is rebuilt on every hard reset. */
void
hl_prof_reset(void)
{
    if (active)
        arm();
}

void
hl_prof_start(uint32_t interval)
{
    free(table);
    table       = calloc(SLOTS, sizeof(slot_t));
    interval_us = interval ? interval : 20;
    total = dropped = 0;
    active          = 1;
    if (added)
        timer_disable(&timer);
    added = 1;
    arm();
}

int
hl_prof_active(void)
{
    return active;
}

/* Stops sampling; writes the histogram JSON to path. Returns 0, or -1 on a write error. */
int
hl_prof_stop(const char *path, uint64_t *samples, uint32_t *distinct, uint32_t *interval)
{
    FILE *fp;

    timer_disable(&timer);
    active    = 0;
    *samples  = total;
    *interval = interval_us;
    *distinct = 0;
    fp        = fopen(path, "w");
    if (!fp)
        return -1;
    fprintf(fp, "{\"interval_us\": %u, \"samples\": %llu, \"pcs\": {", interval_us,
            (unsigned long long) total);
    for (unsigned i = 0; i < SLOTS; i++) {
        if (table[i].count)
            fprintf(fp, "%s\"%x\": %u", (*distinct)++ ? ", " : "", table[i].pc, table[i].count);
    }
    fprintf(fp, "}}\n");
    free(table);
    table = NULL;
    return fclose(fp);
}

/* Linear to physical through the current page tables; 0 on a fault. */
static int
translate(uint32_t lin, uint32_t *phys)
{
    uint32_t pde, pte;

    if (!(cr0 & 0x80000000u)) {
        *phys = lin;
        return 1;
    }
    pde = mem_readl_phys((cr3 & ~0xfffu) + ((lin >> 22) << 2));
    if (!(pde & 1))
        return 0;
    if ((cr4 & 0x10) && (pde & 0x80)) {
        *phys = (pde & 0xffc00000u) | (lin & 0x3fffff);
        return 1;
    }
    pte = mem_readl_phys((pde & ~0xfffu) + (((lin >> 12) & 0x3ff) << 2));
    if (!(pte & 1))
        return 0;
    *phys = (pte & ~0xfffu) | (lin & 0xfff);
    return 1;
}

/* Reads len bytes at a linear address (through paging when on) into a malloc'd buffer; NULL if a page is unmapped. */
uint8_t *
hl_mem_read(uint32_t lin, uint32_t len)
{
    uint8_t *buf = malloc(len ? len : 1);

    for (uint32_t i = 0; i < len; i++) {
        uint32_t phys;

        if (!translate(lin + i, &phys)) {
            free(buf);
            return NULL;
        }
        buf[i] = mem_readb_phys(phys);
    }
    return buf;
}

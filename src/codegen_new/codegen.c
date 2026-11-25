#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "x86_ops.h"
#include "codegen.h"
#include "x86.h"
#include "x86seg_common.h"
#include "x86seg.h"

#include "386_common.h"

#include "codegen_accumulate.h"
#include "codegen_allocator.h"
#include "codegen_backend.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_helpers.h"

#define MAX_INSTRUCTION_COUNT 50
static struct {
    uint32_t pc;
    int      op_ssegs;
    x86seg  *op_ea_seg;
    uint32_t op_32;
    int      first_uop;
    int      TOP;
} codegen_instructions[MAX_INSTRUCTION_COUNT];

int
codegen_get_instruction_uop(codeblock_t *block, uint32_t pc, int *first_instruction, int *TOP)
{
    for (uint8_t c = 0; c <= block->ins; c++) {
        if (codegen_instructions[c].pc == pc) {
            *first_instruction = c;
            *TOP               = codegen_instructions[c].TOP;
            return codegen_instructions[c].first_uop;
        }
    }

    *first_instruction = block->ins;
    return -1;
}

void
codegen_set_loop_start(ir_data_t *ir, int first_instruction)
{
    uop_MOV_IMM(ir, IREG_op32, codegen_instructions[first_instruction].op_32);
    uop_MOV_PTR(ir, IREG_ea_seg, (void *) codegen_instructions[first_instruction].op_ea_seg);
    uop_MOV_IMM(ir, IREG_ssegs, codegen_instructions[first_instruction].op_ssegs);
}

int has_ea;
extern uint32_t codegen_endpc;

codeblock_t *codeblock;
uint16_t    *codeblock_hash;

void (*codegen_timing_start)(void);
void (*codegen_timing_prefix)(uint8_t prefix, uint32_t fetchdat);
void (*codegen_timing_opcode)(uint8_t opcode, uint32_t fetchdat, int op_32, uint32_t op_pc);
void (*codegen_timing_block_start)(void);
void (*codegen_timing_block_end)(void);
int (*codegen_timing_jump_cycles)(void);

/*
 * Branch prediction.
 *
 * P6 (Agner Fog, "The microarchitecture of Intel, AMD and VIA CPUs", PPro/P2/P3
 * branch prediction): 512-entry BTB, 16 ways * 32 sets, set = address bits 4-8,
 * entry allocated on first execution. Each entry is a two-level adaptive
 * predictor: a 4-bit local history picks one of 16 two-bit saturating
 * counters, so any repeating pattern of period 5 or less is predicted
 * perfectly. A misprediction "usually costs between 10 and 20 clock cycles";
 * 15 is used. A branch missing from the BTB is predicted by the static rule
 * (backward taken, forward not taken).
 *
 * P5: 256 entries of one two-bit counter, direct mapped, 4 cycle mispredict
 * (Agner Fog: 4 clocks in the U-pipe, 5 in the V-pipe).
 *
 * codegen_bp_penalty of zero turns prediction off.
 */
#define P6_BTB_SETS 32
#define P6_BTB_WAYS 16
#define P5_BTB_ENTRIES 256
#define P6_MISPREDICT 15
#define P5_MISPREDICT 4

int codegen_bp_penalty;
int codegen_bp_taken_cost; /* 486: a taken jcc costs 3 cycles, not 1 (i486 manual) */
static int bp_two_level;

static struct {
    uint32_t pc;
    uint32_t used; /*LRU stamp*/
    uint8_t  valid;
    uint8_t  history;
    uint8_t  counter[16];
} p6_btb[P6_BTB_SETS][P6_BTB_WAYS];
static uint32_t p6_btb_clock;

static struct {
    uint32_t pc;
    uint8_t  valid;
    uint8_t  counter;
} p5_btb[P5_BTB_ENTRIES];

static int
bp_predict_p6(uint32_t pc, int taken, int backward)
{
    const int set = (pc >> 4) & (P6_BTB_SETS - 1);
    int       way, victim = 0;

    for (way = 0; way < P6_BTB_WAYS; way++) {
        if (p6_btb[set][way].valid && p6_btb[set][way].pc == pc) {
            uint8_t *c        = &p6_btb[set][way].counter[p6_btb[set][way].history];
            const int predicted = *c >= 2;

            if (taken && *c < 3)
                (*c)++;
            else if (!taken && *c > 0)
                (*c)--;
            p6_btb[set][way].history = ((p6_btb[set][way].history << 1) | taken) & 15;
            p6_btb[set][way].used    = ++p6_btb_clock;
            return predicted;
        }
    }
    for (way = 0; way < P6_BTB_WAYS; way++) {
        if (!p6_btb[set][way].valid) {
            victim = way;
            break;
        }
        if (p6_btb[set][way].used < p6_btb[set][victim].used)
            victim = way;
    }
    memset(&p6_btb[set][victim], 0, sizeof(p6_btb[set][victim]));
    p6_btb[set][victim].valid   = 1;
    p6_btb[set][victim].pc      = pc;
    p6_btb[set][victim].used    = ++p6_btb_clock;
    p6_btb[set][victim].history = taken;
    memset(p6_btb[set][victim].counter, 1, sizeof(p6_btb[set][victim].counter));
    return backward;
}

static int
bp_predict_p5(uint32_t pc, int taken, int backward)
{
    const int slot = (pc ^ (pc >> 8)) & (P5_BTB_ENTRIES - 1);
    int       predicted;

    if (p5_btb[slot].valid && p5_btb[slot].pc == pc) {
        predicted = p5_btb[slot].counter >= 2;
        if (taken && p5_btb[slot].counter < 3)
            p5_btb[slot].counter++;
        else if (!taken && p5_btb[slot].counter > 0)
            p5_btb[slot].counter--;
    } else {
        predicted              = backward;
        p5_btb[slot].valid     = 1;
        p5_btb[slot].pc        = pc;
        p5_btb[slot].counter   = taken ? 2 : 1;
    }
    return predicted;
}

void
codegen_bp_resolve(uint32_t pc, uint32_t flags)
{
    const int taken    = flags & 1;
    const int backward = (flags >> 1) & 1;

    if (codegen_bp_penalty) {
        const int predicted = bp_two_level ? bp_predict_p6(pc, taken, backward) : bp_predict_p5(pc, taken, backward);

        if (predicted != taken)
            cycles -= codegen_bp_penalty;
    }
    if (taken)
        cycles -= codegen_bp_taken_cost;
}

void
codegen_timing_set(codegen_timing_t *timing)
{
    codegen_bp_penalty = (timing == &codegen_timing_p6) ? P6_MISPREDICT : (timing == &codegen_timing_pentium) ? P5_MISPREDICT : 0;
    bp_two_level = timing == &codegen_timing_p6;
    codegen_bp_taken_cost = (timing == &codegen_timing_486) ? 2 : 0;
    memset(p6_btb, 0, sizeof(p6_btb));
    memset(p5_btb, 0, sizeof(p5_btb));
    codegen_timing_start       = timing->start;
    codegen_timing_prefix      = timing->prefix;
    codegen_timing_opcode      = timing->opcode;
    codegen_timing_block_start = timing->block_start;
    codegen_timing_block_end   = timing->block_end;
    codegen_timing_jump_cycles = timing->jump_cycles;
}

int codegen_in_recompile;

static int      last_op_ssegs;
static x86seg  *last_op_ea_seg;
static uint32_t last_op_32;

void
codegen_generate_reset(void)
{
    last_op_ssegs  = -1;
    last_op_ea_seg = NULL;
    last_op_32     = -1;
    has_ea         = 0;
}

void
codegen_check_seg_read(UNUSED(codeblock_t *block), ir_data_t *ir, x86seg *seg)
{
    /*Segments always valid in real/V86 mode*/
    if (!(cr0 & 1) || (cpu_state.eflags & VM_FLAG))
        return;
    /*CS and SS must always be valid*/
    if (seg == &cpu_state.seg_cs || seg == &cpu_state.seg_ss)
        return;
    if (seg->checked)
        return;
    if (seg == &cpu_state.seg_ds && codegen_flat_ds && !(cpu_cur_status & CPU_STATUS_NOTFLATDS))
        return;

    uop_CMP_IMM_JZ(ir, ireg_seg_base(seg), (uint32_t) -1, codegen_gpf_rout);

    seg->checked = 1;
}
void
codegen_check_seg_write(UNUSED(codeblock_t *block), ir_data_t *ir, x86seg *seg)
{
    /*Segments always valid in real/V86 mode*/
    if (!(cr0 & 1) || (cpu_state.eflags & VM_FLAG))
        return;
    /*CS and SS must always be valid*/
    if (seg == &cpu_state.seg_cs || seg == &cpu_state.seg_ss)
        return;
    if (seg->checked)
        return;
    if (seg == &cpu_state.seg_ds && codegen_flat_ds && !(cpu_cur_status & CPU_STATUS_NOTFLATDS))
        return;

    uop_CMP_IMM_JZ(ir, ireg_seg_base(seg), (uint32_t) -1, codegen_gpf_rout);

    seg->checked = 1;
}

static x86seg *
codegen_generate_ea_16_long(ir_data_t *ir, x86seg *op_ea_seg, uint32_t fetchdat, int op_ssegs, uint32_t *op_pc)
{
    uint32_t old_pc = (*op_pc) + 1;
    if (!cpu_mod && cpu_rm == 6) {
        uint16_t addr = (fetchdat >> 8) & 0xffff;
        uop_MOV_IMM(ir, IREG_eaaddr, addr);
        (*op_pc) += 2;
    } else {
        int base_reg;
        int index_reg;
        int offset;

        switch (cpu_rm & 7) {
            default:
            case 0:
            case 1:
            case 7:
                base_reg = IREG_EBX;
                break;
            case 2:
            case 3:
            case 6:
                base_reg = IREG_EBP;
                break;
            case 4:
                base_reg = IREG_ESI;
                break;
            case 5:
                base_reg = IREG_EDI;
                break;
        }
        uop_MOV(ir, IREG_eaaddr, base_reg);

        if (!(cpu_rm & 4)) {
            if (!(cpu_rm & 1))
                index_reg = IREG_ESI;
            else
                index_reg = IREG_EDI;

            uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, index_reg);
        }

        switch (cpu_mod) {
            case 1:
                offset = (int) (int8_t) ((fetchdat >> 8) & 0xff);
                uop_ADD_IMM(ir, IREG_eaaddr, IREG_eaaddr, offset);
                (*op_pc)++;
                break;
            case 2:
                offset = (fetchdat >> 8) & 0xffff;
                uop_ADD_IMM(ir, IREG_eaaddr, IREG_eaaddr, offset);
                (*op_pc) += 2;
                break;

            default:
                break;
        }

        uop_AND_IMM(ir, IREG_eaaddr, IREG_eaaddr, 0xffff);

        if (mod1seg[cpu_rm] == &ss && !op_ssegs) {
            op_ea_seg = &cpu_state.seg_ss;
        }
    }

    codegen_mark_code_present(ir->block, cs + old_pc, ((*op_pc) + 1) - old_pc);
    return op_ea_seg;
}

static x86seg *
codegen_generate_ea_32_long(ir_data_t *ir, x86seg *op_ea_seg, uint32_t fetchdat, int op_ssegs, uint32_t *op_pc, int stack_offset)
{
    codeblock_t *block  = ir->block;
    uint32_t     old_pc = (*op_pc) + 1;
    uint32_t     new_eaaddr;
    int          extra_bytes = 0;

    if (cpu_rm == 4) {
        uint8_t sib = fetchdat >> 8;
        (*op_pc)++;

        switch (cpu_mod) {
            case 0:
                if ((sib & 7) == 5) {
                    if (block->flags & CODEBLOCK_NO_IMMEDIATES) {
                        LOAD_IMMEDIATE_FROM_RAM_32(block, ir, IREG_eaaddr, cs + (*op_pc) + 1);
                        extra_bytes = 1;
                    } else {
                        new_eaaddr = fastreadl(cs + (*op_pc) + 1);
                        uop_MOV_IMM(ir, IREG_eaaddr, new_eaaddr);
                        extra_bytes = 5;
                    }
                    (*op_pc) += 4;
                } else {
                    uop_MOV(ir, IREG_eaaddr, sib & 7);
                    extra_bytes = 1;
                }
                break;
            case 1:
                new_eaaddr = (uint32_t) (int8_t) ((fetchdat >> 16) & 0xff);
                uop_MOV_IMM(ir, IREG_eaaddr, new_eaaddr);
                uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, sib & 7);
                (*op_pc)++;
                extra_bytes = 2;
                break;
            case 2:
                if (block->flags & CODEBLOCK_NO_IMMEDIATES) {
                    LOAD_IMMEDIATE_FROM_RAM_32(block, ir, IREG_eaaddr, cs + (*op_pc) + 1);
                    extra_bytes = 1;
                } else {
                    new_eaaddr = fastreadl(cs + (*op_pc) + 1);
                    uop_MOV_IMM(ir, IREG_eaaddr, new_eaaddr);
                    extra_bytes = 5;
                }
                (*op_pc) += 4;
                uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, sib & 7);
                break;

            default:
                break;
        }
        if (stack_offset && (sib & 7) == 4 && (cpu_mod || (sib & 7) != 5)) { /*ESP*/
            uop_ADD_IMM(ir, IREG_eaaddr, IREG_eaaddr, stack_offset);
#if 0
            addbyte(0x05);
            addlong(stack_offset);
#endif
        }
        if (((sib & 7) == 4 || (cpu_mod && (sib & 7) == 5)) && !op_ssegs)
            op_ea_seg = &cpu_state.seg_ss;
        if (((sib >> 3) & 7) != 4) {
            switch (sib >> 6) {
                case 0:
                    uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, (sib >> 3) & 7);
                    break;
                case 1:
                    uop_ADD_LSHIFT(ir, IREG_eaaddr, IREG_eaaddr, (sib >> 3) & 7, 1);
                    break;
                case 2:
                    uop_ADD_LSHIFT(ir, IREG_eaaddr, IREG_eaaddr, (sib >> 3) & 7, 2);
                    break;
                case 3:
                    uop_ADD_LSHIFT(ir, IREG_eaaddr, IREG_eaaddr, (sib >> 3) & 7, 3);
                    break;

                default:
                    break;
            }
        }
    } else {
        if (!cpu_mod && cpu_rm == 5) {
            if (block->flags & CODEBLOCK_NO_IMMEDIATES) {
                LOAD_IMMEDIATE_FROM_RAM_32(block, ir, IREG_eaaddr, cs + (*op_pc) + 1);
            } else {
                new_eaaddr = fastreadl(cs + (*op_pc) + 1);
                uop_MOV_IMM(ir, IREG_eaaddr, new_eaaddr);
                extra_bytes = 4;
            }

            (*op_pc) += 4;
        } else {
            uop_MOV(ir, IREG_eaaddr, cpu_rm);
            if (cpu_mod) {
                if (cpu_rm == 5 && !op_ssegs)
                    op_ea_seg = &cpu_state.seg_ss;
                if (cpu_mod == 1) {
                    uop_ADD_IMM(ir, IREG_eaaddr, IREG_eaaddr, (uint32_t) (int8_t) (fetchdat >> 8));
                    (*op_pc)++;
                    extra_bytes = 1;
                } else {
                    if (block->flags & CODEBLOCK_NO_IMMEDIATES) {
                        LOAD_IMMEDIATE_FROM_RAM_32(block, ir, IREG_temp0, cs + (*op_pc) + 1);
                        uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, IREG_temp0);
                    } else {
                        new_eaaddr = fastreadl(cs + (*op_pc) + 1);
                        uop_ADD_IMM(ir, IREG_eaaddr, IREG_eaaddr, new_eaaddr);
                        extra_bytes = 4;
                    }
                    (*op_pc) += 4;
                }
            }
        }
    }

    if (extra_bytes)
        codegen_mark_code_present(ir->block, cs + old_pc, extra_bytes);

    return op_ea_seg;
}

x86seg *
codegen_generate_ea(ir_data_t *ir, x86seg *op_ea_seg, uint32_t fetchdat, int op_ssegs, uint32_t *op_pc, uint32_t op_32, int stack_offset)
{
    cpu_mod = (fetchdat >> 6) & 3;
    cpu_reg = (fetchdat >> 3) & 7;
    cpu_rm  = fetchdat & 7;

    if ((fetchdat & 0xc0) == 0xc0)
        return NULL;
    if (op_32 & 0x200)
        return codegen_generate_ea_32_long(ir, op_ea_seg, fetchdat, op_ssegs, op_pc, stack_offset);

    return codegen_generate_ea_16_long(ir, op_ea_seg, fetchdat, op_ssegs, op_pc);
}

// clang-format off
static uint8_t opcode_modrm[256] = {
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*00*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*10*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*20*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*30*/

    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*40*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*50*/
    0, 0, 1, 1,  0, 0, 0, 0,  0, 1, 0, 1,  0, 0, 0, 0, /*60*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*70*/

    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1, /*80*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*90*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*a0*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*b0*/

    1, 1, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0, /*c0*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  1, 1, 1, 1, /*d0*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*e0*/
    0, 0, 0, 0,  0, 0, 1, 1,  0, 0, 0, 0,  0, 0, 1, 1, /*f0*/
};

static uint8_t opcode_0f_modrm[256] = {
    1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0, 1, 0, 1, /*00*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  1, 1, 1, 1, /*10*/
    1, 1, 1, 1,  1, 1, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*20*/
    0, 0, 0, 0,  0, 0, 1, 1,  0, 0, 0, 0,  0, 0, 0, 1, /*30*/

    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1, /*40*/
    1, 1, 1, 0,  1, 1, 0, 0,  1, 1, 1, 1,  1, 1, 1, 0, /*50*/
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  0, 0, 1, 1, /*60*/
    0, 1, 1, 1,  1, 1, 1, 0,  1, 1, 1, 1,  1, 1, 1, 1, /*70*/

    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*80*/
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1, /*90*/
    0, 0, 0, 1,  1, 1, 0, 0,  0, 0, 0, 1,  1, 1, 1, 1, /*a0*/
    1, 1, 1, 1,  1, 1, 1, 1,  0, 0, 1, 1,  1, 1, 1, 1, /*b0*/

    1, 1, 0, 0,  0, 0, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0, /*c0*/
    0, 1, 1, 1,  0, 1, 0, 0,  1, 1, 0, 1,  1, 1, 0, 1, /*d0*/
    0, 1, 1, 0,  0, 1, 0, 0,  1, 1, 0, 1,  1, 1, 0, 1, /*e0*/
    0, 1, 1, 1,  0, 1, 0, 0,  1, 1, 1, 0,  1, 1, 1, 0  /*f0*/
};
// clang-format on

void
codegen_generate_call(uint8_t opcode, OpFn op, uint32_t fetchdat, uint32_t new_pc, uint32_t old_pc)
{
    codeblock_t *block              = &codeblock[block_current];
    ir_data_t   *ir                 = codegen_get_ir_data();
    uint32_t     op_pc              = new_pc;
    const OpFn  *op_table           = x86_dynarec_opcodes;
    RecompOpFn  *recomp_op_table    = recomp_opcodes;
    int          opcode_shift       = 0;
    int          opcode_mask        = 0x3ff;
    uint32_t     recomp_opcode_mask = 0x1ff;
    uint32_t     op_32              = use32;
    int          over               = 0;
    int          test_modrm         = 1;
    int          pc_off             = 0;
    int          in_lock            = 0;
    uint32_t     next_pc            = 0;
    uint16_t     op87               = 0x0000;
#ifdef DEBUG_EXTRA
    uint8_t last_prefix = 0;
#endif
    op_ea_seg = &cpu_state.seg_ds;
    op_ssegs  = 0;

    codegen_timing_start();

    while (!over) {
        switch (opcode) {
            case 0x0f:
#ifdef DEBUG_EXTRA
                last_prefix = 0x0f;
#endif
                op_table        = x86_dynarec_opcodes_0f;
                recomp_op_table = fpu_softfloat ? recomp_opcodes_0f_no_mmx : recomp_opcodes_0f;
                over            = 1;
                break;

            case 0x26: /*ES:*/
                op_ea_seg = &cpu_state.seg_es;
                op_ssegs  = 1;
                break;
            case 0x2e: /*CS:*/
                op_ea_seg = &cpu_state.seg_cs;
                op_ssegs  = 1;
                break;
            case 0x36: /*SS:*/
                op_ea_seg = &cpu_state.seg_ss;
                op_ssegs  = 1;
                break;
            case 0x3e: /*DS:*/
                op_ea_seg = &cpu_state.seg_ds;
                op_ssegs  = 1;
                break;
            case 0x64: /*FS:*/
                op_ea_seg = &cpu_state.seg_fs;
                op_ssegs  = 1;
                break;
            case 0x65: /*GS:*/
                op_ea_seg = &cpu_state.seg_gs;
                op_ssegs  = 1;
                break;

            case 0x66: /*Data size select*/
                op_32 = ((use32 & 0x100) ^ 0x100) | (op_32 & 0x200);
                break;
            case 0x67: /*Address size select*/
                op_32 = ((use32 & 0x200) ^ 0x200) | (op_32 & 0x100);
                break;

            case 0xd8:
#ifdef DEBUG_EXTRA
                last_prefix = 0xd8;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_d8_a32 : x86_dynarec_opcodes_d8_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_d8;
                opcode_shift    = 3;
                opcode_mask     = 0x1f;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xd9:
#ifdef DEBUG_EXTRA
                last_prefix = 0xd9;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_d9_a32 : x86_dynarec_opcodes_d9_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_d9;
                opcode_mask     = 0xff;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xda:
#ifdef DEBUG_EXTRA
                last_prefix = 0xda;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_da_a32 : x86_dynarec_opcodes_da_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_da;
                opcode_mask     = 0xff;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xdb:
#ifdef DEBUG_EXTRA
                last_prefix = 0xdb;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_db_a32 : x86_dynarec_opcodes_db_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_db;
                opcode_mask     = 0xff;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xdc:
#ifdef DEBUG_EXTRA
                last_prefix = 0xdc;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_dc_a32 : x86_dynarec_opcodes_dc_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_dc;
                opcode_shift    = 3;
                opcode_mask     = 0x1f;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xdd:
#ifdef DEBUG_EXTRA
                last_prefix = 0xdd;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_dd_a32 : x86_dynarec_opcodes_dd_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_dd;
                opcode_mask     = 0xff;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xde:
#ifdef DEBUG_EXTRA
                last_prefix = 0xde;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_de_a32 : x86_dynarec_opcodes_de_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_de;
                opcode_mask     = 0xff;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;
            case 0xdf:
#ifdef DEBUG_EXTRA
                last_prefix = 0xdf;
#endif
                op87            = (op87 & 0xf800) | ((opcode & 0x07) << 8) | (fetchdat & 0xff);
                op_table        = (op_32 & 0x200) ? x86_dynarec_opcodes_df_a32 : x86_dynarec_opcodes_df_a16;
                recomp_op_table = fpu_softfloat ? NULL : recomp_opcodes_df;
                opcode_mask     = 0xff;
                over            = 1;
                pc_off          = -1;
                test_modrm      = 0;
                block->flags |= CODEBLOCK_HAS_FPU;
                break;

            case 0xf0: /*LOCK*/
                in_lock         = 1;
                break;

            case 0xf2: /*REPNE*/
#ifdef DEBUG_EXTRA
                last_prefix = 0xf2;
#endif
                op_table        = x86_dynarec_opcodes_REPNE;
                recomp_op_table = NULL; // recomp_opcodes_REPNE;
                break;
            case 0xf3: /*REPE*/
#ifdef DEBUG_EXTRA
                last_prefix = 0xf3;
#endif
                op_table        = x86_dynarec_opcodes_REPE;
                recomp_op_table = NULL; // recomp_opcodes_REPE;
                break;

            default:
                goto generate_call;
        }
        fetchdat = fastreadl(cs + op_pc);
        codegen_timing_prefix(opcode, fetchdat);
        if (cpu_state.abrt)
            return;
        opcode = fetchdat & 0xff;
        if (!pc_off)
            fetchdat >>= 8;

        op_pc++;
    }

generate_call:
    codegen_instructions[block->ins].pc        = cpu_state.oldpc;
    codegen_instructions[block->ins].op_ssegs  = last_op_ssegs;
    codegen_instructions[block->ins].op_ea_seg = last_op_ea_seg;
    codegen_instructions[block->ins].op_32     = last_op_32;
    codegen_instructions[block->ins].TOP       = cpu_state.TOP;
    codegen_instructions[block->ins].first_uop = ir->wr_pos;

    codegen_timing_opcode(opcode, fetchdat, op_32, op_pc);

    codegen_accumulate(ir, ACCREG_cycles, -codegen_block_cycles);
    codegen_block_cycles = 0;

    if ((op_table == x86_dynarec_opcodes && ((opcode & 0xf0) == 0x70 || (opcode & 0xfc) == 0xe0 || opcode == 0xc2 || (opcode & 0xfe) == 0xca || (opcode & 0xfc) == 0xcc || (opcode & 0xfc) == 0xe8 || (opcode == 0xff && ((fetchdat & 0x38) >= 0x10 && (fetchdat & 0x38) < 0x30)))) || (op_table == x86_dynarec_opcodes_0f && ((opcode & 0xf0) == 0x80))) {
        /*On some CPUs (eg K6), a jump/branch instruction may be able to pair with
          subsequent instructions, so no cycles may have been deducted for it yet.
          To prevent having zero cycle blocks (eg with a jump instruction pointing
          to itself), apply the cycles that would be taken if this jump is taken,
          then reverse it for subsequent instructions if the jump is not taken*/
        int jump_cycles = 0;

        if (codegen_timing_jump_cycles)
            codegen_timing_jump_cycles();

        if (jump_cycles)
            codegen_accumulate(ir, ACCREG_cycles, -jump_cycles);
        codegen_accumulate_flush(ir);
        if (jump_cycles)
            codegen_accumulate(ir, ACCREG_cycles, jump_cycles);
    }

    if (op_table == x86_dynarec_opcodes_0f && opcode == 0x0f) {
        /*3DNow opcodes are stored after ModR/M, SIB and any offset*/
        uint8_t  modrm     = fetchdat & 0xff;
        uint8_t  sib       = (fetchdat >> 8) & 0xff;
        uint32_t opcode_pc = op_pc + 1;
        uint8_t  opcode_3dnow;

        if ((modrm & 0xc0) != 0xc0) {
            if (op_32 & 0x200) {
                if ((modrm & 7) == 4) {
                    /* Has SIB*/
                    opcode_pc++;
                    if ((modrm & 0xc0) == 0x40)
                        opcode_pc++;
                    else if ((modrm & 0xc0) == 0x80)
                        opcode_pc += 4;
                    else if ((sib & 0x07) == 0x05)
                        opcode_pc += 4;
                } else {
                    if ((modrm & 0xc0) == 0x40)
                        opcode_pc++;
                    else if ((modrm & 0xc0) == 0x80)
                        opcode_pc += 4;
                    else if ((modrm & 0xc7) == 0x05)
                        opcode_pc += 4;
                }
            } else {
                if ((modrm & 0xc0) == 0x40)
                    opcode_pc++;
                else if ((modrm & 0xc0) == 0x80)
                    opcode_pc += 2;
                else if ((modrm & 0xc7) == 0x06)
                    opcode_pc += 2;
            }
        }

        opcode_3dnow = fastreadb(cs + opcode_pc);
        if (!fpu_softfloat && recomp_opcodes_3DNOW[opcode_3dnow]) {
            next_pc = opcode_pc + 1;

            op_table           = x86_dynarec_opcodes_3DNOW;
            recomp_op_table    = fpu_softfloat ? NULL : recomp_opcodes_3DNOW;
            opcode             = opcode_3dnow;
            recomp_opcode_mask = 0xff;
            opcode_mask        = 0xff;
        }
    }
    codegen_mark_code_present(block, cs + old_pc, (op_pc - old_pc) - pc_off);
    if (op87 != 0x0000) {
        uop_MOV_IMM(ir, IREG_x87_op, op87);
    }
    /* It is apparently a prefixed instruction. */
#if 0
    if ((recomp_op_table == recomp_opcodes) && (opcode == 0x48))
        goto codegen_skip;
#endif
    if (in_lock && ((opcode == 0x90) || (opcode == 0xec)))
        goto codegen_skip;

    if (recomp_op_table && recomp_op_table[(opcode | op_32) & recomp_opcode_mask]) {
        uint32_t new_pc = recomp_op_table[(opcode | op_32) & recomp_opcode_mask](block, ir, opcode, fetchdat, op_32, op_pc);
        if (new_pc) {
            if (new_pc != -1)
                uop_MOV_IMM(ir, IREG_pc, new_pc);

            codegen_endpc = (cs + cpu_state.pc) + 8;

            block->ins++;

            if (block->ins >= MAX_INSTRUCTION_COUNT)
                CPU_BLOCK_END();

            return;
        }
    }

codegen_skip:
    if ((op_table == x86_dynarec_opcodes_REPNE || op_table == x86_dynarec_opcodes_REPE) && !op_table[opcode | op_32]) {
        op_table        = x86_dynarec_opcodes;
        recomp_op_table = recomp_opcodes;
    }

    if (in_lock && ((opcode == 0x90) || (opcode == 0xec)))
        /* This is always ILLEGAL. */
        op = x86_dynarec_opcodes_3DNOW[0xff];
    else
        op = op_table[((opcode >> opcode_shift) | op_32) & opcode_mask];

    if (!test_modrm || (op_table == x86_dynarec_opcodes && opcode_modrm[opcode]) || (op_table == x86_dynarec_opcodes_0f && opcode_0f_modrm[opcode]) || (op_table == x86_dynarec_opcodes_3DNOW)) {
        int stack_offset = 0;

        if (op_table == x86_dynarec_opcodes && opcode == 0x8f) /*POP*/
            stack_offset = (op_32 & 0x100) ? 4 : 2;

        cpu_mod = (fetchdat >> 6) & 3;
        cpu_reg = (fetchdat >> 3) & 7;
        cpu_rm  = fetchdat & 7;

        uop_MOV_IMM(ir, IREG_rm_mod_reg, cpu_rm | (cpu_mod << 8) | (cpu_reg << 16));

        op_pc += pc_off;
        if (cpu_mod != 3 && !(op_32 & 0x200)) {
            op_ea_seg = codegen_generate_ea_16_long(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc);
        }
        if (cpu_mod != 3 && (op_32 & 0x200)) {
            op_ea_seg = codegen_generate_ea_32_long(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, stack_offset);
        }
        op_pc -= pc_off;
    }

#ifdef DEBUG_EXTRA
    uop_LOG_INSTR(ir, opcode | (last_prefix << 8));
#endif
    codegen_accumulate_flush(ir);
    if (op_table == x86_dynarec_opcodes_3DNOW)
        uop_MOV_IMM(ir, IREG_pc, next_pc);
    else
        uop_MOV_IMM(ir, IREG_pc, op_pc + pc_off);
    uop_MOV_IMM(ir, IREG_oldpc, old_pc);
    if (op_32 != last_op_32)
        uop_MOV_IMM(ir, IREG_op32, op_32);
    if (op_ea_seg != last_op_ea_seg)
        uop_MOV_PTR(ir, IREG_ea_seg, (void *) op_ea_seg);
    if (op_ssegs != last_op_ssegs)
        uop_MOV_IMM(ir, IREG_ssegs, op_ssegs);
    uop_CALL_INSTRUCTION_FUNC(ir, op, fetchdat);
    codegen_flags_changed = 0;
    codegen_mark_code_present(block, cs + cpu_state.pc, 8);

    last_op_32     = op_32;
    last_op_ea_seg = op_ea_seg;
    last_op_ssegs  = op_ssegs;
#if 0
    codegen_block_ins++;
#endif

    block->ins++;

    if (block->ins >= MAX_INSTRUCTION_COUNT)
        CPU_BLOCK_END();

    codegen_endpc = (cs + cpu_state.pc) + 8;

#if 0
    if (has_ea)
        fatal("Has EA\n");
#endif
}

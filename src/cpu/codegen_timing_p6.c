/*Basic P6 timing model by plant/nerd73. Based on the K6 timing model*/
/*Some cycle timings come from https://www.agner.org/optimize/instruction_tables.pdf*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/machine.h>
#include <86box/plat_unused.h>

#include "x86.h"
#include "x86_ops.h"
#include "x86seg_common.h"
#include "x87_sf.h"
#include "x87.h"
#include "386_common.h"
#include "codegen.h"
#include "codegen_ops.h"
#include "codegen_timing_common.h"

typedef enum uop_type_t {
    UOP_ALU = 0,   /*Executes in Port 0 or 1 ALU units*/
    UOP_ALUP0,     /*Executes in Port 0 ALU unit*/
    UOP_LOAD,      /*Executes in Load unit*/
    UOP_STORED,    /*Executes in Data Store unit*/
    UOP_STOREA,    /*Executes in Address Store unit*/
    UOP_FLOAD,     /*Executes in Load unit*/
    UOP_FSTORED,   /*Executes in Data Store unit*/
    UOP_FSTOREA,   /*Executes in Address Store unit*/
    UOP_MLOAD,     /*Executes in Load unit*/
    UOP_MSTORED,   /*Executes in Data Store unit*/
    UOP_MSTOREA,   /*Executes in Address Store unit*/
    UOP_FLOAT,     /*Executes in Floating Point unit*/
    UOP_MMX,       /*Executes in Port 0 or 1 ALU units as MMX*/
    UOP_MMX_SHIFT, /*Executes in Port 1 ALU unit. Uses MMX shifter*/
    UOP_MMX_MUL,   /*Executes in Port 0 ALU unit. Uses MMX multiplier*/
    UOP_BRANCH,    /*Executes in Branch unit*/
    UOP_FXCH       /*Does not require an execution unit*/
} uop_type_t;

typedef enum decode_type_t {
    DECODE_SIMPLE,
    DECODE_COMPLEX,
} decode_type_t;

#define MAX_UOPS 10

typedef struct p6_uop_t {
    uop_type_t type;
    int        latency;
} p6_uop_t;

typedef struct macro_op_t {
    int           nr_uops;
    decode_type_t decode_type;
    p6_uop_t      uop[MAX_UOPS];
} macro_op_t;

static const macro_op_t alu_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_ALU, .latency = 1}
};
static const macro_op_t alup0_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t load_alu_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t load_alup0_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t alu_store_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_ALU,    .latency = 1},
    .uop[2]      = { .type = UOP_STORED, .latency = 1},
    .uop[3]      = { .type = UOP_STOREA, .latency = 1}
};
static const macro_op_t alup0_store_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0,  .latency = 1},
    .uop[2]      = { .type = UOP_STORED, .latency = 1},
    .uop[3]      = { .type = UOP_STOREA, .latency = 1}
};

static const macro_op_t branch_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_BRANCH, .latency = 2}
};

static const macro_op_t fxch_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FXCH, .latency = 1}
};

static const macro_op_t load_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1}
};

static const macro_op_t store_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_STORED,  .latency = 1},
    .uop[1]      = { .type = UOP_STOREA, .latency = 1}
};

static const macro_op_t bswap_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,  .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1},
};
static const macro_op_t leave_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1},
    .uop[2]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t lods_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t loop_op = {
    .nr_uops     = 5,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,     .latency = 1},
    .uop[1]      = { .type = UOP_ALU,    .latency = 1},
    .uop[2]      = { .type = UOP_ALU,    .latency = 1},
    .uop[3]      = { .type = UOP_ALU,    .latency = 1},
    .uop[4]      = { .type = UOP_BRANCH, .latency = 1}
};
static const macro_op_t mov_reg_seg_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
};
static const macro_op_t movs_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
    .uop[3]      = { .type = UOP_ALU,    .latency = 1}
};
static const macro_op_t pop_reg_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t pop_mem_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
    .uop[3]      = { .type = UOP_ALU,    .latency = 1}
};
static const macro_op_t push_imm_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_STORED,  .latency = 1},
    .uop[1]      = { .type = UOP_STOREA, .latency = 1},
};
static const macro_op_t push_mem_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1}
};
static const macro_op_t push_seg_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
    .uop[3]      = { .type = UOP_ALU,    .latency = 1}
};
static const macro_op_t stos_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[1]      = {.type = UOP_STORED,  .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
    .uop[3]      = { .type = UOP_ALU,    .latency = 1}
};
static const macro_op_t test_reg_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 1}
};
static const macro_op_t test_reg_b_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t test_mem_imm_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t test_mem_imm_b_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t xchg_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,  .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1},
    .uop[2]      = { .type = UOP_ALU, .latency = 1}
};

static const macro_op_t mmx_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_MMX, .latency = 1}
};
static const macro_op_t mmx_mul_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_MMX_MUL, .latency = 1}
};
static const macro_op_t mmx_shift_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_MMX_SHIFT, .latency = 1}
};
static const macro_op_t load_mmx_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 2},
    .uop[1]      = { .type = UOP_MMX, .latency = 2}
};
static const macro_op_t load_mmx_mul_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,     .latency = 2},
    .uop[1]      = { .type = UOP_MMX_MUL, .latency = 2}
};
static const macro_op_t load_mmx_shift_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,       .latency = 2},
    .uop[1]      = { .type = UOP_MMX_SHIFT, .latency = 2}
};
static const macro_op_t mload_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_MLOAD, .latency = 1},
};

static const macro_op_t mstore_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_MSTORED,  .latency = 1},
    .uop[1]      = { .type = UOP_MSTOREA, .latency = 1}
};
static const macro_op_t pmul_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_MMX_MUL, .latency = 1}
};
static const macro_op_t pmul_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,     .latency = 2},
    .uop[1]      = { .type = UOP_MMX_MUL, .latency = 2}
};
static const macro_op_t float_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 1}
};
static const macro_op_t fadd_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 2}
};
static const macro_op_t fmul_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 3}
};
static const macro_op_t float2_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAT,  .latency = 1},
    .uop[1]      = { .type = UOP_FLOAT, .latency = 1}
};
static const macro_op_t fchs_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAT,  .latency = 2},
    .uop[1]      = { .type = UOP_FLOAT, .latency = 2},
    .uop[2]      = { .type = UOP_FLOAT, .latency = 2}
};
static const macro_op_t load_float_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_FLOAT, .latency = 1}
};
static const macro_op_t load_fadd_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_FLOAT, .latency = 2}
};
static const macro_op_t load_fmul_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 4}
};
static const macro_op_t fstore_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FSTORED,  .latency = 1},
    .uop[1]      = { .type = UOP_FSTOREA, .latency = 1},
};
static const macro_op_t load_fiadd_op = {
    .nr_uops     = 7,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_FLOAT, .latency = 1},
    .uop[2]      = { .type = UOP_FLOAT, .latency = 1},
    .uop[3]      = { .type = UOP_FLOAT, .latency = 1},
    .uop[4]      = { .type = UOP_FLOAT, .latency = 1},
    .uop[5]      = { .type = UOP_FLOAT, .latency = 1},
    .uop[6]      = { .type = UOP_FLOAT, .latency = 1}
};
static const macro_op_t fdiv_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 37}
};
static const macro_op_t fdiv_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAD,  .latency = 1 },
    .uop[1]      = { .type = UOP_FLOAT, .latency = 37}
};
static const macro_op_t fsin_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 62}
};
static const macro_op_t fsqrt_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 69}
};

static const macro_op_t fldcw_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 10}
};
static const macro_op_t complex_float_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 1}
};
static const macro_op_t complex_float_l_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAT, .latency = 50}
};
static const macro_op_t flde_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_FLOAD, .latency = 1},
    .uop[2]      = { .type = UOP_FLOAT, .latency = 2}
};
static const macro_op_t fste_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_FLOAT,    .latency = 2},
    .uop[1]      = { .type = UOP_FSTORED, .latency = 1},
    .uop[2]      = { .type = UOP_FSTOREA, .latency = 1}
};

static const macro_op_t complex_alu1_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 1}
};
static const macro_op_t alu2_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,  .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t alu3_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,  .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1},
    .uop[2]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t alu6_op = {
    .nr_uops     = 6,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,  .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1},
    .uop[2]      = { .type = UOP_ALU, .latency = 1},
    .uop[3]      = { .type = UOP_ALU, .latency = 1},
    .uop[4]      = { .type = UOP_ALU, .latency = 1},
    .uop[5]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t complex_alup0_1_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t alup0_3_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0,  .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[2]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t alup0_6_op = {
    .nr_uops     = 6,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0,  .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[2]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[3]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[4]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[5]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t arpl_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,  .latency = 3},
    .uop[1]      = { .type = UOP_ALU, .latency = 3}
};
static const macro_op_t bound_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_LOAD, .latency = 1},
    .uop[2]      = { .type = UOP_ALU,  .latency = 1},
    .uop[3]      = { .type = UOP_ALU,  .latency = 1}
};
static const macro_op_t bsx_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 10}
};
static const macro_op_t call_far_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,     .latency = 3},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
    .uop[3]      = { .type = UOP_BRANCH, .latency = 1}
};
static const macro_op_t cli_sti_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 7}
};
static const macro_op_t cmps_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1},
    .uop[2]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t cmpsb_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[2]      = { .type = UOP_ALU,   .latency = 1}
};
static const macro_op_t cmpxchg_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_ALU,    .latency = 1},
    .uop[2]      = { .type = UOP_STORED, .latency = 1},
    .uop[3]      = { .type = UOP_STOREA, .latency = 1}
};
static const macro_op_t cmpxchg_b_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0,  .latency = 1},
    .uop[2]      = { .type = UOP_STORED, .latency = 1},
    .uop[3]      = { .type = UOP_STOREA, .latency = 1}
};
static const macro_op_t complex_push_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_STORED,  .latency = 1},
    .uop[1]      = { .type = UOP_STOREA, .latency = 1}
};

static const macro_op_t cpuid_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 23}
};
static const macro_op_t div16_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 21}
};
static const macro_op_t div16_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1 },
    .uop[1]      = { .type = UOP_ALUP0, .latency = 21}
};
static const macro_op_t div32_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 37}
};
static const macro_op_t div32_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1 },
    .uop[1]      = { .type = UOP_ALUP0, .latency = 37}
};
static const macro_op_t emms_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 50}
};
static const macro_op_t enter_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_STORED,  .latency = 1 },
    .uop[1]      = { .type = UOP_STOREA, .latency = 1 },
    .uop[2]      = { .type = UOP_ALU,    .latency = 10}
};
static const macro_op_t femms_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 6}
};
static const macro_op_t in_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 18}
};
static const macro_op_t ins_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 18},
    .uop[1]      = { .type = UOP_STORED, .latency = 1 },
    .uop[2]      = { .type = UOP_STOREA, .latency = 1 },
    .uop[3]      = { .type = UOP_ALU,    .latency = 1 }
};
static const macro_op_t int_op = {
    .nr_uops     = 8,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,     .latency = 20},
    .uop[1]      = { .type = UOP_STORED, .latency = 1 },
    .uop[2]      = { .type = UOP_STOREA, .latency = 1 },
    .uop[3]      = { .type = UOP_STORED, .latency = 1 },
    .uop[4]      = { .type = UOP_STOREA, .latency = 1 },
    .uop[5]      = { .type = UOP_STORED, .latency = 1 },
    .uop[6]      = { .type = UOP_STOREA, .latency = 1 },
    .uop[7]      = { .type = UOP_BRANCH, .latency = 1 }
};
static const macro_op_t iret_op = {
    .nr_uops     = 5,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 3 },
    .uop[1]      = { .type = UOP_LOAD,   .latency = 3 },
    .uop[2]      = { .type = UOP_LOAD,   .latency = 3 },
    .uop[3]      = { .type = UOP_ALU,    .latency = 20},
    .uop[4]      = { .type = UOP_BRANCH, .latency = 1 }
};
static const macro_op_t invd_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 500}
};
static const macro_op_t jmp_far_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,     .latency = 3},
    .uop[1]      = { .type = UOP_BRANCH, .latency = 1}
};
static const macro_op_t lss_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_LOAD, .latency = 1},
    .uop[2]      = { .type = UOP_ALU,  .latency = 3}
};
static const macro_op_t mov_mem_seg_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
};
static const macro_op_t mov_seg_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 3}
};
static const macro_op_t mov_seg_reg_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 3}
};
/*Integer multiply: latency 4, throughput 1 on port 0 (Intel P6 optimization manual; Agner Fog, instruction_tables, PPro IMUL)*/
static const macro_op_t mul_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_SIMPLE,
    .uop[0]      = {.type = UOP_ALUP0, .latency = 4}
};
static const macro_op_t mul_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 4}
};
static const macro_op_t mul64_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0,  .latency = 4},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[2]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t mul64_mem_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 4},
    .uop[2]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[3]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t out_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 18}
};
static const macro_op_t outs_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1 },
    .uop[1]      = { .type = UOP_ALU, .latency = 18}
};
static const macro_op_t pusha_op = {
    .nr_uops     = 8,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_STORED,  .latency = 2},
    .uop[1]      = { .type = UOP_STOREA, .latency = 2},
    .uop[2]      = { .type = UOP_STORED, .latency = 2},
    .uop[3]      = { .type = UOP_STOREA, .latency = 2},
    .uop[4]      = { .type = UOP_STORED, .latency = 2},
    .uop[5]      = { .type = UOP_STOREA, .latency = 2},
    .uop[6]      = { .type = UOP_STORED, .latency = 2},
    .uop[7]      = { .type = UOP_STOREA, .latency = 2}
};
static const macro_op_t popa_op = {
    .nr_uops     = 8,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,  .latency = 1},
    .uop[1]      = { .type = UOP_LOAD, .latency = 1},
    .uop[2]      = { .type = UOP_LOAD, .latency = 1},
    .uop[3]      = { .type = UOP_LOAD, .latency = 1},
    .uop[4]      = { .type = UOP_LOAD, .latency = 1},
    .uop[5]      = { .type = UOP_LOAD, .latency = 1},
    .uop[6]      = { .type = UOP_LOAD, .latency = 1},
    .uop[7]      = { .type = UOP_LOAD, .latency = 1}
};
static const macro_op_t popf_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1 },
    .uop[1]      = { .type = UOP_ALU,   .latency = 6 },
    .uop[2]      = { .type = UOP_ALUP0, .latency = 10}
};
static const macro_op_t pushf_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0,   .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1}
};
static const macro_op_t ret_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_BRANCH, .latency = 1}
};
static const macro_op_t retf_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_ALU,    .latency = 3},
    .uop[2]      = { .type = UOP_BRANCH, .latency = 1}
};
static const macro_op_t scas_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t scasb_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t setcc_mem_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0,    .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0,   .latency = 1},
    .uop[2]      = { .type = UOP_FSTORED, .latency = 1},
    .uop[3]      = { .type = UOP_FSTOREA, .latency = 1}
};
static const macro_op_t setcc_reg_op = {
    .nr_uops     = 3,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALUP0,  .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1},
    .uop[2]      = { .type = UOP_ALU,   .latency = 1}
};
static const macro_op_t test_mem_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD, .latency = 1},
    .uop[1]      = { .type = UOP_ALU, .latency = 1}
};
static const macro_op_t test_mem_b_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,   .latency = 1},
    .uop[1]      = { .type = UOP_ALUP0, .latency = 1}
};
static const macro_op_t xchg_mem_op = {
    .nr_uops     = 4,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_LOAD,    .latency = 1},
    .uop[1]      = { .type = UOP_STORED, .latency = 1},
    .uop[2]      = { .type = UOP_STOREA, .latency = 1},
    .uop[3]      = { .type = UOP_ALU,    .latency = 1}
};
static const macro_op_t xlat_op = {
    .nr_uops     = 2,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU,   .latency = 1},
    .uop[1]      = { .type = UOP_LOAD, .latency = 1}
};
static const macro_op_t wbinvd_op = {
    .nr_uops     = 1,
    .decode_type = DECODE_COMPLEX,
    .uop[0]      = {.type = UOP_ALU, .latency = 10000}
};
#define INVALID NULL

static const macro_op_t *opcode_timings_p6[256] = {
    // clang-format off
/*      ADD                    ADD                    ADD                   ADD*/
/*00*/  &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      ADD                    ADD                    PUSH ES               POP ES*/
        &alup0_op,             &alu_op,               &push_seg_op,         &mov_seg_mem_op,
/*      OR                     OR                     OR                    OR*/
        &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      OR                     OR                     PUSH CS               */
        &alup0_op,             &alu_op,               &push_seg_op,         INVALID,

/*      ADC                    ADC                    ADC                   ADC*/
/*10*/  &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      ADC                    ADC                    PUSH SS               POP SS*/
        &complex_alup0_1_op,   &complex_alu1_op,      &push_seg_op,         &mov_seg_mem_op,
/*      SBB                    SBB                    SBB                   SBB*/
/*10*/  &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      SBB                    SBB                    PUSH DS               POP DS*/
        &complex_alup0_1_op,   &complex_alu1_op,      &push_seg_op,         &mov_seg_mem_op,

/*      AND                    AND                    AND                   AND*/
/*20*/  &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      AND                    AND                                          DAA*/
        &alup0_op,             &alu_op,               INVALID,              &complex_alup0_1_op,
/*      SUB                    SUB                    SUB                   SUB*/
        &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      SUB                    SUB                                          DAS*/
        &alup0_op,             &alu_op,               INVALID,              &complex_alup0_1_op,

/*      XOR                    XOR                    XOR                   XOR*/
/*30*/  &alup0_store_op,       &alu_store_op,         &load_alup0_op,       &load_alu_op,
/*      XOR                    XOR                                          AAA*/
        &alup0_op,             &alu_op,               INVALID,              &alup0_6_op,
/*      CMP                    CMP                    CMP                   CMP*/
        &load_alup0_op,        &load_alu_op,          &load_alup0_op,       &load_alu_op,
/*      CMP                    CMP                                          AAS*/
        &alup0_op,             &alu_op,               INVALID,              &alup0_6_op,

/*      INC EAX                INC ECX                INC EDX                INC EBX*/
/*40*/  &alu_op,               &alu_op,               &alu_op,               &alu_op,
/*      INC ESP                INC EBP                INC ESI                INC EDI*/
        &alu_op,               &alu_op,               &alu_op,               &alu_op,
/*      DEC EAX                DEC ECX                DEC EDX                DEC EBX*/
        &alu_op,               &alu_op,               &alu_op,               &alu_op,
/*      DEC ESP                DEC EBP                DEC ESI                DEC EDI*/
        &alu_op,               &alu_op,               &alu_op,               &alu_op,

/*      PUSH EAX               PUSH ECX               PUSH EDX               PUSH EBX*/
/*50*/  &store_op,             &store_op,             &store_op,             &store_op,
/*      PUSH ESP               PUSH EBP               PUSH ESI               PUSH EDI*/
        &store_op,             &store_op,             &store_op,             &store_op,
/*      POP EAX                POP ECX                POP EDX                POP EBX*/
        &pop_reg_op,           &pop_reg_op,           &pop_reg_op,           &pop_reg_op,
/*      POP ESP                POP EBP                POP ESI                POP EDI*/
        &pop_reg_op,           &pop_reg_op,           &pop_reg_op,           &pop_reg_op,

/*      PUSHA                  POPA                   BOUND                  ARPL*/
/*60*/  &pusha_op,             &popa_op,              &bound_op,             &arpl_op,
        INVALID,               INVALID,               INVALID,               INVALID,
/*      PUSH imm               IMUL                   PUSH imm               IMUL*/
        &push_imm_op,          &mul_op,               &push_imm_op,          &mul_op,
/*      INSB                   INSW                   OUTSB                  OUTSW*/
        &ins_op,               &ins_op,               &outs_op,              &outs_op,

/*      Jxx*/
/*70*/  &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,

/*80*/  INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      TEST                       TEST                       XCHG                       XCHG*/
        &test_mem_b_op,            &test_mem_op,              &xchg_mem_op,              &xchg_mem_op,
/*      MOV                        MOV                        MOV                        MOV*/
        &store_op,                 &store_op,                 &load_op,                  &load_op,
/*      MOV from seg               LEA                        MOV to seg                 POP*/
        &mov_mem_seg_op,           &store_op,                 &mov_seg_mem_op,           &pop_mem_op,

/*      NOP                        XCHG                       XCHG                       XCHG*/
/*90*/  &fxch_op,                  &xchg_op,                  &xchg_op,                  &xchg_op,
/*      XCHG                       XCHG                       XCHG                       XCHG*/
        &xchg_op,                  &xchg_op,                  &xchg_op,                  &xchg_op,
/*      CBW                        CWD                        CALL far                   WAIT*/
        &complex_alu1_op,          &complex_alu1_op,          &call_far_op,              &fxch_op,
/*      PUSHF                      POPF                       SAHF                       LAHF*/
        &pushf_op,                 &popf_op,                  &complex_alup0_1_op,       &complex_alup0_1_op,

/*      MOV                        MOV                        MOV                        MOV*/
/*a0*/  &load_op,                  &load_op,                  &store_op,                 &store_op,
/*      MOVSB                      MOVSW                      CMPSB                      CMPSW*/
        &movs_op,                  &movs_op,                  &cmpsb_op,                 &cmps_op,
/*      TEST                       TEST                       STOSB                      STOSW*/
        &test_reg_b_op,            &test_reg_op,              &stos_op,                  &stos_op,
/*      LODSB                      LODSW                      SCASB                      SCASW*/
        &lods_op,                  &lods_op,                  &scasb_op,                 &scas_op,

/*      MOV*/
/*b0*/  &alu_op,       &alu_op,       &alu_op,       &alu_op,
        &alu_op,       &alu_op,       &alu_op,       &alu_op,
        &alu_op,       &alu_op,       &alu_op,       &alu_op,
        &alu_op,       &alu_op,       &alu_op,       &alu_op,

/*                                                            RET imm                    RET*/
/*c0*/  INVALID,                   INVALID,                   &ret_op,                   &ret_op,
/*      LES                        LDS                        MOV                        MOV*/
        &lss_op,                   &lss_op,                   &store_op,                 &store_op,
/*      ENTER                      LEAVE                      RETF                       RETF*/
        &enter_op,                 &leave_op,                 &retf_op,                  &retf_op,
/*      INT3                       INT                        INTO                       IRET*/
        &int_op,                   &int_op,                   &int_op,                   &iret_op,


/*d0*/  INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      AAM                        AAD                        SETALC                     XLAT*/
        &alup0_6_op,               &alup0_3_op,               &complex_alup0_1_op,       &xlat_op,
        INVALID,                   INVALID,                   INVALID,                   INVALID,
        INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      LOOPNE                     LOOPE                      LOOP                       JCXZ*/
/*e0*/  &loop_op,                  &loop_op,                  &loop_op,                  &loop_op,
/*      IN AL                      IN AX                      OUT_AL                     OUT_AX*/
        &in_op,                    &in_op,                    &out_op,                   &out_op,
/*      CALL                       JMP                        JMP                        JMP*/
        &store_op,                 &branch_op,                &jmp_far_op,               &branch_op,
/*      IN AL                      IN AX                      OUT_AL                     OUT_AX*/
        &in_op,                    &in_op,                    &out_op,                   &out_op,

/*                                                            REPNE                      REPE*/
/*f0*/  INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      HLT                        CMC*/
        &complex_alup0_1_op,       &alu2_op,                  INVALID,                   INVALID,
/*      CLC                        STC                        CLI                        STI*/
        &complex_alu1_op,          &complex_alu1_op,          &cli_sti_op,               &cli_sti_op,
/*      CLD                        STD                        INCDEC*/
        &complex_alu1_op,          &complex_alu1_op,          &alup0_store_op,           INVALID
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_mod3[256] = {
    // clang-format off
/*      ADD                       ADD                       ADD                       ADD*/
/*00*/  &alup0_op,                &alu_op,                  &alup0_op,                &alu_op,
/*      ADD                       ADD                       PUSH ES                   POP ES*/
        &alup0_op,                &alu_op,                  &push_seg_op,             &mov_seg_mem_op,
/*      OR                        OR                        OR                        OR*/
        &alup0_op,                &alu_op,                  &alup0_op,                &alu_op,
/*      OR                        OR                        PUSH CS                   */
        &alup0_op,                &alu_op,                  &push_seg_op,             INVALID,

/*      ADC                       ADC                       ADC                       ADC*/
/*10*/  &complex_alup0_1_op,      &complex_alu1_op,         &complex_alup0_1_op,      &complex_alu1_op,
/*      ADC                       ADC                       PUSH SS                   POP SS*/
        &complex_alup0_1_op,      &complex_alu1_op,         &push_seg_op,             &mov_seg_mem_op,
/*      SBB                       SBB                       SBB                       SBB*/
        &complex_alup0_1_op,      &complex_alu1_op,         &complex_alup0_1_op,      &complex_alu1_op,
/*      SBB                       SBB                       PUSH DS                   POP DS*/
        &complex_alup0_1_op,      &complex_alu1_op,         &push_seg_op,             &mov_seg_mem_op,

/*      AND                       AND                       AND                       AND*/
/*20*/  &alup0_op,                &alu_op,                  &alup0_op,                &alu_op,
/*      AND                       AND                                                 DAA*/
        &alup0_op,                &alu_op,                  INVALID,                  &complex_alup0_1_op,
/*      SUB                       SUB                       SUB                       SUB*/
        &alup0_op,                &alu_op,                  &alup0_op,                &alu_op,
/*      SUB                       SUB                                                 DAS*/
        &alup0_op,                &alu_op,                  INVALID,                  &complex_alup0_1_op,

/*      XOR                       XOR                       XOR                       XOR*/
/*30*/  &alup0_op,                &alu_op,                  &alup0_op,                &alu_op,
/*      XOR                       XOR                                                 AAA*/
        &alup0_op,                &alu_op,                  INVALID,                  &alup0_6_op,
/*      CMP                       CMP                       CMP                       CMP*/
        &alup0_op,                &alu_op,                  &alup0_op,                &alu_op,
/*      CMP                       CMP                                                 AAS*/
        &alup0_op,                &alu_op,                  INVALID,                  &alup0_6_op,

/*      INC EAX                INC ECX                INC EDX                INC EBX*/
/*40*/  &alu_op,               &alu_op,               &alu_op,               &alu_op,
/*      INC ESP                INC EBP                INC ESI                INC EDI*/
        &alu_op,               &alu_op,               &alu_op,               &alu_op,
/*      DEC EAX                DEC ECX                DEC EDX                DEC EBX*/
        &alu_op,               &alu_op,               &alu_op,               &alu_op,
/*      DEC ESP                DEC EBP                DEC ESI                DEC EDI*/
        &alu_op,               &alu_op,               &alu_op,               &alu_op,

/*      PUSH EAX               PUSH ECX               PUSH EDX               PUSH EBX*/
/*50*/  &store_op,             &store_op,             &store_op,             &store_op,
/*      PUSH ESP               PUSH EBP               PUSH ESI               PUSH EDI*/
        &store_op,             &store_op,             &store_op,             &store_op,
/*      POP EAX                POP ECX                POP EDX                POP EBX*/
        &pop_reg_op,           &pop_reg_op,           &pop_reg_op,           &pop_reg_op,
/*      POP ESP                POP EBP                POP ESI                POP EDI*/
        &pop_reg_op,           &pop_reg_op,           &pop_reg_op,           &pop_reg_op,

/*      PUSHA                  POPA                   BOUND                  ARPL*/
/*60*/  &pusha_op,             &popa_op,              &bound_op,             &arpl_op,
        INVALID,               INVALID,               INVALID,               INVALID,
/*      PUSH imm               IMUL                   PUSH imm               IMUL*/
        &push_imm_op,          &mul_op,               &push_imm_op,          &mul_op,
/*      INSB                   INSW                   OUTSB                  OUTSW*/
        &ins_op,               &ins_op,               &outs_op,              &outs_op,

/*      Jxx*/
/*70*/  &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,

/*80*/  INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      TEST                       TEST                       XCHG                       XCHG*/
        &complex_alu1_op,          &complex_alu1_op,          &alu3_op,                  &alu3_op,
/*      MOV                        MOV                        MOV                        MOV*/
        &store_op,                 &store_op,                 &load_op,                  &load_op,
/*      MOV from seg               LEA                        MOV to seg                 POP*/
        &mov_reg_seg_op,           &store_op,                 &mov_seg_reg_op,           &pop_reg_op,

/*      NOP                        XCHG                       XCHG                       XCHG*/
/*90*/  &fxch_op,                  &xchg_op,                  &xchg_op,                  &xchg_op,
/*      XCHG                       XCHG                       XCHG                       XCHG*/
        &xchg_op,                  &xchg_op,                  &xchg_op,                  &xchg_op,
/*      CBW                        CWD                        CALL far                   WAIT*/
        &complex_alu1_op,          &complex_alu1_op,          &call_far_op,              &fxch_op,
/*      PUSHF                      POPF                       SAHF                       LAHF*/
        &pushf_op,                 &popf_op,                  &complex_alup0_1_op,       &complex_alup0_1_op,

/*      MOV                        MOV                        MOV                        MOV*/
/*a0*/  &load_op,                  &load_op,                  &store_op,                 &store_op,
/*      MOVSB                      MOVSW                      CMPSB                      CMPSW*/
        &movs_op,                  &movs_op,                  &cmpsb_op,                 &cmps_op,
/*      TEST                       TEST                       STOSB                      STOSW*/
        &test_reg_b_op,            &test_reg_op,              &stos_op,                  &stos_op,
/*      LODSB                      LODSW                      SCASB                      SCASW*/
        &lods_op,                  &lods_op,                  &scasb_op,                 &scas_op,

/*      MOV*/
/*b0*/  &alu_op,       &alu_op,       &alu_op,       &alu_op,
        &alu_op,       &alu_op,       &alu_op,       &alu_op,
        &alu_op,       &alu_op,       &alu_op,       &alu_op,
        &alu_op,       &alu_op,       &alu_op,       &alu_op,

/*                                                            RET imm                    RET*/
/*c0*/  INVALID,                   INVALID,                   &ret_op,                   &ret_op,
/*      LES                        LDS                        MOV                        MOV*/
        &lss_op,                   &lss_op,                   &store_op,                 &store_op,
/*      ENTER                      LEAVE                      RETF                       RETF*/
        &enter_op,                 &leave_op,                 &retf_op,                  &retf_op,
/*      INT3                       INT                        INTO                       IRET*/
        &int_op,                   &int_op,                   &int_op,                   &iret_op,


/*d0*/  INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      AAM                        AAD                        SETALC                     XLAT*/
        &alup0_6_op,               &alup0_3_op,               &complex_alup0_1_op,       &xlat_op,
        INVALID,                   INVALID,                   INVALID,                   INVALID,
        INVALID,                   INVALID,                   INVALID,                   INVALID,

/*      LOOPNE                     LOOPE                      LOOP                       JCXZ*/
/*e0*/  &loop_op,                  &loop_op,                  &loop_op,                  &loop_op,
/*      IN AL                      IN AX                      OUT_AL                     OUT_AX*/
        &in_op,                    &in_op,                    &out_op,                   &out_op,
/*      CALL                       JMP                        JMP                        JMP*/
        &store_op,                 &branch_op,                &jmp_far_op,               &branch_op,
/*      IN AL                      IN AX                      OUT_AL                     OUT_AX*/
        &in_op,                    &in_op,                    &out_op,                   &out_op,

/*                                                            REPNE                      REPE*/
/*f0*/  INVALID,                   INVALID,                   INVALID,                   INVALID,
/*      HLT                        CMC*/
        &complex_alup0_1_op,       &alu2_op,                  INVALID,                   INVALID,
/*      CLC                        STC                        CLI                        STI*/
        &complex_alu1_op,          &complex_alu1_op,          &cli_sti_op,               &cli_sti_op,
/*      CLD                        STD                        INCDEC*/
        &complex_alu1_op,          &complex_alu1_op,          &complex_alup0_1_op,       INVALID
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_0f[256] = {
    // clang-format off
/*00*/  &alu6_op,               &alu6_op,               &alu6_op,               &alu6_op,
        INVALID,                &alu6_op,               &alu6_op,               INVALID,
        &invd_op,               &wbinvd_op,             INVALID,                INVALID,
        INVALID,                &load_op,               &femms_op,              INVALID,

/*10*/  INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,

/*20*/  &alu6_op,               &alu6_op,               &alu6_op,               &alu6_op,
        &alu6_op,               &alu6_op,               INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,

/*30*/  &alu6_op,               &alu6_op,               &alu6_op,               INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,

/*40*/  INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,

/*50*/  INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                INVALID,                INVALID,

/*60*/  &load_mmx_op,           &load_mmx_op,           &load_mmx_op,           &load_mmx_op,
        &load_mmx_op,           &load_mmx_op,           &load_mmx_op,           &load_mmx_op,
        &load_mmx_op,           &load_mmx_op,           &load_mmx_op,           &load_mmx_op,
        INVALID,                INVALID,                &mload_op,              &mload_op,

/*70*/  INVALID,                &load_mmx_shift_op,     &load_mmx_shift_op,     &load_mmx_shift_op,
        &load_mmx_op,           &load_mmx_op,           &load_mmx_op,           &emms_op,
        INVALID,                INVALID,                INVALID,                INVALID,
        INVALID,                INVALID,                &mstore_op,             &mstore_op,

/*80*/  &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,

/*90*/  &setcc_reg_op,           &setcc_reg_op,          &setcc_reg_op,          &setcc_reg_op,
        &setcc_reg_op,           &setcc_reg_op,          &setcc_reg_op,          &setcc_reg_op,
        &setcc_reg_op,           &setcc_reg_op,          &setcc_reg_op,          &setcc_reg_op,
        &setcc_reg_op,           &setcc_reg_op,          &setcc_reg_op,          &setcc_reg_op,

/*a0*/  &push_seg_op,            &mov_seg_mem_op,        &cpuid_op,              &load_alu_op,
        &alu_store_op,           &alu_store_op,          INVALID,                INVALID,
        &push_seg_op,            &mov_seg_mem_op,        INVALID,                &load_alu_op,
        &alu_store_op,           &alu_store_op,          INVALID,                &mul_op,

/*b0*/  &cmpxchg_b_op,           &cmpxchg_op,            &lss_op,                &load_alu_op,
        &lss_op,                 &lss_op,                &load_alup0_op,         &load_alu_op,
        INVALID,                 INVALID,                &load_alu_op,           &load_alu_op,
        &bsx_op,                 &bsx_op,                &load_alup0_op,         &load_alu_op,

/*c0*/  &alup0_store_op,         &alu_store_op,          INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                &cmpxchg_op,
        &bswap_op,               &bswap_op,              &bswap_op,              &bswap_op,
        &bswap_op,               &bswap_op,              &bswap_op,              &bswap_op,

/*d0*/  INVALID,                 &load_mmx_shift_op,     &load_mmx_shift_op,     &load_mmx_shift_op,
        INVALID,                 &load_mmx_mul_op,       INVALID,                INVALID,
        &load_mmx_op,            &load_mmx_op,           INVALID,                &load_mmx_op,
        &load_mmx_op,            &load_mmx_op,           INVALID,                &load_mmx_op,

/*e0*/  &load_mmx_op,            &load_mmx_shift_op,     &load_mmx_shift_op,     INVALID,
        INVALID,                 &pmul_mem_op,           INVALID,                INVALID,
        &load_mmx_op,            &load_mmx_op,           INVALID,                &load_mmx_op,
        &load_mmx_op,            &load_mmx_op,           INVALID,                &load_mmx_op,

/*f0*/  INVALID,                 &load_mmx_shift_op,     &load_mmx_shift_op,     &load_mmx_shift_op,
        INVALID,                 &pmul_mem_op,           INVALID,                INVALID,
        &load_mmx_op,            &load_mmx_op,           &load_mmx_op,           INVALID,
        &load_mmx_op,            &load_mmx_op,           &load_mmx_op,           INVALID,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_0f_mod3[256] = {
    // clang-format off
/*00*/  &alu6_op,                &alu6_op,               &alu6_op,               &alu6_op,
        INVALID,                 &alu6_op,               &alu6_op,               INVALID,
        &invd_op,                &wbinvd_op,             INVALID,                INVALID,
        INVALID,                 INVALID,                &femms_op,              INVALID,

/*10*/  INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,

/*20*/  &alu6_op,                &alu6_op,               &alu6_op,               &alu6_op,
        &alu6_op,                &alu6_op,               INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,

/*30*/  &alu6_op,                &alu6_op,               &alu6_op,               INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,

/*40*/  INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,

/*50*/  INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,

/*60*/  &mmx_op,                 &mmx_op,                &mmx_op,                &mmx_op,
        &mmx_op,                 &mmx_op,                &mmx_op,                &mmx_op,
        &mmx_op,                 &mmx_op,                &mmx_op,                &mmx_op,
        INVALID,                 INVALID,                &mmx_op,                &mmx_op,

/*70*/  INVALID,                 &mmx_shift_op,          &mmx_shift_op,          &mmx_shift_op,
        &mmx_op,                 &mmx_op,                &mmx_op,                &emms_op,
        INVALID,                 INVALID,                INVALID,                INVALID,
        INVALID,                 INVALID,                &mmx_op,                &mmx_op,

/*80*/  &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,
        &branch_op,     &branch_op,     &branch_op,     &branch_op,

/*90*/  &setcc_mem_op,           &setcc_mem_op,          &setcc_mem_op,          &setcc_mem_op,
        &setcc_mem_op,           &setcc_mem_op,          &setcc_mem_op,          &setcc_mem_op,
        &setcc_mem_op,           &setcc_mem_op,          &setcc_mem_op,          &setcc_mem_op,
        &setcc_mem_op,           &setcc_mem_op,          &setcc_mem_op,          &setcc_mem_op,

/*a0*/  &push_seg_op,            &mov_seg_mem_op,        &cpuid_op,              &complex_alu1_op,
        &complex_alu1_op,        &complex_alu1_op,       INVALID,                INVALID,
        &push_seg_op,            &mov_seg_mem_op,        INVALID,                &complex_alu1_op,
        &complex_alu1_op,        &complex_alu1_op,       INVALID,                &mul_op,

/*b0*/  &cmpxchg_b_op,           &cmpxchg_op,            &lss_op,                &complex_alu1_op,
        &lss_op,                 &lss_op,                &alup0_op,              &alu_op,
        INVALID,                 INVALID,                &complex_alu1_op,       &complex_alu1_op,
        &bsx_op,                 &bsx_op,                &alup0_op,              &alu_op,

/*c0*/  &complex_alup0_1_op,     &complex_alu1_op,       INVALID,                INVALID,
        INVALID,                 INVALID,                INVALID,                INVALID,
        &bswap_op,               &bswap_op,              &bswap_op,              &bswap_op,
        &bswap_op,               &bswap_op,              &bswap_op,              &bswap_op,

/*d0*/  INVALID,                 &mmx_shift_op,          &mmx_shift_op,          &mmx_shift_op,
        INVALID,                 &mmx_mul_op,            INVALID,                INVALID,
        &mmx_op,                 &mmx_op,                INVALID,                &mmx_op,
        &mmx_op,                 &mmx_op,                INVALID,                &mmx_op,

/*e0*/  &mmx_op,                 &mmx_shift_op,          &mmx_shift_op,          INVALID,
        INVALID,                 &pmul_op,               INVALID,                INVALID,
        &mmx_op,                 &mmx_op,                INVALID,                &mmx_op,
        &mmx_op,                 &mmx_op,                INVALID,                &mmx_op,

/*f0*/  INVALID,                 &mmx_shift_op,          &mmx_shift_op,          &mmx_shift_op,
        INVALID,                 &pmul_op,               INVALID,                INVALID,
        &mmx_op,                 &mmx_op,                &mmx_op,                INVALID,
        &mmx_op,                 &mmx_op,                &mmx_op,                INVALID,
};

static const macro_op_t *opcode_timings_p6_shift[8] =
{
    // clang-format off
        &alu_store_op,   &alu_store_op,   &alu_store_op,   &alu_store_op,
        &alu_store_op,   &alu_store_op,   &alu_store_op,   &alu_store_op
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_shift_b[8] = {
    // clang-format off
        &alup0_store_op,  &alup0_store_op,  &alup0_store_op,  &alup0_store_op,
        &alup0_store_op,  &alup0_store_op,  &alup0_store_op,  &alup0_store_op
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_shift_mod3[8] = {
    // clang-format off
        &complex_alu1_op,   &complex_alu1_op,   &complex_alu1_op,   &complex_alu1_op,
        &alu_op,            &alu_op,            &alu_op,            &alu_op
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_shift_b_mod3[8] = {
    // clang-format off
        &complex_alup0_1_op,  &complex_alup0_1_op,  &complex_alup0_1_op,  &complex_alup0_1_op,
        &alup0_op,            &alup0_op,            &alup0_op,            &alup0_op
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_80[8] = {
    // clang-format off
        &alup0_store_op, &alup0_store_op, &alup0_store_op,  &alup0_store_op,
        &alup0_store_op, &alup0_store_op, &alup0_store_op,  &alup0_store_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_80_mod3[8] = {
    // clang-format off
        &alup0_op,       &alup0_op,       &alup0_store_op,  &alup0_store_op,
        &alup0_op,       &alup0_op,       &alup0_op,        &alup0_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_8x[8] = {
    // clang-format off
        &alu_store_op,  &alu_store_op,    &alu_store_op,    &alu_store_op,
        &alu_store_op,  &alu_store_op,    &alu_store_op,    &alu_store_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_8x_mod3[8] = {
    // clang-format off
        &alu_op,        &alu_op,          &alu_store_op,    &alu_store_op,
        &alu_op,        &alu_op,          &alu_op,          &alu_op,
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_f6[8] = {
    // clang-format off
/*      TST                                             NOT                     NEG*/
        &test_mem_imm_b_op,     INVALID,                &alup0_store_op,        &alup0_store_op,
/*      MUL                     IMUL                    DIV                     IDIV*/
        &mul_mem_op,            &mul_mem_op,            &div16_mem_op,          &div16_mem_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_f6_mod3[8] = {
    // clang-format off
/*      TST                                             NOT                     NEG*/
        &test_reg_b_op,         INVALID,                &alup0_op,              &alup0_op,
/*      MUL                     IMUL                    DIV                     IDIV*/
        &mul_op,                &mul_op,                &div16_op,              &div16_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_f7[8] = {
    // clang-format off
/*      TST                                             NOT                     NEG*/
        &test_mem_imm_op,       INVALID,                &alu_store_op,          &alu_store_op,
/*      MUL                     IMUL                    DIV                     IDIV*/
        &mul64_mem_op,          &mul64_mem_op,          &div32_mem_op,          &div32_mem_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_f7_mod3[8] = {
    // clang-format off
/*      TST                                             NOT                     NEG*/
        &test_reg_op,           INVALID,                &alu_op,                &alu_op,
/*      MUL                     IMUL                    DIV                     IDIV*/
        &mul64_op,              &mul64_op,              &div32_op,              &div32_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_ff[8] = {
    // clang-format off
/*      INC                     DEC                     CALL                    CALL far*/
        &alu_store_op,          &alu_store_op,          &store_op,              &call_far_op,
/*      JMP                     JMP far                 PUSH*/
        &branch_op,             &jmp_far_op,            &push_mem_op,           INVALID
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_ff_mod3[8] = {
    // clang-format off
/*      INC                     DEC                     CALL                    CALL far*/
        &complex_alu1_op,       &complex_alu1_op,       &store_op,              &call_far_op,
/*      JMP                     JMP far                 PUSH*/
        &branch_op,             &jmp_far_op,            &complex_push_mem_op,   INVALID
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_d8[8] = {
    // clang-format off
/*      FADDs            FMULs            FCOMs            FCOMPs*/
        &load_fadd_op,   &load_fmul_op,   &load_float_op,  &load_float_op,
/*      FSUBs            FSUBRs           FDIVs            FDIVRs*/
        &load_float_op,  &load_float_op,  &fdiv_mem_op,    &fdiv_mem_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_d8_mod3[8] = {
    // clang-format off
/*      FADD             FMUL             FCOM             FCOMP*/
        &fadd_op,        &fmul_op,        &float_op,       &float_op,
/*      FSUB             FSUBR            FDIV             FDIVR*/
        &float_op,       &float_op,       &fdiv_op,        &fdiv_op,
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_d9[8] = {
    // clang-format off
/*      FLDs                                    FSTs                 FSTPs*/
        &load_float_op,      INVALID,           &fstore_op,          &fstore_op,
/*      FLDENV               FLDCW              FSTENV               FSTCW*/
        &complex_float_l_op, &fldcw_op,         &complex_float_l_op, &complex_float_op
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_d9_mod3[64] = {
    // clang-format off
        /*FLD*/
        &float_op,    &float_op,    &float_op,    &float_op,
        &float_op,    &float_op,    &float_op,    &float_op,
        /*FXCH*/
        &fxch_op,     &fxch_op,     &fxch_op,     &fxch_op,
        &fxch_op,     &fxch_op,     &fxch_op,     &fxch_op,
        /*FNOP*/
        &float_op,    INVALID,      INVALID,      INVALID,
        INVALID,      INVALID,      INVALID,      INVALID,
        /*FSTP*/
        &float2_op,   &float2_op,   &float2_op,   &float2_op,
        &float2_op,   &float2_op,   &float2_op,   &float2_op,
/*      opFCHS        opFABS*/
        &fchs_op,     &float_op,    INVALID,      INVALID,
/*      opFTST        opFXAM*/
        &float_op,    &float_op,    INVALID,      INVALID,
/*      opFLD1        opFLDL2T      opFLDL2E      opFLDPI*/
        &float_op,    &float_op,    &float_op,    &float_op,
/*      opFLDEG2      opFLDLN2      opFLDZ*/
        &float_op,    &float_op,    &float_op,    INVALID,
/*      opF2XM1       opFYL2X       opFPTAN       opFPATAN*/
        &fsin_op,     &fsin_op,     &fsin_op,     &fsin_op,
/*                                  opFDECSTP     opFINCSTP,*/
        INVALID,      INVALID,      &float_op,    &float_op,
/*      opFPREM                     opFSQRT       opFSINCOS*/
        &fdiv_op,     INVALID,      &fsqrt_op,    &fsin_op,
/*      opFRNDINT     opFSCALE      opFSIN        opFCOS*/
        &float_op,    &fdiv_op,     &fsin_op,     &fsin_op
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_da[8] = {
    // clang-format off
/*      FIADDl            FIMULl            FICOMl            FICOMPl*/
        &load_fadd_op,    &load_fmul_op,    &load_float_op,   &load_float_op,
/*      FISUBl            FISUBRl           FIDIVl            FIDIVRl*/
        &load_float_op,   &load_float_op,   &fdiv_mem_op,     &fdiv_mem_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_da_mod3[8] = {
    // clang-format off
        INVALID,          INVALID,          INVALID,          INVALID,
/*                        FCOMPP*/
        INVALID,          &float_op,        INVALID,          INVALID
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_db[8] = {
    // clang-format off
/*      FLDil                               FSTil         FSTPil*/
        &load_float_op,   INVALID,          &fstore_op,   &fstore_op,
/*                        FLDe                            FSTPe*/
        INVALID,          &flde_op,         INVALID,      &fste_op
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_db_mod3[64] = {
    // clang-format off
        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,

        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,

        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,

        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,

/*                        opFNOP            opFCLEX       opFINIT*/
        INVALID,          &float_op,        &float_op,    &float_op,
/*      opFNOP            opFNOP*/
        &float_op,        &float_op,        INVALID,      INVALID,

        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,

        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,

        INVALID,          INVALID,          INVALID,      INVALID,
        INVALID,          INVALID,          INVALID,      INVALID,
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_dc[8] = {
    // clang-format off
/*      FADDd             FMULd             FCOMd             FCOMPd*/
        &load_fadd_op,    &load_fmul_op,    &load_float_op,   &load_float_op,
/*      FSUBd             FSUBRd            FDIVd             FDIVRd*/
        &load_float_op,   &load_float_op,   &fdiv_mem_op,     &fdiv_mem_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_dc_mod3[8] = {
    // clang-format off
/*      opFADDr           opFMULr*/
        &fadd_op,         &fmul_op,         INVALID,          INVALID,
/*      opFSUBRr          opFSUBr           opFDIVRr          opFDIVr*/
        &float_op,        &float_op,        &fdiv_op,         &fdiv_op
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_dd[8] = {
    // clang-format off
/*      FLDd                                    FSTd                 FSTPd*/
        &load_float_op,      INVALID,           &fstore_op,          &fstore_op,
/*      FRSTOR                                  FSAVE                FSTSW*/
        &complex_float_l_op, INVALID,           &complex_float_l_op, &complex_float_l_op
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_dd_mod3[8] = {
    // clang-format off
/*      FFFREE                            FST                FSTP*/
        &float_op,       INVALID,         &float_op,         &float_op,
/*      FUCOM            FUCOMP*/
        &float_op,       &float_op,       INVALID,           INVALID
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_de[8] = {
    // clang-format off
/*      FIADDw            FIMULw            FICOMw            FICOMPw*/
        &load_fiadd_op,   &load_fiadd_op,   &load_fiadd_op,   &load_fiadd_op,
/*      FISUBw            FISUBRw           FIDIVw            FIDIVRw*/
        &load_fiadd_op,   &load_fiadd_op,   &load_fiadd_op,   &load_fiadd_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_de_mod3[8] = {
    // clang-format off
/*      FADDP            FMULP                          FCOMPP*/
        &fadd_op,        &fmul_op,        INVALID,      &float_op,
/*      FSUBP            FSUBRP           FDIVP         FDIVRP*/
        &float_op,       &float_op,       &fdiv_op,     &fdiv_op,
    // clang-format on
};

static const macro_op_t *opcode_timings_p6_df[8] = {
    // clang-format off
/*      FILDiw                              FISTiw               FISTPiw*/
        &load_float_op,   INVALID,          &fstore_op,          &fstore_op,
/*                        FILDiq            FBSTP                FISTPiq*/
        INVALID,          &load_float_op,   &complex_float_l_op, &fstore_op,
    // clang-format on
};
static const macro_op_t *opcode_timings_p6_df_mod3[8] = {
    // clang-format off
        INVALID,      INVALID,      INVALID,      INVALID,
/*      FSTSW AX*/
        &float_op,    INVALID,      INVALID,      INVALID
    // clang-format on
};

static uint8_t last_prefix;
static int     prefixes;
static int     saw_66;

/*
 * Timing model overview.
 *
 * Instructions are decoded 4-1-1 (one complex plus two simple decoders per
 * cycle). Each uop is scheduled on an execution port the moment its macro-op is
 * decoded: it starts when the decode cycle has been reached, the previous uop of
 * the same instruction has finished, and its source registers (and flags) are
 * ready. A port is occupied for the uop's throughput (one cycle, or the whole
 * latency for unpipelined units such as the dividers), not for its latency.
 * Retirement is in order, three uops a cycle.
 *
 * Numbers follow the Intel P6 Family optimization manual (AP-526 / 248966) and
 * Agner Fog's instruction_tables.pdf and microarchitecture.pdf:
 *   - IMUL latency 4, throughput 1; L1 load-to-use latency 3.
 *   - Partial register stall: reading a wider register after a narrower write
 *     stalls the front end until the write retires (5-7 cycles on a one-cycle
 *     producer). XOR/SUB reg,reg before the narrow write avoids it; MOVZX/MOVSX
 *     write the whole register and never stall.
 *   - Partial flags stall: INC/DEC leave CF alone; a later instruction that
 *     reads CF together with other flags (JBE/JA, SETBE, PUSHF, LAHF) stalls.
 *   - Store forwarding: a load forwards from a store only if it starts at the
 *     same address and is not wider; otherwise it waits for the store to retire.
 *   - Length-changing prefix: a 66h prefix on an instruction with a 16-bit
 *     immediate costs about six decode cycles.
 *   - Decode works on 16-byte fetch blocks.
 */
#define LOAD_LATENCY          3
#define PARTIAL_RETIRE 6
#define STORE_FWD_FAIL 10
#define LCP_STALL             6
#define UNPIPELINED_THRESHOLD 6 /*uops this slow occupy their unit for the whole latency*/

static int decode_timestamp;
static int last_complete_timestamp;
static int group_done; /*latest completion in the current decode group*/
static int group_ins;  /*instructions in the current decode group*/
static int fetch_block;
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct p6_unit_t {
    uint32_t uop_mask;
    int      first_available_cycle;
} p6_unit_t;

static int        nr_units;
static p6_unit_t *units;

/*Pentium Pro has no MMX*/
static p6_unit_t ppro_units[] = {
    { .uop_mask = (1 << UOP_ALU) | (1 << UOP_ALUP0) | (1 << UOP_FLOAT) }, /*Port 0*/
    { .uop_mask = (1 << UOP_ALU) | (1 << UOP_BRANCH) },                   /*Port 1*/
    { .uop_mask = (1 << UOP_LOAD) | (1 << UOP_FLOAD) },                   /*Port 2*/
    { .uop_mask = (1 << UOP_STORED) | (1 << UOP_FSTORED) },               /*Port 3*/
    { .uop_mask = (1 << UOP_STOREA) | (1 << UOP_FSTOREA) },               /*Port 4*/
};
#define NR_PPRO_UNITS (sizeof(ppro_units) / sizeof(p6_unit_t))

/*Pentium II/Celeron assigns the multiplier to port 0, the shifter to port 1, and shares the MMX ALU*/
static p6_unit_t p2_units[] = {
    { .uop_mask = (1 << UOP_ALU) | (1 << UOP_ALUP0) | (1 << UOP_FLOAT) | /*Port 0*/
          (1 << UOP_MMX) | (1 << UOP_MMX_MUL) },
    { .uop_mask = (1 << UOP_ALU) | (1 << UOP_BRANCH) | /*Port 1*/
          (1 << UOP_MMX) | (1 << UOP_MMX_SHIFT) },
    { .uop_mask = (1 << UOP_LOAD) | (1 << UOP_FLOAD) | (1 << UOP_MLOAD) },       /*Port 2*/
    { .uop_mask = (1 << UOP_STORED) | (1 << UOP_FSTORED) | (1 << UOP_MSTORED) }, /*Port 3*/
    { .uop_mask = (1 << UOP_STOREA) | (1 << UOP_FSTOREA) | (1 << UOP_MSTOREA) }, /*Port 4*/
};
#define NR_P2_UNITS (sizeof(p2_units) / sizeof(p6_unit_t))

static int
uop_latency(const p6_uop_t *uop)
{
    if (uop->type == UOP_LOAD && uop->latency < LOAD_LATENCY)
        return LOAD_LATENCY;
    return uop->latency;
}

/*Runs one uop no earlier than ready_time; returns its completion time*/
static int
uop_run(const p6_uop_t *uop, int ready_time)
{
    p6_unit_t *best_unit        = NULL;
    int        best_start_cycle = 99999;
    int        latency          = uop_latency(uop);

    /*UOP_FXCH does not require execution*/
    if (uop->type == UOP_FXCH)
        return ready_time;

    for (int c = 0; c < nr_units; c++) {
        if (units[c].uop_mask & (1 << uop->type)) {
            if (units[c].first_available_cycle < best_start_cycle) {
                best_unit        = &units[c];
                best_start_cycle = units[c].first_available_cycle;
            }
        }
    }
    if (!best_unit)
        fatal("uop_run: can not find execution unit\n");

    if (best_start_cycle < ready_time)
        best_start_cycle = ready_time;
    best_unit->first_available_cycle = best_start_cycle + ((latency >= UNPIPELINED_THRESHOLD) ? latency : 1);

    return best_start_cycle + latency;
}

#define NR_OPSEQS 3
/*Timestamps of when the last three op sequences completed. Technically this is incorrect,
as the actual size of the opseq buffer is 20 bytes and not 18, but I'm restricted to multiples of 6*/
static int opseq_completion_timestamp[NR_OPSEQS];
static int next_opseq = 0;

#define NR_REGS 8
/*Per integer register: when the value is ready, the width of the last partial write
  (0 = whole register), when that write retires, and whether a zeroing idiom made the
  upper bits known*/
static int     reg_ready[NR_REGS];
static int     reg_partial_bits[NR_REGS];
static int     reg_partial_retire[NR_REGS];
static uint8_t reg_zero_upper;
static int     reg_write_seq[NR_REGS];
/*When the flags are ready, and when a partial-flags writer retires*/
static int flags_ready;
static int flags_partial_retire;
/*Timestamp of when last operation on an FPU register completed*/
static int fpu_st_timestamp[8];

/*Recent integer stores in this block, for store forwarding*/
#define NR_STORES 8
typedef struct store_t {
    int     valid;
    uint8_t modrm_key; /*mod/rm and SIB of the address*/
    uint8_t sib;
    int     disp;
    int     size;
    int     data_ready;
    int     retire;
    int     seq_base, seq_index;
} store_t;
static store_t stores[NR_STORES];
static int     next_store;

void
decode_flush_p6(void)
{
    /*Decoded opseq can not be submitted if there are no free spaces in the
      opseq buffer*/
    if (decode_timestamp < opseq_completion_timestamp[next_opseq])
        decode_timestamp = opseq_completion_timestamp[next_opseq];

    /*Calculate opseq completion time. Since opseqs complete in order, it
      must be after the last completion.*/
    if (group_done <= last_complete_timestamp)
        last_complete_timestamp = last_complete_timestamp + 1;
    else
        last_complete_timestamp = group_done;

    /*Advance to next opseq in buffer*/
    opseq_completion_timestamp[next_opseq] = last_complete_timestamp;
    next_opseq++;
    if (next_opseq == NR_OPSEQS)
        next_opseq = 0;

    decode_timestamp++;
    group_done = 0;
    group_ins  = 0;
}

/*The P6 decoders can decode, per clock :
  - 1 to 3 'simple' instructions, each up to 1 uOP and 7 bytes long
  - 1 'complex' instruction, up to 4 uOPs or 3 per cycle for instructions longer than 4 uOPs
*/

/*The instruction is only of interest here if it's longer than 7 bytes, as that's the
  limit on P6 simple decoding*/
static int
codegen_timing_instr_length(uint64_t deps, uint32_t fetchdat, int op_32)
{
    int len = prefixes + 1; /*Opcode*/
    if (deps & MODRM) {
        len++; /*ModR/M*/
        if (deps & HAS_IMM8)
            len++;
        if (deps & HAS_IMM1632)
            len += (op_32 & 0x100) ? 4 : 2;

        if (op_32 & 0x200) {
            if ((fetchdat & 7) == 4 && (fetchdat & 0xc0) != 0xc0) {
                /* Has SIB*/
                len++;
                if ((fetchdat & 0xc0) == 0x40)
                    len++;
                else if ((fetchdat & 0xc0) == 0x80)
                    len += 4;
                else if ((fetchdat & 0x700) == 0x500)
                    len += 4;
            } else {
                if ((fetchdat & 0xc0) == 0x40)
                    len++;
                else if ((fetchdat & 0xc0) == 0x80)
                    len += 4;
                else if ((fetchdat & 0xc7) == 0x05)
                    len += 4;
            }
        } else {
            if ((fetchdat & 0xc0) == 0x40)
                len++;
            else if ((fetchdat & 0xc0) == 0x80)
                len += 2;
            else if ((fetchdat & 0xc7) == 0x06)
                len += 2;
        }
    }

    return len;
}

/*Opcodes whose operand is a byte (the low opcode bit is not reliable: INC/DEC, XCHG EAX,reg...)*/
static int
opcode_is_byte(uint8_t prefix, uint8_t opcode)
{
    if (prefix == 0x0f)
        return (opcode >= 0x90 && opcode <= 0x9f) || opcode == 0xb0 || opcode == 0xc0;
    if (prefix)
        return 0;
    if (opcode < 0x40)
        return (opcode & 7) < 6 && !(opcode & 1);
    switch (opcode) {
        case 0x80:
        case 0x82:
        case 0x84:
        case 0x86:
        case 0x88:
        case 0x8a:
        case 0xa0:
        case 0xa2:
        case 0xa4:
        case 0xa6:
        case 0xa8:
        case 0xaa:
        case 0xac:
        case 0xae:
        case 0xc0:
        case 0xc6:
        case 0xd0:
        case 0xd2:
        case 0xf6:
        case 0xfe:
        case 0x6c:
        case 0x6e:
        case 0xe4:
        case 0xe6:
        case 0xec:
        case 0xee:
            return 1;
        default:
            return opcode >= 0xb0 && opcode <= 0xb7;
    }
}

enum { FW_NONE, FW_ALL, FW_NOCF };
enum { FR_NONE, FR_CF, FR_MIXED, FR_OTHER };

static int
cc_reads(uint8_t cc)
{
    switch (cc & 15) {
        case 2:
        case 3:
            return FR_CF;
        case 6:
        case 7:
            return FR_MIXED; /*CF and ZF*/
        default:
            return FR_OTHER;
    }
}

/*Which flags an instruction writes and reads, by opcode*/
static void
flag_effects(uint8_t prefix, uint8_t opcode, uint32_t fetchdat, int *write, int *read)
{
    const int reg = (fetchdat >> 3) & 7;

    *write = FW_NONE;
    *read  = FR_NONE;
    if (prefix == 0x0f) {
        if (opcode >= 0x80 && opcode <= 0x8f)
            *read = cc_reads(opcode);
        else if ((opcode >= 0x90 && opcode <= 0x9f) || (opcode >= 0x40 && opcode <= 0x4f))
            *read = cc_reads(opcode);
        else if (opcode == 0xaf || opcode == 0xa3 || opcode == 0xab || opcode == 0xb3 || opcode == 0xbb || opcode == 0xba
                 || opcode == 0xbc || opcode == 0xbd || opcode == 0xa4 || opcode == 0xa5 || opcode == 0xac || opcode == 0xad
                 || opcode == 0xb0 || opcode == 0xb1 || opcode == 0xc0 || opcode == 0xc1)
            *write = FW_ALL;
        return;
    }
    if (prefix)
        return;
    if (opcode < 0x40) {
        if ((opcode & 7) < 6) {
            *write = FW_ALL;
            if ((opcode & 0x18) == 0x10)
                *read = FR_CF; /*ADC, SBB*/
        }
        return;
    }
    if (opcode <= 0x4f) {
        *write = FW_NOCF; /*INC, DEC*/
        return;
    }
    if (opcode >= 0x70 && opcode <= 0x7f) {
        *read = cc_reads(opcode);
        return;
    }
    switch (opcode) {
        case 0x69:
        case 0x6b:
        case 0x84:
        case 0x85:
        case 0xa6:
        case 0xa7:
        case 0xa8:
        case 0xa9:
        case 0xae:
        case 0xaf:
        case 0x9d:
        case 0x9e:
            *write = FW_ALL;
            break;
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
            *write = FW_ALL;
            if (reg == 2 || reg == 3)
                *read = FR_CF;
            break;
        case 0xc0:
        case 0xc1:
        case 0xd0:
        case 0xd1:
        case 0xd2:
        case 0xd3:
            *write = FW_ALL;
            if (reg == 2 || reg == 3)
                *read = FR_CF;
            break;
        case 0xf6:
        case 0xf7:
            if (reg != 2)
                *write = FW_ALL;
            break;
        case 0xfe:
        case 0xff:
            if (reg < 2)
                *write = FW_NOCF;
            break;
        case 0x9c:
        case 0x9f:
            *read = FR_MIXED; /*PUSHF, LAHF*/
            break;
        default:
            break;
    }
}

/*Decodes a 32-bit-addressing memory operand into base, index, scale and displacement.
  Only the bytes in fetchdat are seen, so a 32-bit displacement is truncated.*/
static int
parse_address(uint32_t fetchdat, int op_32, int *base, int *index, int *scale, int *disp)
{
    const int modrm = fetchdat & 0xff;
    const int mod   = modrm >> 6;
    int       rm    = modrm & 7;
    int       pos   = 1;

    if (!(op_32 & 0x200) || mod == 3)
        return 0;
    *index = -1;
    *scale = 0;
    *base  = rm;
    if (rm == 4) {
        const int sib = (fetchdat >> 8) & 0xff;

        pos    = 2;
        *base  = sib & 7;
        *scale = sib >> 6;
        if (((sib >> 3) & 7) != 4)
            *index = (sib >> 3) & 7;
    }
    *disp = 0;
    if (mod == 1) {
        *disp = (int8_t) ((fetchdat >> (8 * pos)) & 0xff);
    } else if (mod == 2 || (mod == 0 && *base == 5)) {
        *disp = (int32_t) ((fetchdat >> (8 * pos)) << (8 * pos)) >> (8 * pos);
        if (mod == 0)
            *base = -1;
    }
    return 1;
}

static int
regs_ready(uint32_t mask)
{
    int t = 0;

    for (int c = 0; c < NR_REGS; c++)
        if ((mask & (1 << c)) && reg_ready[c] > t)
            t = reg_ready[c];
    return t;
}

static int
partial_stall(uint32_t mask, int read_bits)
{
    int t = 0;

    for (int c = 0; c < NR_REGS; c++) {
        if ((mask & (1 << c)) && reg_partial_bits[c] && read_bits > reg_partial_bits[c] && !(reg_zero_upper & (1 << c))
            && reg_partial_retire[c] > t)
            t = reg_partial_retire[c];
    }
    return t;
}

/*Every instruction of the block in decode order, so the block can be replayed*/
#define MAX_REPLAY 1024
typedef struct replay_t {
    const macro_op_t *ins;
    uint64_t          deps;
    uint32_t          fetchdat, op_pc;
    int               op_32, prefixes, saw_66;
    uint8_t           opcode, last_prefix;
} replay_t;
static replay_t replay[MAX_REPLAY];
static int      nr_replay;

static void
decode_instruction(const macro_op_t *ins, uint64_t deps, uint32_t fetchdat, int op_32, uint8_t opcode, uint32_t op_pc)
{
    const int     bit8         = !(opcode & 1);
    const int     is_byte      = opcode_is_byte(last_prefix, opcode);
    const int     opbits       = (op_32 & 0x100) ? 32 : 16;
    const int     mod3         = (fetchdat & 0xc0) == 0xc0;
    int           rd_bits      = is_byte ? 8 : opbits;
    int           wr_bits      = rd_bits;
    int           c;
    int           d            = 0; /*Complex decoder uOPs*/
    decode_type_t decode_type  = ins->decode_type;
    int           instr_length = codegen_timing_instr_length(deps, fetchdat, op_32);
    uint32_t      addr_m       = get_addr_regmask(deps, fetchdat, op_32) & 0xff;
    uint32_t      src_m        = get_srcdep_mask(deps, fetchdat, bit8, op_32) & 0xff;
    uint32_t      dst_m        = get_dstdep_mask(deps, fetchdat, bit8) & 0xff;
    uint32_t      data_m;
    int           idiom = 0, movx = 0;
    int           fpu_floor = 0, flags_floor = 0, load_floor = 0;
    int           fw, fr;
    int           stall;
    int           base, index, scale, disp, have_addr;
    int           ins_done = 0, prev = 0;
    int           mem_load = 0, mem_store = 0, store_data = 0;
    int           lin;

    /*MOVZX/MOVSX read a byte or word but write the whole register*/
    if (last_prefix == 0x0f && (opcode == 0xb6 || opcode == 0xb7 || opcode == 0xbe || opcode == 0xbf)) {
        movx    = 1;
        rd_bits = (opcode & 1) ? 16 : 8;
        wr_bits = 32;
        dst_m   = 1 << ((fetchdat >> 3) & 7);
        src_m &= ~dst_m;
        if (mod3)
            src_m = (src_m & addr_m) | (1 << ((fetchdat & 7) & ((opcode & 1) ? 7 : 3)));
    }
    /*The shared 0F dependency tables carry no register information for these*/
    if (last_prefix == 0x0f && !movx) {
        const uint32_t reg_m = 1u << ((fetchdat >> 3) & 7);
        const uint32_t rm_m  = mod3 ? 1u << (fetchdat & 7) : 0;

        if (opcode == 0xaf || (opcode >= 0x40 && opcode <= 0x4f) || opcode == 0xbc || opcode == 0xbd) {
            src_m = addr_m | rm_m | (opcode == 0xaf || (opcode >= 0x40 && opcode <= 0x4f) ? reg_m : 0);
            dst_m = reg_m;
        } else if (opcode >= 0x90 && opcode <= 0x9f) {
            src_m = addr_m;
            dst_m = mod3 ? 1u << ((fetchdat & 7) & 3) : 0;
        } else if (opcode >= 0xc8 && opcode <= 0xcf) {
            src_m = dst_m = 1u << (opcode & 7);
        }
    }
    if (deps & IMPL_ESP)
        dst_m &= ~(1u << REG_ESP); /*stack engine: ESP updates are not on the critical path*/

    /*XOR/SUB reg,reg breaks the dependency and leaves no partial-register state*/
    if (!last_prefix && !movx && (opcode == 0x31 || opcode == 0x33 || opcode == 0x29 || opcode == 0x2b) && mod3
        && ((fetchdat >> 3) & 7) == (fetchdat & 7) && opbits == 32) {
        idiom = 1;
        src_m = 0;
    }
    data_m = src_m & ~addr_m;
    if (!mod3)
        addr_m &= 0xff;

    flag_effects(last_prefix, opcode, fetchdat, &fw, &fr);

    /*Simple decoders are limited to 7 bytes & 1 uOP*/
    if (decode_type == DECODE_SIMPLE && (instr_length > 7 || ins->nr_uops > 1))
        decode_type = DECODE_COMPLEX;
    /*The 4-1-1 arrangement implies that a complex ins. can't be decoded after another*/
    if (decode_type == DECODE_COMPLEX && group_ins)
        decode_flush_p6();

    /*Front-end stalls: partial registers, partial flags, length-changing prefix*/
    stall = partial_stall(data_m, rd_bits);
    if (partial_stall(addr_m, (op_32 & 0x200) ? 32 : 16) > stall)
        stall = partial_stall(addr_m, (op_32 & 0x200) ? 32 : 16);
    if (fr == FR_MIXED && flags_partial_retire > stall)
        stall = flags_partial_retire;
    if (stall > decode_timestamp) {
        if (group_ins)
            decode_flush_p6();
        if (stall > decode_timestamp)
            decode_timestamp = stall;
    }
    if (saw_66 && (deps & HAS_IMM1632)) {
        if (group_ins)
            decode_flush_p6();
        decode_timestamp += LCP_STALL;
    }

    /*Decode works on 16-byte fetch blocks: a group never spans two, and an
      instruction that straddles a boundary needs an extra cycle*/
    lin = (int) (cs + op_pc) - 1 - prefixes;
    if ((lin >> 4) != fetch_block) {
        if (group_ins)
            decode_flush_p6();
        fetch_block = lin >> 4;
    }
    if (((lin & 15) + instr_length) > 16) {
        if (group_ins)
            decode_flush_p6();
        decode_timestamp++;
        fetch_block = (lin + instr_length - 1) >> 4;
    }

    /*Register, flag and FPU stack readiness*/
    if (fr != FR_NONE)
        flags_floor = flags_ready;
    if ((deps & FPU_READ_ST0) && fpu_st_timestamp[0] > fpu_floor)
        fpu_floor = fpu_st_timestamp[0];
    if ((deps & FPU_READ_ST1) && fpu_st_timestamp[1] > fpu_floor)
        fpu_floor = fpu_st_timestamp[1];
    if (deps & FPU_READ_STREG) {
        int reg = fetchdat & 7;

        if (fpu_st_timestamp[reg] > fpu_floor)
            fpu_floor = fpu_st_timestamp[reg];
    }

    /*Memory operand: store forwarding*/
    for (c = 0; c < ins->nr_uops; c++) {
        if (ins->uop[c].type == UOP_LOAD)
            mem_load = 1;
        if (ins->uop[c].type == UOP_STORED)
            mem_store = 1;
    }
    have_addr = (mem_load || mem_store) && !mod3 && parse_address(fetchdat, op_32, &base, &index, &scale, &disp);
    if (have_addr && mem_load) {
        for (c = 1; c <= NR_STORES; c++) {
            const store_t *s = &stores[(next_store - c + NR_STORES) % NR_STORES];

            if (!s->valid || s->modrm_key != (uint8_t) ((base < 0 ? 0xff : base) | (scale << 5)) || s->sib != (uint8_t) index)
                continue;
            if ((base >= 0 && reg_write_seq[base] != s->seq_base) || (index >= 0 && reg_write_seq[index] != s->seq_index))
                continue;
            if (disp >= s->disp + s->size || disp + (rd_bits >> 3) <= s->disp)
                continue; /*no overlap*/
            if (disp == s->disp && (rd_bits >> 3) <= s->size)
                load_floor = s->data_ready;
            else
                load_floor = s->retire + STORE_FWD_FAIL;
            break;
        }
    }

    /*Schedule each uop*/
    for (c = 0; c < ins->nr_uops; c++) {
        const p6_uop_t *uop   = &ins->uop[c];
        int             ready = decode_timestamp;
        int             done;

        switch (uop->type) {
            case UOP_LOAD:
            case UOP_FLOAD:
            case UOP_MLOAD:
                ready = MAX(ready, regs_ready(addr_m));
                if (uop->type == UOP_LOAD)
                    ready = MAX(ready, load_floor);
                break;
            case UOP_STOREA:
            case UOP_FSTOREA:
            case UOP_MSTOREA:
                ready = MAX(ready, regs_ready(addr_m));
                break;
            default:
                ready = MAX(ready, regs_ready(data_m));
                ready = MAX(ready, MAX(fpu_floor, flags_floor));
                if (c > 0)
                    ready = MAX(ready, prev);
                break;
        }
        done = uop_run(uop, ready);
        if (uop->type == UOP_STORED)
            store_data = done;
        if (uop->type != UOP_STOREA && uop->type != UOP_FSTOREA && uop->type != UOP_MSTOREA)
            prev = done;
        if (done > ins_done)
            ins_done = done;

        /*Instructions longer than 4 uops use 3 uops a cycle on the decoders*/
        if (ins->nr_uops > 4 && ++d == 3) {
            d = 0;
            group_done = MAX(group_done, ins_done);
            decode_flush_p6();
        }
    }
    group_done = MAX(group_done, ins_done);
    group_ins++;
    if (ins->nr_uops > 4 || group_ins >= 3)
        decode_flush_p6();

    /*Publish results*/
    if (mem_store && have_addr) {
        store_t *s = &stores[next_store];

        s->valid      = 1;
        s->modrm_key  = (uint8_t) ((base < 0 ? 0xff : base) | (scale << 5));
        s->sib        = (uint8_t) index;
        s->disp       = disp;
        s->size       = rd_bits >> 3;
        s->data_ready = store_data ? store_data : ins_done;
        s->retire     = s->data_ready + 3;
        s->seq_base   = base >= 0 ? reg_write_seq[base] : 0;
        s->seq_index  = index >= 0 ? reg_write_seq[index] : 0;
        next_store    = (next_store + 1) % NR_STORES;
    }
    for (c = 0; c < NR_REGS; c++) {
        if (!(dst_m & (1 << c)))
            continue;
        reg_write_seq[c]++;
        reg_ready[c] = prev ? prev : ins_done;
        if (idiom) {
            reg_zero_upper |= 1 << c;
            reg_partial_bits[c] = 0;
        } else if (wr_bits == 32) {
            reg_zero_upper &= ~(1 << c);
            reg_partial_bits[c] = 0;
        } else if (!(reg_zero_upper & (1 << c))) {
            reg_partial_bits[c]   = wr_bits;
            reg_partial_retire[c] = reg_ready[c] + PARTIAL_RETIRE;
        }
    }
    if (fw != FW_NONE) {
        flags_ready          = prev ? prev : ins_done;
        flags_partial_retire = (fw == FW_NOCF) ? flags_ready + PARTIAL_RETIRE : 0;
    }

    if (deps & FPU_POP) {
        for (c = 0; c < 7; c++)
            fpu_st_timestamp[c] = fpu_st_timestamp[c + 1];
        fpu_st_timestamp[7] = 0;
    }
    if (deps & FPU_POP2) {
        for (c = 0; c < 6; c++)
            fpu_st_timestamp[c] = fpu_st_timestamp[c + 2];
        fpu_st_timestamp[6] = fpu_st_timestamp[7] = 0;
    }
    if (deps & FPU_PUSH) {
        for (c = 0; c < 7; c++)
            fpu_st_timestamp[c + 1] = fpu_st_timestamp[c];
        fpu_st_timestamp[0] = 0;
    }
    if (deps & FPU_WRITE_ST0)
        fpu_st_timestamp[0] = ins_done;
    if (deps & FPU_WRITE_ST1)
        fpu_st_timestamp[1] = ins_done;
    if (deps & FPU_WRITE_STREG) {
        int reg = fetchdat & 7;
        if (deps & FPU_POP)
            reg--;
        if (reg >= 0 && !(reg == 0 && (deps & FPU_WRITE_ST0)) && !(reg == 1 && (deps & FPU_WRITE_ST1)))
            fpu_st_timestamp[reg] = ins_done;
    }
}

/*
 * Blocks overlap on an out-of-order core: the next one starts while the tail of
 * this one is still executing. Where control can leave the block, the cost of
 * everything decoded so far is taken as what it costs when run again straight
 * away from the state it left behind: its throughput in a loop, or its
 * loop-carried dependency chain, not its latency in isolation.
 */
typedef struct p6_state_t {
    int      decode_timestamp, last_complete_timestamp, group_done, group_ins, fetch_block, next_opseq, next_store;
    int      opseq_completion_timestamp[NR_OPSEQS];
    int      reg_ready[NR_REGS], reg_partial_bits[NR_REGS], reg_partial_retire[NR_REGS], reg_write_seq[NR_REGS];
    uint8_t  reg_zero_upper;
    int      flags_ready, flags_partial_retire;
    int      fpu_st_timestamp[8];
    int      unit_available[8];
    store_t  stores[NR_STORES];
} p6_state_t;

static void
state_save(p6_state_t *t)
{
    t->decode_timestamp        = decode_timestamp;
    t->last_complete_timestamp = last_complete_timestamp;
    t->group_done              = group_done;
    t->group_ins               = group_ins;
    t->fetch_block             = fetch_block;
    t->next_opseq              = next_opseq;
    t->next_store              = next_store;
    memcpy(t->opseq_completion_timestamp, opseq_completion_timestamp, sizeof(opseq_completion_timestamp));
    memcpy(t->reg_ready, reg_ready, sizeof(reg_ready));
    memcpy(t->reg_partial_bits, reg_partial_bits, sizeof(reg_partial_bits));
    memcpy(t->reg_partial_retire, reg_partial_retire, sizeof(reg_partial_retire));
    memcpy(t->reg_write_seq, reg_write_seq, sizeof(reg_write_seq));
    t->reg_zero_upper        = reg_zero_upper;
    t->flags_ready           = flags_ready;
    t->flags_partial_retire  = flags_partial_retire;
    memcpy(t->fpu_st_timestamp, fpu_st_timestamp, sizeof(fpu_st_timestamp));
    for (int c = 0; c < nr_units; c++)
        t->unit_available[c] = units[c].first_available_cycle;
    memcpy(t->stores, stores, sizeof(stores));
}

static void
state_restore(const p6_state_t *t)
{
    decode_timestamp        = t->decode_timestamp;
    last_complete_timestamp = t->last_complete_timestamp;
    group_done              = t->group_done;
    group_ins               = t->group_ins;
    fetch_block             = t->fetch_block;
    next_opseq              = t->next_opseq;
    next_store              = t->next_store;
    memcpy(opseq_completion_timestamp, t->opseq_completion_timestamp, sizeof(opseq_completion_timestamp));
    memcpy(reg_ready, t->reg_ready, sizeof(reg_ready));
    memcpy(reg_partial_bits, t->reg_partial_bits, sizeof(reg_partial_bits));
    memcpy(reg_partial_retire, t->reg_partial_retire, sizeof(reg_partial_retire));
    memcpy(reg_write_seq, t->reg_write_seq, sizeof(reg_write_seq));
    reg_zero_upper       = t->reg_zero_upper;
    flags_ready          = t->flags_ready;
    flags_partial_retire = t->flags_partial_retire;
    memcpy(fpu_st_timestamp, t->fpu_st_timestamp, sizeof(fpu_st_timestamp));
    for (int c = 0; c < nr_units; c++)
        units[c].first_available_cycle = t->unit_available[c];
    memcpy(stores, t->stores, sizeof(stores));
}

static int charged;        /*cycles already handed to the code generator for this block*/
static int finalized_upto; /*instructions covered by the last steady-state estimate*/

/*Charges the steady-state cost of the block so far; leaves the timing state as it was*/
static void
charge_steady_state(void)
{
    p6_state_t saved;
    int        first, steady;

    if (!nr_replay || nr_replay >= MAX_REPLAY || finalized_upto == nr_replay)
        return;
    finalized_upto = nr_replay;
    state_save(&saved);
    if (group_ins)
        decode_flush_p6();
    first = last_complete_timestamp;
    {
        const uint8_t saved_prefix = last_prefix;
        const int     saved_pref   = prefixes;
        const int     saved_66     = saw_66;

        for (int c = 0; c < nr_replay; c++) {
            last_prefix = replay[c].last_prefix;
            prefixes    = replay[c].prefixes;
            saw_66      = replay[c].saw_66;
            decode_instruction(replay[c].ins, replay[c].deps, replay[c].fetchdat, replay[c].op_32, replay[c].opcode, replay[c].op_pc);
        }
        if (group_ins)
            decode_flush_p6();
        last_prefix = saved_prefix;
        prefixes    = saved_pref;
        saw_66      = saved_66;
    }
    steady = last_complete_timestamp - first;
    state_restore(&saved);
    codegen_block_cycles += steady - charged;
    charged = steady;
}

/*Opcodes after which the code generator may leave the block*/
static int
leaves_block(uint8_t prefix, uint8_t opcode, uint32_t fetchdat)
{
    if (prefix == 0x0f)
        return opcode >= 0x80 && opcode <= 0x8f;
    if (prefix == 0xf2 || prefix == 0xf3)
        return 0;
    return (opcode & 0xf0) == 0x70 || (opcode & 0xfc) == 0xe0 || opcode == 0xc2 || (opcode & 0xfe) == 0xca
        || (opcode & 0xfc) == 0xcc || (opcode & 0xfc) == 0xe8 || (opcode == 0xff && (fetchdat & 0x38) >= 0x10 && (fetchdat & 0x38) < 0x30);
}

void
codegen_timing_p6_block_start(void)
{
    int c;

    for (c = 0; c < nr_units; c++)
        units[c].first_available_cycle = 0;

    decode_timestamp        = 0;
    last_complete_timestamp = 0;
    group_done              = 0;
    group_ins               = 0;
    fetch_block             = -1;
    nr_replay               = 0;
    charged                 = 0;
    finalized_upto          = 0;

    for (c = 0; c < NR_OPSEQS; c++)
        opseq_completion_timestamp[c] = 0;
    next_opseq = 0;

    for (c = 0; c < NR_REGS; c++) {
        reg_ready[c]          = 0;
        reg_partial_bits[c]   = 0;
        reg_partial_retire[c] = 0;
        reg_write_seq[c]      = 0;
    }
    reg_zero_upper       = 0;
    flags_ready          = 0;
    flags_partial_retire = 0;
    for (c = 0; c < 8; c++)
        fpu_st_timestamp[c] = 0;
    memset(stores, 0, sizeof(stores));
    next_store = 0;
}

void
codegen_timing_p6_start(void)
{
    if (cpu_s->cpu_type == CPU_PENTIUMPRO) {
        units    = ppro_units;
        nr_units = NR_PPRO_UNITS;
    } else {
        units    = p2_units;
        nr_units = NR_P2_UNITS;
    }
    last_prefix = 0;
    prefixes    = 0;
    saw_66      = 0;
}

void
codegen_timing_p6_prefix(uint8_t prefix, UNUSED(uint32_t fetchdat))
{
    if (prefix != 0x0f)
        decode_timestamp++;
    if (prefix == 0x66)
        saw_66 = 1;

    last_prefix = prefix;
    prefixes++;
}

void
codegen_timing_p6_opcode(uint8_t opcode, UNUSED(uint32_t fetchdat), int op_32, UNUSED(uint32_t op_pc))
{
    const macro_op_t **ins_table;
    const macro_op_t  *ins_table_one;
    const uint64_t    *deps;
    uint64_t           deps_one;
    int                mod3                        = ((fetchdat & 0xc0) == 0xc0);
    int                old_last_complete_timestamp = last_complete_timestamp;

    const uint8_t raw_opcode = opcode;
    int bit8_unused = 0;
    (void) bit8_unused;
    switch (last_prefix) {
        case 0x0f:
            ins_table = mod3 ? opcode_timings_p6_0f_mod3 : opcode_timings_p6_0f;
            deps      = mod3 ? opcode_deps_0f_mod3 : opcode_deps_0f;
            break;

        case 0xd8:
            ins_table = mod3 ? opcode_timings_p6_d8_mod3 : opcode_timings_p6_d8;
            deps      = mod3 ? opcode_deps_d8_mod3 : opcode_deps_d8;
            opcode    = (opcode >> 3) & 7;
            break;
        case 0xd9:
            ins_table = mod3 ? opcode_timings_p6_d9_mod3 : opcode_timings_p6_d9;
            deps      = mod3 ? opcode_deps_d9_mod3 : opcode_deps_d9;
            opcode    = mod3 ? opcode & 0x3f : (opcode >> 3) & 7;
            break;
        case 0xda:
            ins_table = mod3 ? opcode_timings_p6_da_mod3 : opcode_timings_p6_da;
            deps      = mod3 ? opcode_deps_da_mod3 : opcode_deps_da;
            opcode    = (opcode >> 3) & 7;
            break;
        case 0xdb:
            ins_table = mod3 ? opcode_timings_p6_db_mod3 : opcode_timings_p6_db;
            deps      = mod3 ? opcode_deps_db_mod3 : opcode_deps_db;
            opcode    = mod3 ? opcode & 0x3f : (opcode >> 3) & 7;
            break;
        case 0xdc:
            ins_table = mod3 ? opcode_timings_p6_dc_mod3 : opcode_timings_p6_dc;
            deps      = mod3 ? opcode_deps_dc_mod3 : opcode_deps_dc;
            opcode    = (opcode >> 3) & 7;
            break;
        case 0xdd:
            ins_table = mod3 ? opcode_timings_p6_dd_mod3 : opcode_timings_p6_dd;
            deps      = mod3 ? opcode_deps_dd_mod3 : opcode_deps_dd;
            opcode    = (opcode >> 3) & 7;
            break;
        case 0xde:
            ins_table = mod3 ? opcode_timings_p6_de_mod3 : opcode_timings_p6_de;
            deps      = mod3 ? opcode_deps_de_mod3 : opcode_deps_de;
            opcode    = (opcode >> 3) & 7;
            break;
        case 0xdf:
            ins_table = mod3 ? opcode_timings_p6_df_mod3 : opcode_timings_p6_df;
            deps      = mod3 ? opcode_deps_df_mod3 : opcode_deps_df;
            opcode    = (opcode >> 3) & 7;
            break;

        default:
            switch (opcode) {
                case 0x80:
                case 0x82:
                    ins_table = mod3 ? opcode_timings_p6_80_mod3 : opcode_timings_p6_80;
                    deps      = mod3 ? opcode_deps_8x_mod3 : opcode_deps_8x;
                    opcode    = (fetchdat >> 3) & 7;
                    break;
                case 0x81:
                case 0x83:
                    ins_table = mod3 ? opcode_timings_p6_8x_mod3 : opcode_timings_p6_8x;
                    deps      = mod3 ? opcode_deps_8x_mod3 : opcode_deps_8x;
                    opcode    = (fetchdat >> 3) & 7;
                    break;

                case 0xc0:
                case 0xd0:
                case 0xd2:
                    ins_table = mod3 ? opcode_timings_p6_shift_b_mod3 : opcode_timings_p6_shift_b;
                    deps      = mod3 ? opcode_deps_shift_mod3 : opcode_deps_shift;
                    opcode    = (fetchdat >> 3) & 7;
                    break;

                case 0xc1:
                case 0xd1:
                case 0xd3:
                    ins_table = mod3 ? opcode_timings_p6_shift_mod3 : opcode_timings_p6_shift;
                    deps      = mod3 ? opcode_deps_shift_mod3 : opcode_deps_shift;
                    opcode    = (fetchdat >> 3) & 7;
                    break;

                case 0xf6:
                    ins_table = mod3 ? opcode_timings_p6_f6_mod3 : opcode_timings_p6_f6;
                    deps      = mod3 ? opcode_deps_f6_mod3 : opcode_deps_f6;
                    opcode    = (fetchdat >> 3) & 7;
                    break;
                case 0xf7:
                    ins_table = mod3 ? opcode_timings_p6_f7_mod3 : opcode_timings_p6_f7;
                    deps      = mod3 ? opcode_deps_f7_mod3 : opcode_deps_f7;
                    opcode    = (fetchdat >> 3) & 7;
                    break;
                case 0xff:
                    ins_table = mod3 ? opcode_timings_p6_ff_mod3 : opcode_timings_p6_ff;
                    deps      = mod3 ? opcode_deps_ff_mod3 : opcode_deps_ff;
                    opcode    = (fetchdat >> 3) & 7;
                    break;

                default:
                    ins_table = mod3 ? opcode_timings_p6_mod3 : opcode_timings_p6;
                    deps      = mod3 ? opcode_deps_mod3 : opcode_deps;
                    break;
            }
    }

    if (!ins_table[opcode]) {
        ins_table_one = &complex_alu1_op;
        deps_one      = 0;
    } else {
        ins_table_one = ins_table[opcode];
        deps_one      = deps[opcode];
    }
    if (nr_replay < MAX_REPLAY)
        replay[nr_replay++] = (replay_t) { ins_table_one, deps_one, fetchdat, op_pc, op_32, prefixes, saw_66, raw_opcode, last_prefix };
    decode_instruction(ins_table_one, deps_one, fetchdat, op_32, raw_opcode, op_pc);
    codegen_block_cycles += (last_complete_timestamp - old_last_complete_timestamp);
    charged += (last_complete_timestamp - old_last_complete_timestamp);
    if (leaves_block(last_prefix, raw_opcode, fetchdat))
        charge_steady_state();
    last_prefix = 0;
}

void
codegen_timing_p6_block_end(void)
{
    charge_steady_state();
}

int
codegen_timing_p6_jump_cycles(void)
{
    if (group_ins)
        return 1;
    return 0;
}

codegen_timing_t codegen_timing_p6 = {
    codegen_timing_p6_start,
    codegen_timing_p6_prefix,
    codegen_timing_p6_opcode,
    codegen_timing_p6_block_start,
    codegen_timing_p6_block_end,
    codegen_timing_p6_jump_cycles
};

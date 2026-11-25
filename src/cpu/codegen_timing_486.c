#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "x86.h"
#include "x86_ops.h"
#include "x87_sf.h"
#include "x87.h"
#include "codegen.h"
#include "codegen_ops.h"
#include "codegen_timing_common.h"
#include "x87_timings.h"

#define CYCLES(c)         (int *) c
#define CYCLES2(c16, c32) (int *) ((-1 & ~0xffff) | c16 | (c32 << 8))

static int *opcode_timings_486[256] = {
    // clang-format off
/*00*/  &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(2),      CYCLES(3),      &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(2),      NULL,
/*10*/  &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(2),      CYCLES(3),      &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(2),      CYCLES(3),
/*20*/  &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(3),      &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(3),
/*30*/  &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(2),      &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(2),

/*40*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,
/*50*/  CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),
/*60*/  CYCLES(11),     CYCLES(9),      CYCLES(7),      CYCLES(9),      CYCLES(4),      CYCLES(4),      CYCLES(2),      CYCLES(2),      CYCLES(1),      CYCLES2(17,25), CYCLES(1),      CYCLES2(17,20), CYCLES(17),     CYCLES(17),     CYCLES(17),     CYCLES(17),
/*70*/  &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,

/*80*/  &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_rm,     &timing_rm,     CYCLES(5),      CYCLES(5),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(3),      CYCLES(1),      CYCLES(5),      CYCLES(6),
/*90*/  CYCLES(1),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(0),      CYCLES(4),      CYCLES(4),      CYCLES(5),      CYCLES(2),      CYCLES(3),
/*a0*/  CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(7),      CYCLES(7),      CYCLES(8),      CYCLES(8),      CYCLES(1),      CYCLES(1),      CYCLES(5),      CYCLES(5),      CYCLES(5),      CYCLES(5),      CYCLES(6),      CYCLES(6),
/*b0*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,

/*c0*/  CYCLES(4),      CYCLES(4),      CYCLES(5),      CYCLES(5),      CYCLES(6),      CYCLES(6),      CYCLES(1),      CYCLES(1),      CYCLES(14),     CYCLES(5),      CYCLES(0),      CYCLES(0),      &timing_int,    &timing_int,    CYCLES(3),      CYCLES(0),
/*d0*/  CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(15),     CYCLES(14),     CYCLES(2),      CYCLES(4),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*e0*/  CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(5),      CYCLES(14),     CYCLES(14),     CYCLES(16),     CYCLES(16),     CYCLES(3),      CYCLES(3),      CYCLES(17),     CYCLES(3),      CYCLES(14),     CYCLES(14),     CYCLES(14),     CYCLES(14),
/*f0*/  CYCLES(4),      CYCLES(0),      CYCLES(0),      CYCLES(0),      CYCLES(4),      CYCLES(2),      NULL,           NULL,           CYCLES(2),      CYCLES(2),      CYCLES(3),      CYCLES(2),      CYCLES(2),      CYCLES(2),      CYCLES(3),      NULL
    // clang-format on
};

static int *opcode_timings_486_mod3[256] = {
    // clang-format off
/*00*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(2),      CYCLES(3),      &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(2),      NULL,
/*10*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(2),      CYCLES(3),      &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(2),      CYCLES(3),
/*20*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(3),      &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(3),
/*30*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(2),      &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(4),      CYCLES(2),

/*40*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,
/*50*/  CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),
/*60*/  CYCLES(11),     CYCLES(9),      CYCLES(7),      CYCLES(9),      CYCLES(4),      CYCLES(4),      CYCLES(2),      CYCLES(2),      CYCLES(1),      CYCLES2(14,25), CYCLES(1),      CYCLES2(17,20), CYCLES(17),     CYCLES(17),     CYCLES(17),     CYCLES(17),
/*70*/  &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,

/*80*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(5),      CYCLES(5),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(2),      CYCLES(1),      CYCLES(2),      CYCLES(1),
/*90*/  CYCLES(1),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(0),      CYCLES(4),      CYCLES(4),      CYCLES(5),      CYCLES(2),      CYCLES(3),
/*a0*/  CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(7),      CYCLES(7),      CYCLES(8),      CYCLES(8),      CYCLES(1),      CYCLES(1),      CYCLES(5),      CYCLES(5),      CYCLES(5),      CYCLES(5),      CYCLES(6),      CYCLES(6),
/*b0*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,

/*c0*/  CYCLES(4),      CYCLES(4),      CYCLES(5),      CYCLES(5),      CYCLES(6),      CYCLES(6),      CYCLES(1),      CYCLES(1),      CYCLES(14),     CYCLES(5),      CYCLES(0),      CYCLES(0),      &timing_int,    &timing_int,    CYCLES(3),      CYCLES(0),
/*d0*/  CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(15),     CYCLES(14),     CYCLES(2),      CYCLES(4),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*e0*/  CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(5),      CYCLES(14),     CYCLES(14),     CYCLES(16),     CYCLES(16),     CYCLES(3),      CYCLES(3),      CYCLES(17),     CYCLES(3),      CYCLES(14),     CYCLES(14),     CYCLES(14),     CYCLES(14),
/*f0*/  CYCLES(4),      CYCLES(0),      CYCLES(0),      CYCLES(0),      CYCLES(4),      CYCLES(2),      NULL,           NULL,           CYCLES(2),      CYCLES(2),      CYCLES(3),      CYCLES(2),      CYCLES(2),      CYCLES(2),      CYCLES(3),      NULL
    // clang-format on
};

static int *opcode_timings_486_0f[256] = {
    // clang-format off
/*00*/  CYCLES(20),     CYCLES(11),     CYCLES(11),     CYCLES(10),     NULL,           CYCLES(195),    CYCLES(7),      NULL,           CYCLES(1000),   CYCLES(10000),  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*10*/  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*20*/  CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(6),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*30*/  CYCLES(9),      CYCLES(1),      CYCLES(9),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,

/*40*/  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*50*/  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*60*/  &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     NULL,           NULL,           &timing_rm,     &timing_rm,
/*70*/  NULL,           &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     &timing_rm,     CYCLES(100),    NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           &timing_rm,     &timing_rm,

/*80*/  &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,
/*90*/  CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),
/*a0*/  CYCLES(3),      CYCLES(3),      CYCLES(14),     CYCLES(8),      CYCLES(3),      CYCLES(4),      NULL,           NULL,           CYCLES(3),      CYCLES(3),      NULL,           CYCLES(13),     CYCLES(3),      CYCLES(3),      NULL,           CYCLES2(18,30),
/*b0*/  CYCLES(10),     CYCLES(10),     CYCLES(6),      CYCLES(13),     CYCLES(6),      CYCLES(6),      CYCLES(3),      CYCLES(3),      NULL,           NULL,           CYCLES(6),      CYCLES(13),     CYCLES(7),      CYCLES(7),      CYCLES(3),      CYCLES(3),

/*c0*/  CYCLES(4),      CYCLES(4),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),
/*d0*/  NULL,           &timing_rm,     &timing_rm,     &timing_rm,     NULL,           &timing_rm,     NULL,           NULL,           &timing_rm,     &timing_rm,     NULL,           &timing_rm,     &timing_rm,     &timing_rm,     NULL,           &timing_rm,
/*e0*/  NULL,           &timing_rm,     &timing_rm,     NULL,           NULL,           &timing_rm,     NULL,           NULL,           &timing_rm,     &timing_rm,     NULL,           &timing_rm,     &timing_rm,     &timing_rm,     NULL,           &timing_rm,
/*f0*/  NULL,           &timing_rm,     &timing_rm,     &timing_rm,     NULL,           &timing_rm,     NULL,           NULL,           &timing_rm,     &timing_rm,     &timing_rm,     NULL,           &timing_rm,     &timing_rm,     &timing_rm,     NULL,
    // clang-format on
};
static int *opcode_timings_486_0f_mod3[256] = {
    // clang-format off
/*00*/  CYCLES(20),     CYCLES(11),     CYCLES(11),     CYCLES(10),     NULL,           CYCLES(195),    CYCLES(7),      NULL,           CYCLES(1000),   CYCLES(10000),  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*10*/  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*20*/  CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(6),      CYCLES(6),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*30*/  CYCLES(9),      CYCLES(1),      CYCLES(9),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,

/*40*/  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*50*/  NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*60*/  &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     NULL,           NULL,           &timing_rr,     &timing_rr,
/*70*/  NULL,           &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     CYCLES(100),    NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           &timing_rr,     &timing_rr,

/*80*/  &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,    &timing_bnt,
/*90*/  CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),
/*a0*/  CYCLES(3),      CYCLES(3),      CYCLES(14),     CYCLES(8),      CYCLES(3),      CYCLES(4),      NULL,           NULL,           CYCLES(3),      CYCLES(3),      NULL,           CYCLES(13),     CYCLES(3),      CYCLES(3),      NULL,           CYCLES2(18,30),
/*b0*/  CYCLES(10),     CYCLES(10),     CYCLES(6),      CYCLES(13),     CYCLES(6),      CYCLES(6),      CYCLES(3),      CYCLES(3),      NULL,           NULL,           CYCLES(6),      CYCLES(13),     CYCLES(7),      CYCLES(7),      CYCLES(3),      CYCLES(3),

/*c0*/  CYCLES(4),      CYCLES(4),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),      CYCLES(1),
/*d0*/  NULL,           &timing_rr,     &timing_rr,     &timing_rr,     NULL,           &timing_rr,     NULL,           NULL,           &timing_rr,     &timing_rr,     NULL,           &timing_rr,     &timing_rr,     &timing_rr,     NULL,           &timing_rr,
/*e0*/  NULL,           &timing_rr,     &timing_rr,     NULL,           NULL,           &timing_rr,     NULL,           NULL,           &timing_rr,     &timing_rr,     NULL,           &timing_rr,     &timing_rr,     &timing_rr,     NULL,           &timing_rr,
/*f0*/  NULL,           &timing_rr,     &timing_rr,     &timing_rr,     NULL,           &timing_rr,     NULL,           NULL,           &timing_rr,     &timing_rr,     &timing_rr,     NULL,           &timing_rr,     &timing_rr,     &timing_rr,     NULL,
    // clang-format on
};

static int *opcode_timings_486_shift[8] = {
    // clang-format off
        CYCLES(7),      CYCLES(7),      CYCLES(10),     CYCLES(10),     CYCLES(7),      CYCLES(7),      CYCLES(7),      CYCLES(7)
};
static int *opcode_timings_486_shift_mod3[8] = {
// clang-format off
        CYCLES(3),      CYCLES(3),      CYCLES(9),      CYCLES(9),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3)
    // clang-format on
};

static int *opcode_timings_486_f6[8] = {
    // clang-format off
        &timing_rm,     NULL,           &timing_mm,     &timing_mm,     CYCLES(13),     CYCLES(14),     CYCLES(16),     CYCLES(19)
    // clang-format on
};
static int *opcode_timings_486_f6_mod3[8] = {
    // clang-format off
        &timing_rr,     NULL,           &timing_rr,     &timing_rr,     CYCLES(13),     CYCLES(14),     CYCLES(16),     CYCLES(19)
    // clang-format on
};
static int *opcode_timings_486_f7[8] = {
    // clang-format off
        &timing_rm,     NULL,           &timing_mm,     &timing_mm,     CYCLES(21),     CYCLES2(22,38), CYCLES2(24,40), CYCLES2(27,43)
    // clang-format on
};
static int *opcode_timings_486_f7_mod3[8] = {
    // clang-format off
        &timing_rr,     NULL,           &timing_rr,     &timing_rr,     CYCLES(21),     CYCLES2(22,38), CYCLES2(24,40), CYCLES2(27,43)
};
static int *opcode_timings_486_ff[8] = {
// clang-format off
        &timing_mm,     &timing_mm,     CYCLES(5),      CYCLES(0),      CYCLES(5),      CYCLES(0),      CYCLES(5),      NULL
};
static int *opcode_timings_486_ff_mod3[8] = {
// clang-format off
        &timing_rr,     &timing_rr,     CYCLES(5),      CYCLES(0),      CYCLES(5),      CYCLES(0),      CYCLES(5),      NULL
    // clang-format on
};

static int *opcode_timings_486_d8[8] = {
    // clang-format off
/*      FADDil          FMULil          FCOMil          FCOMPil         FSUBil          FSUBRil         FDIVil          FDIVRil*/
        CYCLES(8),      CYCLES(11),     CYCLES(4),      CYCLES(4),      CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};
static int *opcode_timings_486_d8_mod3[8] = {
    // clang-format off
/*      FADD            FMUL            FCOM            FCOMP           FSUB            FSUBR           FDIV            FDIVR*/
        CYCLES(8),      CYCLES(16),     CYCLES(4),      CYCLES(4),      CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};

static int *opcode_timings_486_d9[8] = {
    // clang-format off
/*      FLDs                            FSTs            FSTPs           FLDENV          FLDCW           FSTENV          FSTCW*/
        CYCLES(3),      NULL,           CYCLES(7),      CYCLES(7),      CYCLES(34),     CYCLES(4),      CYCLES(67),     CYCLES(3)
    // clang-format on
};
static int *opcode_timings_486_d9_mod3[64] = {
    // clang-format off
        /*FLD*/
        CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),
        /*FXCH*/
        CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),      CYCLES(4),
        /*FNOP*/
        CYCLES(3),      NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
        /*FSTP*/
        CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),      CYCLES(3),
/*      opFCHS          opFABS                                          opFTST          opFXAM*/
        CYCLES(6),      CYCLES(3),      NULL,           NULL,           CYCLES(4),      CYCLES(8),      NULL,           NULL,
/*      opFLD1          opFLDL2T        opFLDL2E        opFLDPI         opFLDEG2        opFLDLN2        opFLDZ*/
        CYCLES(4),      CYCLES(8),      CYCLES(8),      CYCLES(8),      CYCLES(8),      CYCLES(8),      CYCLES(4),      NULL,
/*      opF2XM1         opFYL2X         opFPTAN         opFPATAN                                        opFDECSTP       opFINCSTP,*/
        CYCLES(140),    CYCLES(196),    CYCLES(200),    CYCLES(218),    NULL,           NULL,           CYCLES(3),      CYCLES(3),
/*      opFPREM                         opFSQRT         opFSINCOS       opFRNDINT       opFSCALE        opFSIN          opFCOS*/
        CYCLES(70),     NULL,           CYCLES(83),     CYCLES(292),    CYCLES(21),     CYCLES(30),     CYCLES(257),    CYCLES(257)
    // clang-format on
};

static int *opcode_timings_486_da[8] = {
    // clang-format off
/*      FADDil          FMULil          FCOMil          FCOMPil         FSUBil          FSUBRil         FDIVil          FDIVRil*/
        CYCLES(8),      CYCLES(11),     CYCLES(4),      CYCLES(4),      CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};
static int *opcode_timings_486_da_mod3[8] = {
    // clang-format off
        NULL,           NULL,           NULL,           NULL,           NULL,           CYCLES(5),      NULL,           NULL
    // clang-format on
};

static int *opcode_timings_486_db[8] = {
    // clang-format off
/*      FLDil                           FSTil           FSTPil                          FLDe                            FSTPe*/
        CYCLES(9),      NULL,           CYCLES(28),     CYCLES(28),     NULL,           CYCLES(5),      NULL,           CYCLES(6)
    // clang-format on
};
static int *opcode_timings_486_db_mod3[64] = {
    // clang-format off
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
/*                      opFNOP          opFCLEX         opFINIT         opFNOP          opFNOP*/
        NULL,           CYCLES(3),      CYCLES(7),      CYCLES(17),     CYCLES(3),      CYCLES(3),      NULL,
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
        NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,           NULL,
    // clang-format on
};

static int *opcode_timings_486_dc[8] = {
    // clang-format off
/*      opFADDd_a16     opFMULd_a16     opFCOMd_a16     opFCOMPd_a16    opFSUBd_a16     opFSUBRd_a16    opFDIVd_a16     opFDIVRd_a16*/
        CYCLES(8),      CYCLES(11),     CYCLES(4),      CYCLES(4),      CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};
static int *opcode_timings_486_dc_mod3[8] = {
    // clang-format off
/*      opFADDr         opFMULr                                         opFSUBRr        opFSUBr         opFDIVRr        opFDIVr*/
        CYCLES(8),      CYCLES(16),     NULL,           NULL,           CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};

static int *opcode_timings_486_dd[8] = {
    // clang-format off
/*      FLDd                            FSTd            FSTPd           FRSTOR                           FSAVE          FSTSW*/
        CYCLES(3),      NULL,           CYCLES(8),      CYCLES(8),      CYCLES(131),     NULL,           CYCLES(154),   CYCLES(3)
    // clang-format on
};
static int *opcode_timings_486_dd_mod3[8] = {
    // clang-format off
/*      FFFREE                          FST             FSTP            FUCOM            FUCOMP*/
        CYCLES(3),      NULL,           CYCLES(3),      CYCLES(3),      CYCLES(4),       CYCLES(4),      NULL,          NULL
    // clang-format on
};

static int *opcode_timings_486_de[8] = {
    // clang-format off
/*      FADDiw          FMULiw          FCOMiw          FCOMPiw         FSUBil          FSUBRil         FDIVil          FDIVRil*/
        CYCLES(8),      CYCLES(11),     CYCLES(4),      CYCLES(4),      CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};
static int *opcode_timings_486_de_mod3[8] = {
    // clang-format off
/*      FADD            FMUL                            FCOMPP          FSUB            FSUBR           FDIV            FDIVR*/
        CYCLES(8),      CYCLES(16),     NULL,           CYCLES(5),      CYCLES(8),      CYCLES(8),      CYCLES(73),     CYCLES(73)
    // clang-format on
};

static int *opcode_timings_486_df[8] = {
    // clang-format off
/*      FILDiw                          FISTiw          FISTPiw                          FILDiq          FBSTP          FISTPiq*/
        CYCLES(13),     NULL,           CYCLES(29),     CYCLES(29),     NULL,            CYCLES(10),     CYCLES(172),   CYCLES(28)
    // clang-format on
};
static int *opcode_timings_486_df_mod3[8] = {
    // clang-format off
/*      FFREE                           FST             FSTP            FUCOM            FUCOMP*/
        CYCLES(3),      NULL,           CYCLES(3),      CYCLES(3),      CYCLES(4),       CYCLES(4),      NULL,          NULL
    // clang-format on
};

static int *opcode_timings_486_8x[8] = {
    // clang-format off
        &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_rm
    // clang-format on
};
static int *opcode_timings_486_8x_mod3[8] = {
    // clang-format off
        &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr
    // clang-format on
};
static int *opcode_timings_486_81[8] = {
    // clang-format off
        &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_mr,     &timing_rm
    // clang-format on
};
static int *opcode_timings_486_81_mod3[8] = {
    // clang-format off
        &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr,     &timing_rr
    // clang-format on
};

static int      timing_count;
static uint8_t  last_prefix;
static uint32_t regmask_modified;

static inline int
COUNT(int *c, int op_32)
{
    if ((uintptr_t) c <= 10000)
        return (int) (uintptr_t) c;
    if (((uintptr_t) c & ~0xffff) == (-1 & ~0xffff)) {
        if (op_32 & 0x100)
            return ((uintptr_t) c >> 8) & 0xff;
        return (uintptr_t) c & 0xff;
    }
    return *c;
}

/*
 * x87 concurrency (Intel i486 Processor Programmer's Reference Manual, floating
 * point instruction clock counts; 86Box's x87_concurrency_486, which holds the
 * same figures). Of an instruction's clocks the last "concurrent" ones run in
 * the FPU while the integer unit goes on: FDIV 73 clocks, 70 concurrent; FMUL
 * 16/13; FADD 8-20/7. The next x87 instruction waits for the FPU. fpu_busy is
 * the FPU time still to run, in cycles of the integer timeline. It is tracked
 * within a block; codegen_fpu_sync carries it across blocks.
 */
static int fpu_busy;

static int
fpu_concurrent_486(int prefix, int modrm)
{
    const x87_timings_t *c    = &x87_concurrency_486;
    const int            reg  = (modrm >> 3) & 7;
    const int            mod3 = (modrm & 0xc0) == 0xc0;

    switch (prefix) {
        case 0xd8:
        case 0xdc:
        case 0xde:
            switch (reg) {
                case 1:
                    return mod3 ? c->fmul : prefix == 0xd8 ? c->fmul_32 : prefix == 0xdc ? c->fmul_64 : c->fmul_i16;
                case 2:
                case 3:
                    return c->fcom;
                case 6:
                case 7:
                    return c->fdiv;
                default:
                    return c->fadd;
            }
        case 0xda:
            return mod3 ? c->fucom : reg == 1 ? c->fmul_i32 : reg >= 6 ? c->fdiv_i32 : reg >= 2 && reg <= 3 ? c->fcom_i32 : c->fadd_i32;
        case 0xd9:
            if (!mod3)
                return 0;
            switch (modrm) {
                case 0xe8 ... 0xee:
                    return c->fld_const;
                case 0xf0:
                    return c->f2xm1;
                case 0xf1:
                    return c->fyl2x;
                case 0xf2:
                    return c->fptan;
                case 0xf3:
                    return c->fpatan;
                case 0xf4:
                    return c->fxtract;
                case 0xf5:
                    return c->fprem1;
                case 0xf8:
                    return c->fprem;
                case 0xf9:
                    return c->fyl2xp1;
                case 0xfa:
                    return c->fsqrt;
                case 0xfb:
                    return c->fsincos;
                case 0xfd:
                    return c->fscale;
                case 0xfe:
                case 0xff:
                    return c->fsin_cos;
                default:
                    return 0;
            }
        case 0xdb:
            return mod3 ? 0 : (reg == 0 ? c->fild_32 : 0);
        case 0xdf:
            return mod3 ? 0 : (reg == 0 ? c->fild_16 : reg == 5 ? c->fild_64 : 0);
        default:
            return 0;
    }
}

void
codegen_timing_486_block_start(void)
{
    regmask_modified = 0;
    fpu_busy         = 0;
}

void
codegen_timing_486_start(void)
{
    timing_count = 0;
    last_prefix  = 0;
}

void
codegen_timing_486_prefix(uint8_t prefix, UNUSED(uint32_t fetchdat))
{
    /*Counted as it is decoded: a fetch fault in a later prefix leaves with
      the cycles of the ones before it (codegen_generate_call)*/
    codegen_block_cycles += COUNT(opcode_timings_486[prefix], 0);
    last_prefix = prefix;
}

static int
has_base_index(uint32_t fetchdat, int op_32)
{
    const int rm = fetchdat & 7;
    const int md = (fetchdat >> 6) & 3;

    if (!(op_32 & 0x200))
        return rm < 4;
    if (rm != 4)
        return 0;
    const int sib = (fetchdat >> 8) & 0xff;
    return ((sib >> 3) & 7) != 4 && !((sib & 7) == 5 && md == 0);
}

void
codegen_timing_486_opcode(uint8_t opcode, uint32_t fetchdat, int op_32, UNUSED(uint32_t op_pc))
{
    const uint8_t raw_opcode = opcode;

    int           **timings;
    const uint64_t *deps;
    int             mod3 = ((fetchdat & 0xc0) == 0xc0);
    int             bit8 = !(opcode & 1);

    switch (last_prefix) {
        case 0x0f:
            timings = mod3 ? opcode_timings_486_0f_mod3 : opcode_timings_486_0f;
            deps    = mod3 ? opcode_deps_0f_mod3 : opcode_deps_0f;
            break;

        case 0xd8:
            timings = mod3 ? opcode_timings_486_d8_mod3 : opcode_timings_486_d8;
            deps    = mod3 ? opcode_deps_d8_mod3 : opcode_deps_d8;
            opcode  = (opcode >> 3) & 7;
            break;
        case 0xd9:
            timings = mod3 ? opcode_timings_486_d9_mod3 : opcode_timings_486_d9;
            deps    = mod3 ? opcode_deps_d9_mod3 : opcode_deps_d9;
            opcode  = mod3 ? opcode & 0x3f : (opcode >> 3) & 7;
            break;
        case 0xda:
            timings = mod3 ? opcode_timings_486_da_mod3 : opcode_timings_486_da;
            deps    = mod3 ? opcode_deps_da_mod3 : opcode_deps_da;
            opcode  = (opcode >> 3) & 7;
            break;
        case 0xdb:
            timings = mod3 ? opcode_timings_486_db_mod3 : opcode_timings_486_db;
            deps    = mod3 ? opcode_deps_db_mod3 : opcode_deps_db;
            opcode  = mod3 ? opcode & 0x3f : (opcode >> 3) & 7;
            break;
        case 0xdc:
            timings = mod3 ? opcode_timings_486_dc_mod3 : opcode_timings_486_dc;
            deps    = mod3 ? opcode_deps_dc_mod3 : opcode_deps_dc;
            opcode  = (opcode >> 3) & 7;
            break;
        case 0xdd:
            timings = mod3 ? opcode_timings_486_dd_mod3 : opcode_timings_486_dd;
            deps    = mod3 ? opcode_deps_dd_mod3 : opcode_deps_dd;
            opcode  = (opcode >> 3) & 7;
            break;
        case 0xde:
            timings = mod3 ? opcode_timings_486_de_mod3 : opcode_timings_486_de;
            deps    = mod3 ? opcode_deps_de_mod3 : opcode_deps_de;
            opcode  = (opcode >> 3) & 7;
            break;
        case 0xdf:
            timings = mod3 ? opcode_timings_486_df_mod3 : opcode_timings_486_df;
            deps    = mod3 ? opcode_deps_df_mod3 : opcode_deps_df;
            opcode  = (opcode >> 3) & 7;
            break;

        default:
            switch (opcode) {
                case 0x80:
                case 0x82:
                case 0x83:
                    timings = mod3 ? opcode_timings_486_8x_mod3 : opcode_timings_486_8x;
                    deps    = mod3 ? opcode_deps_8x_mod3 : opcode_deps_8x;
                    opcode  = (fetchdat >> 3) & 7;
                    break;
                case 0x81:
                    timings = mod3 ? opcode_timings_486_81_mod3 : opcode_timings_486_81;
                    deps    = mod3 ? opcode_deps_81_mod3 : opcode_deps_81;
                    opcode  = (fetchdat >> 3) & 7;
                    break;

                case 0xc0:
                case 0xc1:
                case 0xd0:
                case 0xd1:
                case 0xd2:
                case 0xd3:
                    timings = mod3 ? opcode_timings_486_shift_mod3 : opcode_timings_486_shift;
                    deps    = mod3 ? opcode_deps_shift_mod3 : opcode_deps_shift;
                    opcode  = (fetchdat >> 3) & 7;
                    break;

                case 0xf6:
                    timings = mod3 ? opcode_timings_486_f6_mod3 : opcode_timings_486_f6;
                    deps    = mod3 ? opcode_deps_f6_mod3 : opcode_deps_f6;
                    opcode  = (fetchdat >> 3) & 7;
                    break;
                case 0xf7:
                    timings = mod3 ? opcode_timings_486_f7_mod3 : opcode_timings_486_f7;
                    deps    = mod3 ? opcode_deps_f7_mod3 : opcode_deps_f7;
                    opcode  = (fetchdat >> 3) & 7;
                    break;
                case 0xff:
                    timings = mod3 ? opcode_timings_486_ff_mod3 : opcode_timings_486_ff;
                    deps    = mod3 ? opcode_deps_ff_mod3 : opcode_deps_ff;
                    opcode  = (fetchdat >> 3) & 7;
                    break;

                default:
                    timings = mod3 ? opcode_timings_486_mod3 : opcode_timings_486;
                    deps    = mod3 ? opcode_deps_mod3 : opcode_deps;
                    break;
            }
    }

    int own_count = COUNT(timings[opcode], op_32);
    timing_count += own_count;
    if (!mod3 && has_base_index(fetchdat, op_32))
        timing_count++; /*base+index addressing: one extra cycle (i486 programmer's reference)*/
    if (regmask_modified & get_addr_regmask(deps[opcode], fetchdat, op_32))
        timing_count++; /*AGI stall*/
    if (last_prefix >= 0xd8 && last_prefix <= 0xdf) {
        const int concurrent = MIN(fpu_concurrent_486(last_prefix, raw_opcode), own_count);

        timing_count += fpu_busy; /*an x87 instruction waits for the one before*/
        timing_count -= concurrent;
        fpu_busy         = concurrent;
        codegen_fpu_x87  = 3; /*waits for the FPU and leaves it busy*/
        codegen_fpu_busy = concurrent;
    } else if (fpu_busy) {
        fpu_busy -= MIN(fpu_busy, timing_count);
    }
    codegen_block_cycles += timing_count;

    regmask_modified = get_dstdep_mask(deps[opcode], fetchdat, bit8);
}

void
codegen_timing_486_block_end(void)
{
    //
}

codegen_timing_t codegen_timing_486 = {
    codegen_timing_486_start,
    codegen_timing_486_prefix,
    codegen_timing_486_opcode,
    codegen_timing_486_block_start,
    codegen_timing_486_block_end,
    NULL
};

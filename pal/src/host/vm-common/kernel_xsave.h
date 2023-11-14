/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2023 Intel Corporation */

/* Declarations for FP regs aka xsave (via `xsave`/`xrstor` instructions). Note that we only support
 * x86-64 CPUs that have the XSAVE feature. */

#pragma once

#include <stdint.h>

#define VM_XSAVE_ALIGN 64
#define VM_XSAVE_RESET_STATE_SIZE (512 + 64) /* 512 for legacy regs, 64 for xsave header */

enum VM_XFEATURE {
    VM_XFEATURE_FP,
    VM_XFEATURE_SSE,
    VM_XFEATURE_YMM,
    VM_XFEATURE_BNDREGS,
    VM_XFEATURE_BNDCSR,
    VM_XFEATURE_OPMASK,
    VM_XFEATURE_ZMM_Hi256,
    VM_XFEATURE_Hi16_ZMM,
    VM_XFEATURE_RSRVD8,
    VM_XFEATURE_PKRU,
    VM_XFEATURE_RSRVD10,
    VM_XFEATURE_RSRVD11,
    VM_XFEATURE_RSRVD12,
    VM_XFEATURE_RSRVD13,
    VM_XFEATURE_RSRVD14,
    VM_XFEATURE_RSRVD15,
    VM_XFEATURE_RSRVD16,
    VM_XFEATURE_AMX_CFG,
    VM_XFEATURE_AMX_DATA,
};

#define VM_XFEATURE_MASK_FP        (1UL << VM_XFEATURE_FP)
#define VM_XFEATURE_MASK_SSE       (1UL << VM_XFEATURE_SSE)
#define VM_XFEATURE_MASK_YMM       (1UL << VM_XFEATURE_YMM)
#define VM_XFEATURE_MASK_BNDREGS   (1UL << VM_XFEATURE_BNDREGS)
#define VM_XFEATURE_MASK_BNDCSR    (1UL << VM_XFEATURE_BNDCSR)
#define VM_XFEATURE_MASK_OPMASK    (1UL << VM_XFEATURE_OPMASK)
#define VM_XFEATURE_MASK_ZMM_Hi256 (1UL << VM_XFEATURE_ZMM_Hi256)
#define VM_XFEATURE_MASK_Hi16_ZMM  (1UL << VM_XFEATURE_Hi16_ZMM)
#define VM_XFEATURE_MASK_RSRVD8    (1UL << VM_XFEATURE_RSRVD8)
#define VM_XFEATURE_MASK_PKRU      (1UL << VM_XFEATURE_PKRU)
#define VM_XFEATURE_MASK_RSRVD10   (1UL << VM_XFEATURE_RSRVD10)
#define VM_XFEATURE_MASK_RSRVD11   (1UL << VM_XFEATURE_RSRVD11)
#define VM_XFEATURE_MASK_RSRVD12   (1UL << VM_XFEATURE_RSRVD12)
#define VM_XFEATURE_MASK_RSRVD13   (1UL << VM_XFEATURE_RSRVD13)
#define VM_XFEATURE_MASK_RSRVD14   (1UL << VM_XFEATURE_RSRVD14)
#define VM_XFEATURE_MASK_RSRVD15   (1UL << VM_XFEATURE_RSRVD15)
#define VM_XFEATURE_MASK_RSRVD16   (1UL << VM_XFEATURE_RSRVD16)
#define VM_XFEATURE_MASK_AMX_CFG   (1UL << VM_XFEATURE_AMX_CFG)
#define VM_XFEATURE_MASK_AMX_DATA  (1UL << VM_XFEATURE_AMX_DATA)

#define VM_XFEATURE_MASK_FPSSE     (VM_XFEATURE_MASK_FP | VM_XFEATURE_MASK_SSE)
#define VM_XFEATURE_MASK_AVX512    (VM_XFEATURE_MASK_OPMASK | VM_XFEATURE_MASK_ZMM_Hi256 \
                                      | VM_XFEATURE_MASK_Hi16_ZMM)

extern uint64_t g_xcr0;
extern uint32_t g_xsave_size;
extern const uint32_t g_xsave_reset_state[VM_XSAVE_RESET_STATE_SIZE / sizeof(uint32_t)];

int xsave_init(void);
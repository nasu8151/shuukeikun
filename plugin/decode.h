#ifndef DECODE_H
#define DECODE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Instruction classes tracked by the bitwidth plugin.
 *
 * Write-back classes (ADD..ROR): the result written back to reg_a is
 * measured after the instruction executes.
 *
 * Compare classes (CMP/CMN/TST/TEQ): these do not write back a register,
 * only flags. The plugin recomputes the discarded ALU result from the
 * (pre-execution) operand values (reg_a op reg_b, or reg_a op imm_b) and
 * measures that.
 *
 * CLS_STR: the value of the source register being written to memory by a
 * single-register store (STR/STRB/STRH, immediate or register offset).
 * Multi-register stores (STM/PUSH) and STRD are not decoded (v1 scope).
 */
typedef enum {
    CLS_ADD, CLS_SUB, CLS_RSB, CLS_ADC, CLS_SBC,
    CLS_AND, CLS_ORR, CLS_EOR, CLS_BIC, CLS_MVN,
    CLS_MUL, CLS_DIV,
    CLS_LSL, CLS_LSR, CLS_ASR, CLS_ROR,
    CLS_CMP, CLS_CMN, CLS_TST, CLS_TEQ,
    CLS_STR,
    CLS_COUNT
} InsnClass;

extern const char *insn_class_names[CLS_COUNT];

typedef struct {
    InsnClass cls;
    bool is_writeback;   /* true: read reg_a post-exec. false: compute from reg_a/reg_b(or imm_b) */
    int reg_a;            /* writeback dest reg / compare first operand reg / store data reg */
    int reg_b;             /* compare second operand reg, or -1 if immediate operand used */
    uint32_t imm_b;        /* compare second operand immediate, valid when reg_b == -1 */
    int store_size;        /* CLS_STR only: 1, 2 or 4 bytes */
} DecodedInsn;

/*
 * Decode one Thumb instruction.
 * hw1: first (or only) 16-bit halfword, in host order (already byte-swapped
 *      from the little-endian instruction stream).
 * hw2: second halfword for 32-bit Thumb-2 instructions; ignored if size==2.
 * size: instruction length in bytes (2 or 4), as reported by QEMU.
 *
 * Returns true and fills *out if the instruction is one of the tracked
 * ALU/store classes; returns false otherwise (branches, loads, moves,
 * SP/PC-relative address formation, multi-register transfers, DSP/FPU
 * instructions, etc. -- all intentionally out of scope).
 */
bool decode_insn(uint16_t hw1, uint16_t hw2, int size, DecodedInsn *out);

/* ARM Thumb-2 modified-immediate expansion (T32ExpandImm), given the raw
 * 12-bit i:imm3:imm8 field. Exposed for unit testing. */
uint32_t thumb_expand_imm(uint32_t imm12);

#endif /* DECODE_H */

/*
 * Minimal Thumb/Thumb-2 decoder for the ALU-operand-bitwidth QEMU plugin.
 *
 * Scope: ARMv7-M baseline (Cortex-M3: no DSP extension, no FPU, no MVE).
 * Only the encodings needed to classify ALU-class instructions (and single
 * register stores) and extract their relevant register indices are
 * implemented; everything else decodes as "not matched" and is ignored by
 * the plugin.
 *
 * Bit-field layouts were taken directly from QEMU's own Thumb decodetree
 * specifications (target/arm/tcg/t16.decode, t32.decode) rather than
 * reconstructed from memory, to avoid transcription errors.
 */

#include "decode.h"

const char *insn_class_names[CLS_COUNT] = {
    "ADD", "SUB", "RSB", "ADC", "SBC",
    "AND", "ORR", "EOR", "BIC", "MVN",
    "MUL", "DIV",
    "LSL", "LSR", "ASR", "ROR",
    "CMP", "CMN", "TST", "TEQ",
    "STR",
};

uint32_t thumb_expand_imm(uint32_t imm12)
{
    uint32_t imm8 = imm12 & 0xFF;
    uint32_t sel = (imm12 >> 8) & 0xF; /* i:imm3[2:0] */

    switch (sel) {
    case 0:
        return imm8;
    case 1:
        return imm8 * 0x00010001u;
    case 2:
        return imm8 * 0x01000100u;
    case 3:
        return imm8 * 0x01010101u;
    default: {
        uint32_t unrotated = imm8 | 0x80;
        unsigned rot = (imm12 >> 7) & 0x1F;
        return rot ? (unrotated >> rot) | (unrotated << (32 - rot)) : unrotated;
    }
    }
}

static void set_writeback(DecodedInsn *out, InsnClass cls, int rd)
{
    out->cls = cls;
    out->is_writeback = true;
    out->reg_a = rd;
}

static void set_compare_reg(DecodedInsn *out, InsnClass cls, int rn, int rm)
{
    out->cls = cls;
    out->is_writeback = false;
    out->reg_a = rn;
    out->reg_b = rm;
}

static void set_compare_imm(DecodedInsn *out, InsnClass cls, int rn, uint32_t imm)
{
    out->cls = cls;
    out->is_writeback = false;
    out->reg_a = rn;
    out->reg_b = -1;
    out->imm_b = imm;
}

static void set_store(DecodedInsn *out, int rt, int size)
{
    out->cls = CLS_STR;
    out->is_writeback = false;
    out->reg_a = rt;
    out->store_size = size;
}

static bool decode_t16(uint16_t hw, DecodedInsn *out)
{
    /* Data-processing (two low registers): 010000 oooo rm(3) rd(3) */
    if ((hw & 0xFC00) == 0x4000) {
        unsigned op = (hw >> 6) & 0xF;
        unsigned ra = (hw >> 3) & 0x7; /* rm, or rn for MUL/RSB/MVN's source */
        unsigned rb = hw & 0x7;        /* rdn (dest), or rn/rd for TST/CMP/CMN/MUL/RSB */

        switch (op) {
        case 0x8: /* TST */
            set_compare_reg(out, CLS_TST, rb, ra);
            return true;
        case 0xA: /* CMP */
            set_compare_reg(out, CLS_CMP, rb, ra);
            return true;
        case 0xB: /* CMN */
            set_compare_reg(out, CLS_CMN, rb, ra);
            return true;
        default: {
            static const InsnClass map[16] = {
                CLS_AND, CLS_EOR, CLS_LSL, CLS_LSR, CLS_ASR, CLS_ADC, CLS_SBC, CLS_ROR,
                CLS_COUNT, CLS_RSB, CLS_COUNT, CLS_COUNT,
                CLS_ORR, CLS_MUL, CLS_BIC, CLS_MVN,
            };
            set_writeback(out, map[op], rb);
            return true;
        }
        }
    }

    /* Shift (immediate): 000 shty(2) imm5(5) rm(3) rd(3); shty=11 belongs
     * to the add/sub group below, so it never reaches here. */
    if ((hw & 0xE000) == 0x0000 && ((hw >> 11) & 0x3) != 0x3) {
        unsigned shty = (hw >> 11) & 0x3;
        unsigned shim = (hw >> 6) & 0x1F;
        unsigned rd = hw & 0x7;
        if (shty == 0 && shim == 0) {
            return false; /* MOV (register) alias -- data movement, excluded */
        }
        set_writeback(out, shty == 0 ? CLS_LSL : shty == 1 ? CLS_LSR : CLS_ASR, rd);
        return true;
    }

    /* Add/subtract (three low regs, or two low regs + imm3): 00011 ... */
    if ((hw & 0xF800) == 0x1800) {
        bool is_sub = (hw >> 9) & 1;
        unsigned rd = hw & 0x7;
        set_writeback(out, is_sub ? CLS_SUB : CLS_ADD, rd);
        return true;
    }

    /* Move/compare/add/subtract immediate (one low reg + imm8): 001 op(2) */
    if ((hw & 0xE000) == 0x2000) {
        unsigned top5 = (hw >> 11) & 0x1F;
        unsigned rd = (hw >> 8) & 0x7;
        unsigned imm8 = hw & 0xFF;
        if (top5 == 4) {
            return false; /* MOV immediate -- excluded */
        } else if (top5 == 5) {
            set_compare_imm(out, CLS_CMP, rd, imm8);
            return true;
        } else if (top5 == 6) {
            set_writeback(out, CLS_ADD, rd);
            return true;
        } else if (top5 == 7) {
            set_writeback(out, CLS_SUB, rd);
            return true;
        }
        return false;
    }

    /* Special data instructions (add/cmp/mov with high registers) */
    if ((hw & 0xFF00) == 0x4400 || (hw & 0xFF00) == 0x4500 || (hw & 0xFF00) == 0x4600) {
        unsigned dn = (hw >> 7) & 1;
        unsigned rm = (hw >> 3) & 0xF;
        unsigned rdn = (dn << 3) | (hw & 0x7);
        if ((hw & 0xFF00) == 0x4400) { /* ADD (hi) */
            if (rdn == 13 || rdn == 15) {
                return false; /* SP/PC involved -- address/branch, excluded */
            }
            set_writeback(out, CLS_ADD, rdn);
            return true;
        } else if ((hw & 0xFF00) == 0x4500) { /* CMP (hi) */
            set_compare_reg(out, CLS_CMP, rdn, rm);
            return true;
        }
        return false; /* MOV (hi) -- excluded */
    }

    /* ADD/SUB (SP plus immediate): 1011 0000 x imm7 -- stack frame
     * adjustment, address calculation, excluded entirely. */
    if ((hw & 0xFF00) == 0xB000) {
        return false;
    }

    /* ADR / ADD (SP plus immediate, Rd form): 1010 x ... -- address
     * formation, excluded entirely. */
    if ((hw & 0xF000) == 0xA000) {
        return false;
    }

    /* Load/store (register offset): 0101 ooo rm(3) rn(3) rt(3) */
    if ((hw & 0xFE00) == 0x5000) { /* STR */
        set_store(out, hw & 0x7, 4);
        return true;
    }
    if ((hw & 0xFE00) == 0x5200) { /* STRH */
        set_store(out, hw & 0x7, 2);
        return true;
    }
    if ((hw & 0xFE00) == 0x5400) { /* STRB */
        set_store(out, hw & 0x7, 1);
        return true;
    }

    /* Load/store word/byte (immediate offset): 011xx imm5 rn(3) rt(3) */
    if ((hw & 0xF800) == 0x6000) { /* STR */
        set_store(out, hw & 0x7, 4);
        return true;
    }
    if ((hw & 0xF800) == 0x7000) { /* STRB */
        set_store(out, hw & 0x7, 1);
        return true;
    }

    /* Load/store halfword (immediate offset): 10000 imm5 rn(3) rt(3) */
    if ((hw & 0xF800) == 0x8000) { /* STRH */
        set_store(out, hw & 0x7, 2);
        return true;
    }

    /* Load/store (SP-relative): 10010 rt(3) imm8 */
    if ((hw & 0xF800) == 0x9000) { /* STR */
        set_store(out, (hw >> 8) & 0x7, 4);
        return true;
    }

    return false;
}

/* Data-processing (register) / (register-shifted register) / (immediate),
 * shared opcode table used by both the register and modified-immediate
 * 32-bit forms (t32.decode groups them with identical op4 assignments). */
static bool decode_t32_dpi_common(unsigned op4, bool s, unsigned rn, unsigned rd,
                                   bool is_reg, unsigned rm_or_zero,
                                   uint32_t imm12_or_zero, DecodedInsn *out)
{
    bool is_special = s && rd == 0xF; /* TST/TEQ/CMN/CMP carve-out */

    switch (op4) {
    case 0x0: /* AND / TST */
        if (is_special) {
            if (is_reg) {
                set_compare_reg(out, CLS_TST, rn, rm_or_zero);
            } else {
                set_compare_imm(out, CLS_TST, rn, thumb_expand_imm(imm12_or_zero));
            }
            return true;
        }
        set_writeback(out, CLS_AND, rd);
        return true;
    case 0x1: /* BIC */
        set_writeback(out, CLS_BIC, rd);
        return true;
    case 0x2: /* ORR, or MOV(.W)/shift-immediate alias when rn==PC */
        if (rn == 0xF) {
            if (is_reg) {
                /* MOV_rxri: shty/shim encoded in the caller-provided
                 * rm_or_zero/imm12_or_zero smuggling -- handled by caller. */
                return false; /* handled specially in decode_t32() */
            }
            return false; /* MOV (immediate), plain constant load -- excluded */
        }
        set_writeback(out, CLS_ORR, rd);
        return true;
    case 0x3: /* MVN (rn==PC) or ORN */
        set_writeback(out, rn == 0xF ? CLS_MVN : CLS_ORR, rd);
        return true;
    case 0x4: /* EOR / TEQ */
        if (is_special) {
            if (is_reg) {
                set_compare_reg(out, CLS_TEQ, rn, rm_or_zero);
            } else {
                set_compare_imm(out, CLS_TEQ, rn, thumb_expand_imm(imm12_or_zero));
            }
            return true;
        }
        set_writeback(out, CLS_EOR, rd);
        return true;
    case 0x8: /* ADD / CMN */
        if (is_special) {
            if (is_reg) {
                set_compare_reg(out, CLS_CMN, rn, rm_or_zero);
            } else {
                set_compare_imm(out, CLS_CMN, rn, thumb_expand_imm(imm12_or_zero));
            }
            return true;
        }
        if (rn == 13) {
            return false; /* ADD involving SP -- address calc, excluded */
        }
        set_writeback(out, CLS_ADD, rd);
        return true;
    case 0xA: /* ADC */
        set_writeback(out, CLS_ADC, rd);
        return true;
    case 0xB: /* SBC */
        set_writeback(out, CLS_SBC, rd);
        return true;
    case 0xD: /* SUB / CMP */
        if (is_special) {
            if (is_reg) {
                set_compare_reg(out, CLS_CMP, rn, rm_or_zero);
            } else {
                set_compare_imm(out, CLS_CMP, rn, thumb_expand_imm(imm12_or_zero));
            }
            return true;
        }
        if (rn == 13) {
            return false; /* SUB involving SP -- address calc, excluded */
        }
        set_writeback(out, CLS_SUB, rd);
        return true;
    case 0xE: /* RSB */
        set_writeback(out, CLS_RSB, rd);
        return true;
    default:
        return false;
    }
}

static bool decode_t32(uint16_t hw1, uint16_t hw2, DecodedInsn *out)
{
    /* Data-processing (register-shifted register): LSL/LSR/ASR/ROR (reg) */
    if ((hw1 & 0xFF80) == 0xFA00 && (hw2 & 0xF0F0) == 0xF000) {
        unsigned shty = (hw1 >> 5) & 0x3;
        unsigned rd = (hw2 >> 8) & 0xF;
        static const InsnClass map[4] = { CLS_LSL, CLS_LSR, CLS_ASR, CLS_ROR };
        set_writeback(out, map[shty], rd);
        return true;
    }

    /* Data-processing (register), 3-operand with optional shift */
    if ((hw1 & 0xFE00) == 0xEA00) {
        unsigned op4 = (hw1 >> 5) & 0xF;
        bool s = (hw1 >> 4) & 1;
        unsigned rn = hw1 & 0xF;
        unsigned rd = (hw2 >> 8) & 0xF;
        unsigned rm = hw2 & 0xF;

        if (op4 == 0x2 && rn == 0xF) {
            /* MOV.W Rd, Rm {,shift} -- true MOV when shift is LSL #0,
             * otherwise an alias for LSL/LSR/ASR/ROR (immediate). */
            unsigned imm3 = (hw2 >> 12) & 0x7;
            unsigned imm2 = (hw2 >> 6) & 0x3;
            unsigned shim = (imm3 << 2) | imm2;
            unsigned shty = (hw2 >> 4) & 0x3;
            if (shty == 0 && shim == 0) {
                return false; /* MOV (register) -- excluded */
            }
            static const InsnClass map[4] = { CLS_LSL, CLS_LSR, CLS_ASR, CLS_ROR };
            set_writeback(out, map[shty], rd);
            return true;
        }
        return decode_t32_dpi_common(op4, s, rn, rd, true, rm, 0, out);
    }

    /* Data-processing (modified immediate) */
    if ((hw1 & 0xFA00) == 0xF000) {
        unsigned op4 = (hw1 >> 5) & 0xF;
        bool s = (hw1 >> 4) & 1;
        unsigned rn = hw1 & 0xF;
        unsigned rd = (hw2 >> 8) & 0xF;
        unsigned i = (hw1 >> 10) & 1;
        unsigned imm3 = (hw2 >> 12) & 0x7;
        unsigned imm8 = hw2 & 0xFF;
        uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;

        if (op4 == 0x2 && rn == 0xF) {
            return false; /* MOV (immediate) -- excluded */
        }
        return decode_t32_dpi_common(op4, s, rn, rd, false, 0, imm12, out);
    }

    /* Data-processing (plain binary immediate): ADDW/SUBW only (ADR is the
     * rn==PC alternative in the same encoding slot, excluded). */
    if ((hw1 & 0xFBF0) == 0xF200) { /* ADDW */
        unsigned rn = hw1 & 0xF;
        unsigned rd = (hw2 >> 8) & 0xF;
        if (rn == 0xF || rn == 13) {
            return false; /* ADR, or ADD involving SP -- address calc */
        }
        set_writeback(out, CLS_ADD, rd);
        return true;
    }
    if ((hw1 & 0xFBF0) == 0xF2A0) { /* SUBW */
        unsigned rn = hw1 & 0xF;
        unsigned rd = (hw2 >> 8) & 0xF;
        if (rn == 0xF || rn == 13) {
            return false; /* ADR (negative), or SUB involving SP */
        }
        set_writeback(out, CLS_SUB, rd);
        return true;
    }

    /* Store, single register (STR/STRB/STRH, all addressing modes) */
    if ((hw1 & 0xFFF0) == 0xF800) { /* STRB, rr or ri (idx/neg/unpriv) */
        set_store(out, (hw2 >> 12) & 0xF, 1);
        return true;
    }
    if ((hw1 & 0xFFF0) == 0xF880) { /* STRB, ri positive-immediate */
        set_store(out, (hw2 >> 12) & 0xF, 1);
        return true;
    }
    if ((hw1 & 0xFFF0) == 0xF820) { /* STRH, rr or ri (idx/neg/unpriv) */
        set_store(out, (hw2 >> 12) & 0xF, 2);
        return true;
    }
    if ((hw1 & 0xFFF0) == 0xF8A0) { /* STRH, ri positive-immediate */
        set_store(out, (hw2 >> 12) & 0xF, 2);
        return true;
    }
    if ((hw1 & 0xFFF0) == 0xF840) { /* STR, rr or ri (idx/neg/unpriv) */
        set_store(out, (hw2 >> 12) & 0xF, 4);
        return true;
    }
    if ((hw1 & 0xFFF0) == 0xF8C0) { /* STR, ri positive-immediate */
        set_store(out, (hw2 >> 12) & 0xF, 4);
        return true;
    }

    /* Multiply / multiply-accumulate: MUL, MLA, MLS (32-bit result only) */
    if ((hw1 & 0xFFF0) == 0xFB00) {
        unsigned sel = (hw2 >> 4) & 0xF;
        if (sel == 0x0 || sel == 0x1) {
            set_writeback(out, CLS_MUL, (hw2 >> 8) & 0xF);
            return true;
        }
        return false;
    }

    /* SDIV / UDIV */
    if ((hw1 & 0xFFF0) == 0xFB90 && (hw2 & 0xF0F0) == 0xF0F0) {
        set_writeback(out, CLS_DIV, (hw2 >> 8) & 0xF);
        return true;
    }
    if ((hw1 & 0xFFF0) == 0xFBB0 && (hw2 & 0xF0F0) == 0xF0F0) {
        set_writeback(out, CLS_DIV, (hw2 >> 8) & 0xF);
        return true;
    }

    return false;
}

bool decode_insn(uint16_t hw1, uint16_t hw2, int size, DecodedInsn *out)
{
    if (size == 2) {
        return decode_t16(hw1, out);
    } else if (size == 4) {
        return decode_t32(hw1, hw2, out);
    }
    return false;
}

/*
 * bitwidth plugin -- histograms the effective bit-width of values produced
 * by ALU-class instructions (and stored by single-register STR variants)
 * on arm-softmmu (Cortex-M / ARMv7-M) targets.
 *
 * For write-back classes (ADD/SUB/.../ROR/MUL/DIV) the value measured is
 * the result written back to the destination register.
 *
 * IMPORTANT: qemu_plugin_register_vcpu_insn_exec_cb() fires *before* the
 * instruction's own semantic effects are applied (confirmed empirically:
 * reading the destination register from an instruction's own exec callback
 * observes its pre-execution value, not the result). So a write-back
 * result cannot be sampled from the instruction's own callback. Instead,
 * each write-back instruction's (class, dest-register) is queued as
 * "pending" and sampled from the *next* instruction's exec callback --
 * which fires before that next instruction runs, i.e. after the prior
 * (write-back) instruction has fully completed. Within a TB this is exact.
 * If a write-back instruction happens to be the very last instruction of
 * its translation block, there is no following instruction in the same TB
 * to piggyback on, and that one dynamic occurrence is dropped (rare;
 * accepted as measurement noise for this statistical study).
 *
 * For flag-only compare classes (CMP/CMN/TST/TEQ), there is no destination
 * register, so the plugin recomputes the discarded ALU result itself from
 * the operand values (register and/or decoded immediate), read at the
 * compare instruction's own (pre-its-own-execution) callback -- which is
 * exactly the point after all prior instructions that computed those
 * operands have completed, so no deferral is needed here.
 *
 * For CLS_STR, the value measured is the source register's content
 * (truncated to the store's access size), i.e. the payload actually
 * written to memory -- not the address. Same reasoning: the store hasn't
 * happened yet at its own pre-exec callback, but the register holding the
 * data to be stored was already set by a prior instruction, so no
 * deferral is needed.
 *
 * Effective bit width:
 *   - ALU classes: value reinterpreted as a signed 32-bit integer, then
 *     32 - clz(abs(value)), 0 maps to 0 (per CLAUDE.md's stated formula).
 *   - STR class: value masked to the access width, then treated as an
 *     unsigned bit pattern, 32 - clz(value), 0 maps to 0.
 *
 * Output: CSV of "class,bitwidth,count" written at exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <glib.h>
#include <qemu-plugin.h>

#include "decode.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NUM_ARM_REGS 16
#define MAX_BITS 32

static struct qemu_plugin_register *reg_handles[NUM_ARM_REGS];
static GByteArray *read_buf;
static uint64_t histogram[CLS_COUNT][MAX_BITS + 1];
static char *output_path = NULL;

static int effective_bits_signed(uint32_t v)
{
    int32_t sv = (int32_t)v;
    uint32_t av = (sv < 0) ? (uint32_t)(-(int64_t)sv) : (uint32_t)sv;
    if (av == 0) {
        return 0;
    }
    return 32 - __builtin_clz(av);
}

static int effective_bits_unsigned(uint32_t v)
{
    if (v == 0) {
        return 0;
    }
    return 32 - __builtin_clz(v);
}

static bool read_reg(int idx, uint32_t *out)
{
    if (idx < 0 || idx >= NUM_ARM_REGS || !reg_handles[idx]) {
        return false;
    }
    g_byte_array_set_size(read_buf, 0);
    int sz = qemu_plugin_read_register(reg_handles[idx], read_buf);
    if (sz < 4) {
        return false;
    }
    *out = (uint32_t)read_buf->data[0] | ((uint32_t)read_buf->data[1] << 8) |
           ((uint32_t)read_buf->data[2] << 16) | ((uint32_t)read_buf->data[3] << 24);
    return true;
}

static uint32_t compute_cmp_result(InsnClass cls, uint32_t a, uint32_t b)
{
    switch (cls) {
    case CLS_CMP:
        return a - b;
    case CLS_CMN:
        return a + b;
    case CLS_TST:
        return a & b;
    case CLS_TEQ:
        return a ^ b;
    default:
        return 0;
    }
}

typedef struct {
    InsnClass cls;
    int reg;
} PendingWriteback;

/* Sample a previous write-back instruction's destination register, from
 * the following instruction's pre-execution callback (see file header). */
static void cb_sample_writeback(unsigned int vcpu_index, void *udata)
{
    PendingWriteback *pw = (PendingWriteback *)udata;
    uint32_t val;
    if (read_reg(pw->reg, &val)) {
        histogram[pw->cls][effective_bits_signed(val)]++;
    }
}

/* Compare classes and stores: both read state set up by prior
 * instructions, so this instruction's own pre-execution callback is the
 * correct sampling point (see file header). */
static void cb_compare_or_store(unsigned int vcpu_index, void *udata)
{
    DecodedInsn *di = (DecodedInsn *)udata;
    uint32_t val;

    if (di->cls == CLS_STR) {
        if (!read_reg(di->reg_a, &val)) {
            return;
        }
        if (di->store_size < 4) {
            val &= (1u << (di->store_size * 8)) - 1;
        }
        histogram[CLS_STR][effective_bits_unsigned(val)]++;
        return;
    }

    /* Compare class: recompute the discarded ALU result. */
    uint32_t a, b;
    if (!read_reg(di->reg_a, &a)) {
        return;
    }
    if (di->reg_b >= 0) {
        if (!read_reg(di->reg_b, &b)) {
            return;
        }
    } else {
        b = di->imm_b;
    }
    uint32_t result = compute_cmp_result(di->cls, a, b);
    histogram[di->cls][effective_bits_signed(result)]++;
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    bool have_pending = false;
    InsnClass pending_cls = CLS_COUNT;
    int pending_reg = -1;

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        if (have_pending) {
            PendingWriteback *pw = g_new(PendingWriteback, 1);
            pw->cls = pending_cls;
            pw->reg = pending_reg;
            qemu_plugin_register_vcpu_insn_exec_cb(insn, cb_sample_writeback,
                                                    QEMU_PLUGIN_CB_R_REGS, pw);
            have_pending = false;
        }

        size_t size = qemu_plugin_insn_size(insn);
        if (size != 2 && size != 4) {
            continue;
        }
        const uint8_t *data = qemu_plugin_insn_data(insn);
        /* Instruction stream is little-endian; halfwords are stored
         * low-byte first regardless of overall size. */
        uint16_t hw1 = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        uint16_t hw2 = 0;
        if (size == 4) {
            hw2 = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
        }

        DecodedInsn di;
        if (!decode_insn(hw1, hw2, (int)size, &di)) {
            continue;
        }

        if (di.is_writeback) {
            have_pending = true;
            pending_cls = di.cls;
            pending_reg = di.reg_a;
        } else {
            DecodedInsn *udata = g_new(DecodedInsn, 1);
            *udata = di;
            qemu_plugin_register_vcpu_insn_exec_cb(insn, cb_compare_or_store,
                                                    QEMU_PLUGIN_CB_R_REGS, udata);
        }
    }
}

static void vcpu_init(qemu_plugin_id_t id, unsigned int vcpu_index)
{
    GArray *regs = qemu_plugin_get_registers();
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (strlen(d->name) >= 1 && d->name[0] == 'r') {
            int idx = atoi(d->name + 1);
            if (idx >= 0 && idx < NUM_ARM_REGS) {
                reg_handles[idx] = d->handle;
            }
        } else if (!strcmp(d->name, "sp")) {
            reg_handles[13] = d->handle;
        } else if (!strcmp(d->name, "lr")) {
            reg_handles[14] = d->handle;
        } else if (!strcmp(d->name, "pc")) {
            reg_handles[15] = d->handle;
        }
    }
    g_array_free(regs, TRUE);
}

static void dump_csv(void)
{
    FILE *f = fopen(output_path ? output_path : "bitwidth.csv", "w");
    if (!f) {
        return;
    }
    fprintf(f, "class,bitwidth,count\n");
    for (int c = 0; c < CLS_COUNT; c++) {
        for (int b = 0; b <= MAX_BITS; b++) {
            if (histogram[c][b] > 0) {
                fprintf(f, "%s,%d,%" PRIu64 "\n", insn_class_names[c], b, histogram[c][b]);
            }
        }
    }
    fclose(f);
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    dump_csv();
    g_byte_array_free(read_buf, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                            const qemu_info_t *info,
                                            int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "out=")) {
            output_path = g_strdup(argv[i] + 4);
        }
    }
    read_buf = g_byte_array_new();
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}

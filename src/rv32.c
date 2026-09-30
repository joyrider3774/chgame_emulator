/*
 * The QingKe V4C core: RV32IMAC plus WCH's XW compressed byte/halfword
 * loads and stores, the PFIC's vectored interrupts and the hardware
 * prologue/epilogue (HPE) that "WCH-Interrupt-fast" handlers rely on.
 *
 * Instructions are decoded once into ChgInsn entries, one per halfword of
 * flash and RAM, and executed from there. A store to RAM throws away the
 * entries it overlaps, so code copied into RAM (.data.hotcode) and later
 * rewritten is always seen as it is now; flash entries are dropped when the
 * flash controller writes a page.
 *
 * Timing. Every number below was measured on a CHGame with the loops in
 * tests/sketches/chg_cal (cycles per loop iteration, compared case by case):
 *
 *   The core executes an instruction a cycle; loads and stores take 2, from
 *   and to RAM or a peripheral alike, and a load's result is usable at once.
 *   Multiplies take 1. Divides run beside the pipeline for DIV_LATENCY
 *   cycles: only the next divide, or an instruction reading the result,
 *   waits for them.
 *
 *   Instructions come from a prefetcher that reads one 32-bit word at a
 *   time and runs at most PREFETCH_DEPTH words ahead: a word of flash takes
 *   FLASH_WORD cycles, a word of SRAM RAM_WORD. Code in flash is therefore
 *   fetch bound, whatever the instructions are. The SRAM has one port, which
 *   data accesses from RAM code take from the prefetcher: a load one cycle,
 *   a store two, and back to back stores go in pairs of 2 + 1. A load from
 *   flash takes the flash away from the prefetcher the same way.
 *
 *   A taken branch starts fetching at the target a cycle later, or once the
 *   word in flight is in, so a target in the upper half of a word costs a
 *   word more. In RAM code a load, or a store to SRAM, in the two
 *   instructions before the branch falls in that bubble and saves a cycle.
 *
 *   A load from flash made by RAM code costs 4 5/8 cycles, kept as eighths
 *   and carried over.
 *
 * With these rules the 51 loops of chg_cal come out within 0.5% of the chip
 * (0.03% on average).
 */
#include <string.h>
#include <stdio.h>
#include "machine.h"

#define FLASH_WORD      4       /* cycles to fetch a 32-bit word of flash */
#define RAM_WORD        1       /* ...of SRAM */
#define PREFETCH_DEPTH  2       /* words the prefetcher may be ahead */
#define LOAD_CYCLES     2
#define STORE_CYCLES    2
#define RAM_LOAD_PORT   1       /* SRAM port cycles a data access takes */
#define RAM_STORE_PORT  2
#define RAM_STORE2_PORT 1       /* ...a store straight after a store */
#define FLASH_LOAD_RAM8 37      /* eighths a flash load costs from RAM code, in all */
#define DIV_LATENCY     10

/* the prefetcher's word number for an address. Flash and SRAM share the
   numbering; a jump always restarts the prefetcher, so that is harmless */
#define WIDX(a) ((uint32_t)(((a) & 0xffffu) >> 2))

enum {
    OP_NONE = 0,
    OP_LUI, OP_AUIPC, OP_JAL, OP_JALR,
    OP_BEQ, OP_BNE, OP_BLT, OP_BGE, OP_BLTU, OP_BGEU,
    OP_LB, OP_LH, OP_LW, OP_LBU, OP_LHU,
    OP_SB, OP_SH, OP_SW,
    OP_ADDI, OP_SLTI, OP_SLTIU, OP_XORI, OP_ORI, OP_ANDI, OP_SLLI, OP_SRLI, OP_SRAI,
    OP_ADD, OP_SUB, OP_SLL, OP_SLT, OP_SLTU, OP_XOR, OP_SRL, OP_SRA, OP_OR, OP_AND,
    OP_MUL, OP_MULH, OP_MULHSU, OP_MULHU, OP_DIV, OP_DIVU, OP_REM, OP_REMU,
    OP_LR, OP_SC, OP_AMOSWAP, OP_AMOADD, OP_AMOXOR, OP_AMOAND, OP_AMOOR,
    OP_AMOMIN, OP_AMOMAX, OP_AMOMINU, OP_AMOMAXU,
    OP_FENCE, OP_ECALL, OP_EBREAK, OP_MRET, OP_WFI,
    OP_CSRRW, OP_CSRRS, OP_CSRRC, OP_CSRRWI, OP_CSRRSI, OP_CSRRCI,
    OP_ILLEGAL,
};

const char *cpu_op_name(int op)
{
    static const char *names[] = {
        "?", "lui", "auipc", "jal", "jalr", "beq", "bne", "blt", "bge", "bltu", "bgeu",
        "lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw",
        "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
        "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
        "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu",
        "lr.w", "sc.w", "amoswap.w", "amoadd.w", "amoxor.w", "amoand.w", "amoor.w",
        "amomin.w", "amomax.w", "amominu.w", "amomaxu.w",
        "fence", "ecall", "ebreak", "mret", "wfi",
        "csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci", "illegal",
    };
    return (op >= 0 && op < (int)(sizeof(names) / sizeof(names[0]))) ? names[op] : "?";
}

/* mcause values */
#define CAUSE_MISALIGNED_FETCH  0
#define CAUSE_FETCH_FAULT       1
#define CAUSE_ILLEGAL           2
#define CAUSE_BREAKPOINT        3
#define CAUSE_LOAD_FAULT        5
#define CAUSE_STORE_FAULT       7
#define CAUSE_ECALL_U           8
#define CAUSE_ECALL_M           11

#define MSTATUS_MIE   0x00000008u
#define MSTATUS_MPIE  0x00000080u
#define MSTATUS_MPP   0x00001800u

/* ------------------------------------------------------------------------ */
/* Decoding                                                                  */
/* ------------------------------------------------------------------------ */

static inline int32_t sext(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

#define BITS(v, hi, lo) (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define RD(r) ((r) == 0 ? 32 : (r))

static void set(ChgInsn *d, int op, int rd, int rs1, int rs2, int32_t imm)
{
    d->op = (uint8_t)op;
    d->rd = (uint8_t)RD(rd);
    d->rs1 = (uint8_t)rs1;
    d->rs2 = (uint8_t)rs2;
    d->imm = imm;
}

static void decode32(uint32_t w, ChgInsn *d)
{
    const int rd = BITS(w, 11, 7), rs1 = BITS(w, 19, 15), rs2 = BITS(w, 24, 20);
    const int f3 = BITS(w, 14, 12), f7 = BITS(w, 31, 25);
    const int32_t immI = (int32_t)w >> 20;
    const int32_t immS = ((int32_t)w >> 25 << 5) | BITS(w, 11, 7);
    const int32_t immB = sext((BITS(w, 31, 31) << 12) | (BITS(w, 7, 7) << 11) |
                              (BITS(w, 30, 25) << 5) | (BITS(w, 11, 8) << 1), 13);
    const int32_t immJ = sext((BITS(w, 31, 31) << 20) | (BITS(w, 19, 12) << 12) |
                              (BITS(w, 20, 20) << 11) | (BITS(w, 30, 21) << 1), 21);

    set(d, OP_ILLEGAL, 0, 0, 0, (int32_t)w);
    switch (w & 0x7f) {
    case 0x37: set(d, OP_LUI, rd, 0, 0, (int32_t)(w & 0xfffff000u)); break;
    case 0x17: set(d, OP_AUIPC, rd, 0, 0, (int32_t)(w & 0xfffff000u)); break;
    case 0x6f: set(d, OP_JAL, rd, 0, 0, immJ); break;
    case 0x67: if (f3 == 0) set(d, OP_JALR, rd, rs1, 0, immI); break;
    case 0x63: {
        static const int ops[8] = { OP_BEQ, OP_BNE, 0, 0, OP_BLT, OP_BGE, OP_BLTU, OP_BGEU };
        if (ops[f3]) set(d, ops[f3], 0, rs1, rs2, immB);
        break;
    }
    case 0x03: {
        static const int ops[8] = { OP_LB, OP_LH, OP_LW, 0, OP_LBU, OP_LHU, 0, 0 };
        if (ops[f3]) set(d, ops[f3], rd, rs1, 0, immI);
        break;
    }
    case 0x23: {
        static const int ops[8] = { OP_SB, OP_SH, OP_SW, 0, 0, 0, 0, 0 };
        if (ops[f3]) set(d, ops[f3], 0, rs1, rs2, immS);
        break;
    }
    case 0x13:
        switch (f3) {
        case 0: set(d, OP_ADDI, rd, rs1, 0, immI); break;
        case 2: set(d, OP_SLTI, rd, rs1, 0, immI); break;
        case 3: set(d, OP_SLTIU, rd, rs1, 0, immI); break;
        case 4: set(d, OP_XORI, rd, rs1, 0, immI); break;
        case 6: set(d, OP_ORI, rd, rs1, 0, immI); break;
        case 7: set(d, OP_ANDI, rd, rs1, 0, immI); break;
        case 1: if (f7 == 0) set(d, OP_SLLI, rd, rs1, 0, rs2); break;
        case 5:
            if (f7 == 0) set(d, OP_SRLI, rd, rs1, 0, rs2);
            else if (f7 == 0x20) set(d, OP_SRAI, rd, rs1, 0, rs2);
            break;
        }
        break;
    case 0x33:
        if (f7 == 0) {
            static const int ops[8] = { OP_ADD, OP_SLL, OP_SLT, OP_SLTU, OP_XOR, OP_SRL, OP_OR, OP_AND };
            set(d, ops[f3], rd, rs1, rs2, 0);
        } else if (f7 == 0x20) {
            if (f3 == 0) set(d, OP_SUB, rd, rs1, rs2, 0);
            else if (f3 == 5) set(d, OP_SRA, rd, rs1, rs2, 0);
        } else if (f7 == 1) {
            static const int ops[8] = { OP_MUL, OP_MULH, OP_MULHSU, OP_MULHU, OP_DIV, OP_DIVU, OP_REM, OP_REMU };
            set(d, ops[f3], rd, rs1, rs2, 0);
        }
        break;
    case 0x2f:
        if (f3 == 2) {
            switch (f7 >> 2) {
            case 0x02: if (rs2 == 0) set(d, OP_LR, rd, rs1, 0, 0); break;
            case 0x03: set(d, OP_SC, rd, rs1, rs2, 0); break;
            case 0x01: set(d, OP_AMOSWAP, rd, rs1, rs2, 0); break;
            case 0x00: set(d, OP_AMOADD, rd, rs1, rs2, 0); break;
            case 0x04: set(d, OP_AMOXOR, rd, rs1, rs2, 0); break;
            case 0x0c: set(d, OP_AMOAND, rd, rs1, rs2, 0); break;
            case 0x08: set(d, OP_AMOOR, rd, rs1, rs2, 0); break;
            case 0x10: set(d, OP_AMOMIN, rd, rs1, rs2, 0); break;
            case 0x14: set(d, OP_AMOMAX, rd, rs1, rs2, 0); break;
            case 0x18: set(d, OP_AMOMINU, rd, rs1, rs2, 0); break;
            case 0x1c: set(d, OP_AMOMAXU, rd, rs1, rs2, 0); break;
            }
        }
        break;
    case 0x0f: set(d, OP_FENCE, 0, 0, 0, 0); break;
    case 0x73:
        if (f3 == 0) {
            if (w == 0x00000073) set(d, OP_ECALL, 0, 0, 0, 0);
            else if (w == 0x00100073) set(d, OP_EBREAK, 0, 0, 0, 0);
            else if (w == 0x30200073) set(d, OP_MRET, 0, 0, 0, 0);
            else if (w == 0x10500073) set(d, OP_WFI, 0, 0, 0, 0);
        } else if (f3 != 4) {
            static const int ops[8] = { 0, OP_CSRRW, OP_CSRRS, OP_CSRRC, 0, OP_CSRRWI, OP_CSRRSI, OP_CSRRCI };
            set(d, ops[f3], rd, rs1, 0, (int32_t)BITS(w, 31, 20));
        }
        break;
    }
}

/* 16-bit instructions, expanded into the 32-bit operations they stand for */
static void decode16(uint32_t h, ChgInsn *d)
{
    const int f3 = BITS(h, 15, 13);
    const int rdp = 8 + BITS(h, 4, 2);      /* rd'/rs2' */
    const int rs1p = 8 + BITS(h, 9, 7);     /* rs1'/rd' */
    const int r = BITS(h, 11, 7);           /* full rd/rs1 */
    const int r2 = BITS(h, 6, 2);           /* full rs2 */

    set(d, OP_ILLEGAL, 0, 0, 0, (int32_t)h);
    switch (h & 3) {
    case 0:
        switch (f3) {
        case 0: {   /* c.addi4spn */
            uint32_t imm = (BITS(h, 10, 7) << 6) | (BITS(h, 12, 11) << 4) |
                           (BITS(h, 5, 5) << 3) | (BITS(h, 6, 6) << 2);
            if (imm) set(d, OP_ADDI, rdp, 2, 0, (int32_t)imm);
            break;
        }
        case 1: {   /* XW c.lbu: off[0]=12 off[1]=5 off[2]=6 off[4:3]=11:10 */
            uint32_t off = BITS(h, 12, 12) | (BITS(h, 5, 5) << 1) | (BITS(h, 6, 6) << 2) |
                           (BITS(h, 11, 10) << 3);
            set(d, OP_LBU, rdp, rs1p, 0, (int32_t)off);
            break;
        }
        case 2: {   /* c.lw */
            uint32_t off = (BITS(h, 12, 10) << 3) | (BITS(h, 6, 6) << 2) | (BITS(h, 5, 5) << 6);
            set(d, OP_LW, rdp, rs1p, 0, (int32_t)off);
            break;
        }
        case 4: {   /* XW sp relative byte/halfword, the sub-op in bits 6:5 */
            if (BITS(h, 12, 11) != 0)
                break;
            const uint32_t boff = BITS(h, 10, 7);
            const uint32_t hoff = (BITS(h, 10, 8) << 1) | (BITS(h, 7, 7) << 4);
            switch (BITS(h, 6, 5)) {
            case 0: set(d, OP_LBU, rdp, 2, 0, (int32_t)boff); break;       /* c.lbusp */
            case 1: set(d, OP_LHU, rdp, 2, 0, (int32_t)hoff); break;       /* c.lhusp */
            case 2: set(d, OP_SB, 0, 2, rdp, (int32_t)boff); break;        /* c.sbsp */
            case 3: set(d, OP_SH, 0, 2, rdp, (int32_t)hoff); break;        /* c.shsp */
            }
            break;
        }
        case 5: {   /* XW c.sb */
            uint32_t off = BITS(h, 12, 12) | (BITS(h, 5, 5) << 1) | (BITS(h, 6, 6) << 2) |
                           (BITS(h, 11, 10) << 3);
            set(d, OP_SB, 0, rs1p, rdp, (int32_t)off);
            break;
        }
        case 6: {   /* c.sw */
            uint32_t off = (BITS(h, 12, 10) << 3) | (BITS(h, 6, 6) << 2) | (BITS(h, 5, 5) << 6);
            set(d, OP_SW, 0, rs1p, rdp, (int32_t)off);
            break;
        }
        }
        break;

    case 1: {
        const int32_t imm6 = sext((BITS(h, 12, 12) << 5) | BITS(h, 6, 2), 6);
        switch (f3) {
        case 0: set(d, OP_ADDI, r, r, 0, imm6); break;          /* c.addi, c.nop */
        case 1:                                                 /* c.jal */
        case 5: {                                               /* c.j */
            int32_t off = sext((BITS(h, 12, 12) << 11) | (BITS(h, 8, 8) << 10) |
                               (BITS(h, 10, 9) << 8) | (BITS(h, 6, 6) << 7) |
                               (BITS(h, 7, 7) << 6) | (BITS(h, 2, 2) << 5) |
                               (BITS(h, 11, 11) << 4) | (BITS(h, 5, 3) << 1), 12);
            set(d, OP_JAL, f3 == 1 ? 1 : 0, 0, 0, off);
            break;
        }
        case 2: set(d, OP_ADDI, r, 0, 0, imm6); break;          /* c.li */
        case 3:
            if (r == 2) {                                       /* c.addi16sp */
                int32_t imm = sext((BITS(h, 12, 12) << 9) | (BITS(h, 4, 3) << 7) |
                                   (BITS(h, 5, 5) << 6) | (BITS(h, 2, 2) << 5) |
                                   (BITS(h, 6, 6) << 4), 10);
                if (imm) set(d, OP_ADDI, 2, 2, 0, imm);
            } else if (imm6) {                                  /* c.lui */
                set(d, OP_LUI, r, 0, 0, (int32_t)((uint32_t)imm6 << 12));
            }
            break;
        case 4: {
            const uint32_t sh = BITS(h, 6, 2) | (BITS(h, 12, 12) << 5);
            switch (BITS(h, 11, 10)) {
            case 0: if (sh < 32) set(d, OP_SRLI, rs1p, rs1p, 0, (int32_t)sh); break;
            case 1: if (sh < 32) set(d, OP_SRAI, rs1p, rs1p, 0, (int32_t)sh); break;
            case 2: set(d, OP_ANDI, rs1p, rs1p, 0, imm6); break;
            case 3:
                if (BITS(h, 12, 12) == 0) {
                    static const int ops[4] = { OP_SUB, OP_XOR, OP_OR, OP_AND };
                    set(d, ops[BITS(h, 6, 5)], rs1p, rs1p, rdp, 0);
                }
                break;
            }
            break;
        }
        case 6:
        case 7: {                                               /* c.beqz, c.bnez */
            int32_t off = sext((BITS(h, 12, 12) << 8) | (BITS(h, 6, 5) << 6) |
                               (BITS(h, 2, 2) << 5) | (BITS(h, 11, 10) << 3) |
                               (BITS(h, 4, 3) << 1), 9);
            set(d, f3 == 6 ? OP_BEQ : OP_BNE, 0, rs1p, 0, off);
            break;
        }
        }
        break;
    }

    case 2:
        switch (f3) {
        case 0: {                                               /* c.slli */
            const uint32_t sh = BITS(h, 6, 2) | (BITS(h, 12, 12) << 5);
            if (sh < 32) set(d, OP_SLLI, r, r, 0, (int32_t)sh);
            break;
        }
        case 1: {   /* XW c.lhu: off[1]=5 off[2]=6 off[4:3]=11:10 off[5]=12 */
            uint32_t off = (BITS(h, 5, 5) << 1) | (BITS(h, 6, 6) << 2) |
                           (BITS(h, 11, 10) << 3) | (BITS(h, 12, 12) << 5);
            set(d, OP_LHU, rdp, rs1p, 0, (int32_t)off);
            break;
        }
        case 2: {                                               /* c.lwsp */
            uint32_t off = (BITS(h, 12, 12) << 5) | (BITS(h, 6, 4) << 2) | (BITS(h, 3, 2) << 6);
            if (r) set(d, OP_LW, r, 2, 0, (int32_t)off);
            break;
        }
        case 4:
            if (BITS(h, 12, 12) == 0) {
                if (r2 == 0) { if (r) set(d, OP_JALR, 0, r, 0, 0); }      /* c.jr */
                else set(d, OP_ADD, r, 0, r2, 0);                          /* c.mv */
            } else {
                if (r == 0 && r2 == 0) set(d, OP_EBREAK, 0, 0, 0, 0);      /* c.ebreak */
                else if (r2 == 0) set(d, OP_JALR, 1, r, 0, 0);             /* c.jalr */
                else set(d, OP_ADD, r, r, r2, 0);                          /* c.add */
            }
            break;
        case 5: {   /* XW c.sh */
            uint32_t off = (BITS(h, 5, 5) << 1) | (BITS(h, 6, 6) << 2) |
                           (BITS(h, 11, 10) << 3) | (BITS(h, 12, 12) << 5);
            set(d, OP_SH, 0, rs1p, rdp, (int32_t)off);
            break;
        }
        case 6: {                                               /* c.swsp */
            uint32_t off = (BITS(h, 12, 9) << 2) | (BITS(h, 8, 7) << 6);
            set(d, OP_SW, 0, 2, r2, (int32_t)off);
            break;
        }
        }
        break;
    }
}

/* Decodes the instruction at pc whose first 32 bits are raw. Returns its
   length. cost is what executing it takes once it has been fetched; the
   fetch itself is worked out as it runs, see cpu_exec. */
int cpu_decode(uint32_t pc, uint32_t raw, ChgInsn *d, bool in_flash)
{
    (void)in_flash;
    if ((raw & 3) == 3) {
        decode32(raw, d);
        d->len = 4;
    } else {
        decode16(raw & 0xffff, d);
        d->len = 2;
    }
    int cost = 1;
    switch (d->op) {
    case OP_LB: case OP_LH: case OP_LW: case OP_LBU: case OP_LHU: case OP_LR:
        cost = LOAD_CYCLES;
        break;
    case OP_SB: case OP_SH: case OP_SW:
        cost = STORE_CYCLES;
        break;
    case OP_AMOSWAP: case OP_AMOADD: case OP_AMOXOR: case OP_AMOAND: case OP_AMOOR:
    case OP_AMOMIN: case OP_AMOMAX: case OP_AMOMINU: case OP_AMOMAXU: case OP_SC:
        cost = LOAD_CYCLES + STORE_CYCLES;
        break;
    }
    d->cost = (uint8_t)cost;
    d->wend = (uint16_t)(((pc & 0xffff) + d->len - 1) >> 2);
    return d->len;
}

/* ------------------------------------------------------------------------ */
/* Memory                                                                    */
/* ------------------------------------------------------------------------ */

static inline void ram_invalidate(ChgMachine *m, uint32_t off, int size)
{
    /* the entries starting in the bytes written, and the one before them,
       whose 32-bit instruction may reach into them */
    uint32_t first = off >> 1;
    uint32_t last = (off + (uint32_t)size - 1) >> 1;
    if (first) first--;
    for (uint32_t i = first; i <= last && i < CHG_RAM_SIZE / 2; i++)
        m->dc_ram[i].op = OP_NONE;
}

void cpu_invalidate_flash(ChgMachine *m, uint32_t addr, uint32_t len)
{
    uint32_t first = addr >> 1, last = (addr + len - 1) >> 1;
    if (first) first--;
    for (uint32_t i = first; i <= last && i < CHG_FLASH_SIZE / 2; i++)
        m->dc_flash[i].op = OP_NONE;
}

/* 'wait' collects extra cycles from the bus in its low bits, and these flags
   for costs that depend on where the code runs (see cpu_exec) */
#define WAIT_FLASH_DATA 0x10000
#define WAIT_RAM_STORE  0x20000
#define WAIT_RAM_LOAD   0x40000
#define WAIT_LOAD       0x80000     /* any load at all */
#define WAIT_CYCLES     0x0ffff

static inline uint32_t load(ChgMachine *m, uint32_t addr, int size, int *wait)
{
    uint32_t off = addr - CHG_RAM_BASE;
    if (off <= CHG_RAM_SIZE - (uint32_t)size) {
        const uint8_t *p = m->ram + off;
        *wait |= WAIT_RAM_LOAD;
        if (size == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
        if (size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
        return *p;
    }
    if (addr <= CHG_FLASH_SIZE - (uint32_t)size) {
        const uint8_t *p = m->flash + addr;
        *wait |= WAIT_FLASH_DATA;
        if (size == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
        if (size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
        return *p;
    }
    *wait |= WAIT_LOAD;
    return bus_read_slow(m, addr, size, wait);
}

static inline void store(ChgMachine *m, uint32_t addr, uint32_t v, int size, int *wait)
{
    uint32_t off = addr - CHG_RAM_BASE;
    if (off <= CHG_RAM_SIZE - (uint32_t)size) {
        uint8_t *p = m->ram + off;
        if (size == 4) memcpy(p, &v, 4);
        else if (size == 2) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }
        else *p = (uint8_t)v;
        ram_invalidate(m, off, size);
        *wait |= WAIT_RAM_STORE;
        return;
    }
    bus_write_slow(m, addr, v, size, wait);
}

/* ------------------------------------------------------------------------ */
/* Traps                                                                     */
/* ------------------------------------------------------------------------ */

static const uint8_t hpe_regs[16] = { 1, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31 };

/* Enters trap handler 'vector'. irq is the interrupt number, or -1 for an
   exception. Returns the handler's address. */
static uint32_t trap_enter(ChgMachine *m, uint32_t pc, uint32_t cause, uint32_t tval, int vector, int irq)
{
    ChgCpu *c = &m->cpu;
    const int depth = c->trap_depth < CHG_HPE_DEPTH ? c->trap_depth : CHG_HPE_DEPTH - 1;
    /* The core keeps what this trap replaces, so that a preempted handler
       still finds its own mepc when the interrupt that preempted it returns:
       WCH-Interrupt-fast handlers never save mepc themselves */
    c->trap_mepc[depth] = c->mepc;
    c->trap_mcause[depth] = c->mcause;
    c->trap_mstatus[depth] = c->mstatus;
    c->mepc = pc;
    c->mcause = cause;
    c->mtval = tval;

    /* With nesting enabled the core does not drop MIE on an interrupt: the
       PFIC's priorities decide what may preempt, which is why the core's own
       SysTick handler turns interrupts off itself around its critical part. */
    const bool nest = (c->intsyscr & 2) != 0 && irq >= 0;
    c->mstatus = (c->mstatus & ~(MSTATUS_MPIE | MSTATUS_MPP)) |
                 ((c->mstatus & MSTATUS_MIE) ? MSTATUS_MPIE : 0) | MSTATUS_MPP;
    if (!nest)
        c->mstatus &= ~MSTATUS_MIE;

    c->trap_hpe[depth] = (c->intsyscr & 1) != 0;
    c->trap_irq[depth] = irq;
    if (c->trap_hpe[depth])
        for (int i = 0; i < 16; i++)
            c->hpe[depth][i] = c->x[hpe_regs[i]];
    if (c->trap_depth < CHG_HPE_DEPTH)
        c->trap_depth++;
    c->wfi = false;

    const uint32_t base = c->mtvec & ~3u;
    if (!(c->mtvec & 1))
        return base;
    if (c->mtvec & 2) {
        int wait = 0;
        return load(m, base + 4u * (uint32_t)vector, 4, &wait) & ~1u;
    }
    return base + 4u * (uint32_t)vector;
}

static uint32_t exception(ChgMachine *m, uint32_t pc, uint32_t cause, uint32_t tval)
{
    ChgCpu *c = &m->cpu;
    int vector = 3;     /* HardFault */
    if (cause == CAUSE_ECALL_M) vector = 5;
    else if (cause == CAUSE_ECALL_U) vector = 8;
    else if (cause == CAUSE_BREAKPOINT) vector = 9;
    if (vector == 3 && !c->faulted) {
        c->faulted = true;
        c->fault_cause = cause;
        c->fault_pc = pc;
        c->fault_tval = tval;
        fprintf(stderr, "CPU fault: mcause %u at pc %08x, mtval %08x\n", cause, pc, tval);
    }
    return trap_enter(m, pc, cause, tval, vector, -1);
}

static uint32_t mret(ChgMachine *m)
{
    ChgCpu *c = &m->cpu;
    const uint32_t target = c->mepc;
    if (c->trap_depth > 0) {
        c->trap_depth--;
        const int d = c->trap_depth;
        if (c->trap_hpe[d])
            for (int i = 0; i < 16; i++)
                c->x[hpe_regs[i]] = c->hpe[d][i];
        /* back to what the trap interrupted, handler or not */
        c->mstatus = (c->trap_mstatus[d] & ~MSTATUS_MIE) |
                     ((c->mstatus & MSTATUS_MPIE) ? MSTATUS_MIE : 0);
        c->mepc = c->trap_mepc[d];
        c->mcause = c->trap_mcause[d];
    } else {
        /* an mret with no trap to return from, e.g. startup's jump to main */
        c->mstatus = (c->mstatus & ~(MSTATUS_MIE | MSTATUS_MPP)) |
                     ((c->mstatus & MSTATUS_MPIE) ? MSTATUS_MIE : 0) | MSTATUS_MPIE;
    }
    chg_kick(m);
    return target;
}

/* The interrupt the PFIC would hand the core now, or -1 */
static int pick_irq(ChgMachine *m)
{
    ChgCpu *c = &m->cpu;
    ChgPfic *p = &m->pfic;
    uint64_t ready = (p->pending | p->line) & p->enabled;
    if (!ready)
        return -1;

    /* What is running already, and so how urgent something must be to preempt it */
    int cur = 256;
    for (int d = 0; d < c->trap_depth; d++) {
        if (c->trap_irq[d] < 0) { cur = -1; break; }   /* no interrupts inside a fault handler */
        int pr = p->prio[c->trap_irq[d]];
        if (pr < cur) cur = pr;
    }
    if (cur < 0)
        return -1;

    int best = -1, bestPrio = 256;
    for (int i = 0; i < CHG_IRQ_COUNT; i++) {
        if (!(ready >> i & 1))
            continue;
        const int pr = p->prio[i];
        if (p->ithresdr && pr >= (int)(p->ithresdr & 0xff))
            continue;
        if (pr < bestPrio) { best = i; bestPrio = pr; }
    }
    if (best < 0)
        return -1;
    if (c->trap_depth > 0) {
        /* only a higher preemption level (the top priority bit) gets in */
        if (!(c->intsyscr & 2) || (bestPrio >> 7) >= (cur >> 7))
            return -1;
    }
    return best;
}

bool cpu_irq_waiting(ChgMachine *m)
{
    ChgPfic *p = &m->pfic;
    return ((p->pending | p->line) & p->enabled) != 0;
}

bool cpu_take_interrupt(ChgMachine *m)
{
    ChgCpu *c = &m->cpu;
    if (!(c->mstatus & MSTATUS_MIE)) {
        /* a pending interrupt still ends WFI, it just is not taken */
        if (c->wfi && cpu_irq_waiting(m))
            c->wfi = false;
        return false;
    }
    const int irq = pick_irq(m);
    if (irq < 0)
        return false;
    m->pfic.pending &= ~(1ull << irq);
    c->pc = trap_enter(m, c->pc, 0x80000000u | (uint32_t)irq, 0, irq, irq);
    /* the vector is read from the table in flash, then fetching starts over */
    m->cycles += 1 + FLASH_WORD;
    c->fetch_word = WIDX(c->pc) - 1;
    c->fetch_time = m->cycles;
    return true;
}

/* ------------------------------------------------------------------------ */
/* CSRs                                                                      */
/* ------------------------------------------------------------------------ */

static uint32_t csr_read(ChgMachine *m, uint32_t csr, bool *ok)
{
    ChgCpu *c = &m->cpu;
    *ok = true;
    switch (csr) {
    case 0x300: return c->mstatus;
    case 0x301: return 0x40901105u;     /* RV32IMAC + X */
    case 0x305: return c->mtvec;
    case 0x340: return c->mscratch;
    case 0x341: return c->mepc;
    case 0x342: return c->mcause;
    case 0x343: return c->mtval;
    case 0x800: return c->mstatus & (MSTATUS_MIE | MSTATUS_MPIE);
    case 0x804: return c->intsyscr;
    case 0xBC0: return c->corecfgr;
    case 0xF11: return 0x00000489u;     /* WCH's JEDEC id */
    case 0xF12: return 0xDC68D841u;     /* QingKe V4C, as the manual gives marchid */
    case 0xF13: return 0;
    case 0xF14: return 0;
    case 0xB00: case 0xC00: return (uint32_t)m->cycles;
    case 0xB80: case 0xC80: return (uint32_t)(m->cycles >> 32);
    }
    if (csr >= 0x7A0 && csr <= 0x7AF) return 0;   /* debug triggers */
    return c->csr_misc[csr & 15];
}

static void csr_write(ChgMachine *m, uint32_t csr, uint32_t v)
{
    ChgCpu *c = &m->cpu;
    switch (csr) {
    case 0x300: c->mstatus = v & 0x00001888u; chg_kick(m); return;
    case 0x305: c->mtvec = v; return;
    case 0x340: c->mscratch = v; return;
    case 0x341: c->mepc = v & ~1u; return;
    case 0x342: c->mcause = v; return;
    case 0x343: c->mtval = v; return;
    case 0x800:
        c->mstatus = (c->mstatus & ~(MSTATUS_MIE | MSTATUS_MPIE)) | (v & (MSTATUS_MIE | MSTATUS_MPIE));
        chg_kick(m);
        return;
    case 0x804: c->intsyscr = v; return;
    case 0xBC0: c->corecfgr = v; return;
    }
    c->csr_misc[csr & 15] = v;
}

/* ------------------------------------------------------------------------ */
/* Execution                                                                 */
/* ------------------------------------------------------------------------ */

void cpu_reset(ChgMachine *m)
{
    ChgCpu *c = &m->cpu;
    memset(c, 0, sizeof(*c));
    c->pc = m->entry;
    c->mstatus = MSTATUS_MPP;
    c->fetch_word = WIDX(m->entry) - 1;
    c->fetch_time = m->cycles;
    memset(m->dc_ram, 0, sizeof(m->dc_ram));
    memset(m->dc_flash, 0, sizeof(m->dc_flash));
}

static ChgInsn *fetch(ChgMachine *m, uint32_t pc, ChgInsn *tmp)
{
    ChgInsn *d;
    uint32_t raw;
    uint32_t off = pc - CHG_RAM_BASE;
    if (pc < CHG_FLASH_SIZE - 2) {
        d = &m->dc_flash[pc >> 1];
        if (d->op) return d;
        memcpy(&raw, m->flash + pc, 4);
        cpu_decode(pc, raw, d, true);
        return d;
    }
    if (off < CHG_RAM_SIZE - 2) {
        d = &m->dc_ram[off >> 1];
        if (d->op) return d;
        memcpy(&raw, m->ram + off, 4);
        cpu_decode(pc, raw, d, false);
        return d;
    }
    /* anywhere else (the 0x08000000 alias, the last halfword of a region):
       decoded every time, which is fine for how rarely it happens */
    int wait = 0;
    raw = bus_read_slow(m, pc, 2, &wait);
    if ((raw & 3) == 3)
        raw |= bus_read_slow(m, pc + 2, 2, &wait) << 16;
    cpu_decode(pc, raw, tmp, false);
    tmp->cost = (uint8_t)(tmp->cost + wait);
    return tmp;
}

void cpu_exec(ChgMachine *m)
{
    ChgCpu *c = &m->cpu;
    uint32_t *x = c->x;
    uint32_t pc = c->pc;
    ChgInsn tmp;
    uint64_t retired = 0;

    /* the timing state lives in locals while running */
    uint32_t fw = c->fetch_word;
    uint64_t ft = c->fetch_time;
    uint64_t div_ready = c->div_ready;
    unsigned div_rd = c->div_rd;
    unsigned frac = c->frac8;
    bool pair_open = false;     /* the last instruction was a RAM store that opened a pair */
    unsigned recent_data = 0;   /* bit n: the instruction n back touched SRAM, from RAM code */

    while (m->cycles < m->stop_at) {
        retired++;
        const ChgInsn *d = fetch(m, pc, &tmp);
        const bool in_flash = pc < CHG_FLASH_SIZE;
        const uint32_t word = in_flash ? FLASH_WORD : RAM_WORD;

        /* The instruction waits for its words. The prefetcher delivers one
           every 'word' cycles but may not run more than PREFETCH_DEPTH words
           ahead, so after the core has been busy the words it buffered are
           there at once and the rest follow at the memory's pace */
        {
            const uint32_t w = d->wend;
            while ((int32_t)(w - fw) > 0) {
                fw++;
                ft += word;
                if (ft + word * (PREFETCH_DEPTH - 1) < m->cycles)
                    ft = m->cycles - word * (PREFETCH_DEPTH - 1);
            }
            if (ft > m->cycles)
                m->cycles = ft;
        }
        /* reading a register the divider has not delivered yet */
        if (div_ready > m->cycles && (d->rs1 == div_rd || d->rs2 == div_rd))
            m->cycles = div_ready;

        const uint64_t issue = m->cycles;
        uint32_t npc = pc + d->len;
        int cost = d->cost;
        int wait = 0;
        bool taken = false;
        const uint32_t a = x[d->rs1], b = x[d->rs2];
        const int32_t imm = d->imm;

        switch (d->op) {
        case OP_LUI:   x[d->rd] = (uint32_t)imm; break;
        case OP_AUIPC: x[d->rd] = pc + (uint32_t)imm; break;
        case OP_JAL:
            x[d->rd] = npc;
            npc = pc + (uint32_t)imm;
            taken = true;
            break;
        case OP_JALR:
            npc = (a + (uint32_t)imm) & ~1u;
            x[d->rd] = pc + d->len;
            taken = true;
            break;

#define BRANCH(cond) if (cond) { npc = pc + (uint32_t)imm; taken = true; } break
        case OP_BEQ:  BRANCH(a == b);
        case OP_BNE:  BRANCH(a != b);
        case OP_BLT:  BRANCH((int32_t)a < (int32_t)b);
        case OP_BGE:  BRANCH((int32_t)a >= (int32_t)b);
        case OP_BLTU: BRANCH(a < b);
        case OP_BGEU: BRANCH(a >= b);
#undef BRANCH

        case OP_LB:  c->pc = pc; x[d->rd] = (uint32_t)(int8_t)load(m, a + (uint32_t)imm, 1, &wait); break;
        case OP_LH:  c->pc = pc; x[d->rd] = (uint32_t)(int16_t)load(m, a + (uint32_t)imm, 2, &wait); break;
        case OP_LW:  c->pc = pc; x[d->rd] = load(m, a + (uint32_t)imm, 4, &wait); break;
        case OP_LBU: c->pc = pc; x[d->rd] = load(m, a + (uint32_t)imm, 1, &wait); break;
        case OP_LHU: c->pc = pc; x[d->rd] = load(m, a + (uint32_t)imm, 2, &wait); break;
        case OP_SB:  c->pc = pc; store(m, a + (uint32_t)imm, b, 1, &wait); break;
        case OP_SH:  c->pc = pc; store(m, a + (uint32_t)imm, b, 2, &wait); break;
        case OP_SW:  c->pc = pc; store(m, a + (uint32_t)imm, b, 4, &wait); break;

        case OP_ADDI:  x[d->rd] = a + (uint32_t)imm; break;
        case OP_SLTI:  x[d->rd] = (int32_t)a < imm; break;
        case OP_SLTIU: x[d->rd] = a < (uint32_t)imm; break;
        case OP_XORI:  x[d->rd] = a ^ (uint32_t)imm; break;
        case OP_ORI:   x[d->rd] = a | (uint32_t)imm; break;
        case OP_ANDI:  x[d->rd] = a & (uint32_t)imm; break;
        case OP_SLLI:  x[d->rd] = a << imm; break;
        case OP_SRLI:  x[d->rd] = a >> imm; break;
        case OP_SRAI:  x[d->rd] = (uint32_t)((int32_t)a >> imm); break;

        case OP_ADD:  x[d->rd] = a + b; break;
        case OP_SUB:  x[d->rd] = a - b; break;
        case OP_SLL:  x[d->rd] = a << (b & 31); break;
        case OP_SLT:  x[d->rd] = (int32_t)a < (int32_t)b; break;
        case OP_SLTU: x[d->rd] = a < b; break;
        case OP_XOR:  x[d->rd] = a ^ b; break;
        case OP_SRL:  x[d->rd] = a >> (b & 31); break;
        case OP_SRA:  x[d->rd] = (uint32_t)((int32_t)a >> (b & 31)); break;
        case OP_OR:   x[d->rd] = a | b; break;
        case OP_AND:  x[d->rd] = a & b; break;

        case OP_MUL:    x[d->rd] = a * b; break;
        case OP_MULH:   x[d->rd] = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
        case OP_MULHSU: x[d->rd] = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
        case OP_MULHU:  x[d->rd] = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
#define DIVIDER() do { \
            if (m->cycles < div_ready) m->cycles = div_ready; \
            div_ready = m->cycles + DIV_LATENCY; \
            div_rd = d->rd; \
        } while (0)
        case OP_DIV:
            DIVIDER();
            if (b == 0) x[d->rd] = 0xffffffffu;
            else if (a == 0x80000000u && b == 0xffffffffu) x[d->rd] = a;
            else x[d->rd] = (uint32_t)((int32_t)a / (int32_t)b);
            break;
        case OP_DIVU: DIVIDER(); x[d->rd] = b ? a / b : 0xffffffffu; break;
        case OP_REM:
            DIVIDER();
            if (b == 0) x[d->rd] = a;
            else if (a == 0x80000000u && b == 0xffffffffu) x[d->rd] = 0;
            else x[d->rd] = (uint32_t)((int32_t)a % (int32_t)b);
            break;
        case OP_REMU: DIVIDER(); x[d->rd] = b ? a % b : a; break;
#undef DIVIDER

        case OP_LR:
            c->pc = pc;
            x[d->rd] = load(m, a, 4, &wait);
            c->lr_valid = true;
            c->lr_addr = a;
            break;
        case OP_SC:
            c->pc = pc;
            if (c->lr_valid && c->lr_addr == a) {
                store(m, a, b, 4, &wait);
                x[d->rd] = 0;
            } else {
                x[d->rd] = 1;
            }
            c->lr_valid = false;
            break;
        case OP_AMOSWAP: case OP_AMOADD: case OP_AMOXOR: case OP_AMOAND: case OP_AMOOR:
        case OP_AMOMIN: case OP_AMOMAX: case OP_AMOMINU: case OP_AMOMAXU: {
            c->pc = pc;
            const uint32_t old = load(m, a, 4, &wait);
            uint32_t v = b;
            switch (d->op) {
            case OP_AMOADD: v = old + b; break;
            case OP_AMOXOR: v = old ^ b; break;
            case OP_AMOAND: v = old & b; break;
            case OP_AMOOR:  v = old | b; break;
            case OP_AMOMIN: v = (int32_t)old < (int32_t)b ? old : b; break;
            case OP_AMOMAX: v = (int32_t)old > (int32_t)b ? old : b; break;
            case OP_AMOMINU: v = old < b ? old : b; break;
            case OP_AMOMAXU: v = old > b ? old : b; break;
            }
            store(m, a, v, 4, &wait);
            x[d->rd] = old;
            break;
        }

        case OP_FENCE: break;
        case OP_ECALL:
            npc = exception(m, pc, CAUSE_ECALL_M, 0);
            taken = true;
            chg_kick(m);
            break;
        case OP_EBREAK:
            npc = exception(m, pc, CAUSE_BREAKPOINT, pc);
            taken = true;
            chg_kick(m);
            break;
        case OP_MRET:
            npc = mret(m);
            taken = true;
            break;
        case OP_WFI:
            c->wfi = true;
            chg_kick(m);
            break;

        case OP_CSRRW: case OP_CSRRS: case OP_CSRRC:
        case OP_CSRRWI: case OP_CSRRSI: case OP_CSRRCI: {
            bool ok;
            const uint32_t csr = (uint32_t)imm;
            const uint32_t old = csr_read(m, csr, &ok);
            const uint32_t src = (d->op >= OP_CSRRWI) ? (uint32_t)d->rs1 : a;
            c->pc = pc;
            switch (d->op) {
            case OP_CSRRW: case OP_CSRRWI: csr_write(m, csr, src); break;
            case OP_CSRRS: case OP_CSRRSI: if (d->rs1) csr_write(m, csr, old | src); break;
            case OP_CSRRC: case OP_CSRRCI: if (d->rs1) csr_write(m, csr, old & ~src); break;
            }
            x[d->rd] = old;
            break;
        }

        case OP_ILLEGAL:
        default:
            npc = exception(m, pc, CAUSE_ILLEGAL, (uint32_t)imm);
            taken = true;
            chg_kick(m);
            break;
        }

        m->cycles += (uint64_t)cost + (uint64_t)(wait & WAIT_CYCLES);

        if (wait & (WAIT_FLASH_DATA | WAIT_RAM_STORE | WAIT_RAM_LOAD | WAIT_LOAD)) {
            if (wait & WAIT_FLASH_DATA) {
                if (in_flash) {
                    /* the load had the flash; the prefetcher waited for it */
                    ft += FLASH_WORD;
                    if (m->cycles < issue + FLASH_WORD) m->cycles = issue + FLASH_WORD;
                } else {
                    frac += FLASH_LOAD_RAM8 - 8 * LOAD_CYCLES;
                    m->cycles += frac >> 3;
                    frac &= 7;
                }
            }
            if (!in_flash) {
                /* the SRAM port was the prefetcher's for these cycles */
                if (wait & WAIT_RAM_LOAD)
                    ft += RAM_LOAD_PORT;
                if (wait & WAIT_RAM_STORE) {
                    /* back to back stores go in pairs: 2 port cycles, then 1 */
                    const bool second = pair_open;
                    ft += second ? RAM_STORE2_PORT : RAM_STORE_PORT;
                    pair_open = !second;
                }
            }
        }
        if (!(wait & WAIT_RAM_STORE))
            pair_open = false;
        recent_data = ((recent_data << 1) |
                       (!in_flash && (wait & (WAIT_RAM_STORE | WAIT_RAM_LOAD | WAIT_LOAD)) != 0)) & 7;

        if (taken) {
            /* fetching starts over at the target once the word in flight is
               in. A data access to SRAM in the two instructions before the
               branch lands in the refetch bubble and hides a cycle of it */
            const unsigned hidden = (recent_data & 6) ? 1 : 0;
            uint64_t start = issue + 1 - hidden;
            const uint64_t inflight = ft + word - hidden;
            if (inflight > start)
                start = inflight;
            fw = WIDX(npc) - 1;
            ft = start;
        }
        pc = npc;
    }
    c->pc = pc;
    c->fetch_word = fw;
    c->fetch_time = ft;
    c->div_ready = div_ready;
    c->div_rd = (uint8_t)div_rd;
    c->frac8 = (uint8_t)frac;
    m->instret += retired;
}

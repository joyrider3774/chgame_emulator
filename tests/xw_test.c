/*
 * Checks the decoder's reading of WCH's XW compressed loads and stores
 * against encodings produced by WCH's own assembler (riscv-none-embed-as
 * -march=rv32imacxw, disassembled with objdump -M xw), kept in
 * xw_golden.txt as "<hex> <mnemonic> <reg>,<offset>(<base>)".
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "machine.h"

static int reg_num(const char *s)
{
    static const char *abi[32] = { "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };
    for (int i = 0; i < 32; i++) if (!strcmp(s, abi[i])) return i;
    return -1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "xw_golden.txt";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    char line[256];
    int checked = 0, failed = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned hex;
        char mn[16], ops[64];
        if (sscanf(line, "%x %15s %63s", &hex, mn, ops) != 3) continue;
        char r[8], base[8];
        int off;
        if (sscanf(ops, "%7[^,],%d(%7[^)])", r, &off, base) != 3) continue;
        const int rn = reg_num(r), bn = reg_num(base);

        ChgInsn d;
        memset(&d, 0, sizeof d);
        cpu_decode(0x100, hex, &d, false);

        /* the op numbers are private to rv32.c, so compare by what a load or a
           store has to get right: which register, which base, which offset,
           and the length */
        const bool isStore = mn[0] == 's';
        const int got_r = isStore ? d.rs2 : (d.rd == 32 ? 0 : d.rd);
        bool ok = d.len == 2 && d.rs1 == bn && got_r == rn && d.imm == off &&
                  !strcmp(cpu_op_name(d.op), mn);
        checked++;
        if (!ok) {
            failed++;
            printf("MISMATCH %04x %s %s: got %s len %d rd %d rs1 %d rs2 %d imm %d\n", hex, mn, ops,
                   cpu_op_name(d.op), d.len, d.rd, d.rs1, d.rs2, d.imm);
        }
    }
    fclose(f);
    printf("%d XW encodings checked, %d wrong\n", checked, failed);
    return failed ? 1 : 0;
}

/*
 * Calibration loops for the emulator's cycle model. Every function is
 *
 *     void name(uint32_t iterations, void *pointer)
 *
 * and is one loop whose body is exactly what the name says, so its cost per
 * iteration minus the cost of a loop with a different body gives the cost of
 * one instruction kind. Each exists twice: f_* in flash, r_* in RAM (.data is
 * copied to RAM at start up and runs from there).
 *
 * Registers: a0 counts down, a1 is the pointer, t1-t3 are scratch.
 */

    /* A 2 byte pad is only emitted in rvc mode; under norvc .balign
       silently leaves the address as it is */
    .macro ALIGN4
    .option push
    .option rvc
    .balign 4
    .option pop
    .endm

    .macro LOOP_END32
    addi a0, a0, -1
    bnez a0, 1b
    ret
    .endm

    .macro TESTS p
    .option push
    /* the linker must not move code: the layout is the point */
    .option norelax

    /* sequential code: 8 and 24 32-bit instructions, so the difference is 16 */
    .option norvc
    ALIGN4
    .global \p\()add32_8
\p\()add32_8:
1:  .rept 8
    addi t1, t1, 1
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()add32_24
\p\()add32_24:
1:  .rept 24
    addi t1, t1, 1
    .endr
    LOOP_END32

    /* the same with 16-bit instructions: 16 and 48 of them, 8 and 24 words */
    .option rvc
    ALIGN4
    .global \p\()add16_16
\p\()add16_16:
1:  .rept 16
    c.addi t1, 1
    .endr
    c.addi a0, -1
    c.bnez a0, 1b
    ret

    ALIGN4
    .global \p\()add16_48
\p\()add16_48:
1:  .rept 48
    c.addi t1, 1
    .endr
    c.addi a0, -1
    c.bnez a0, 1b
    ret

    /* add32_8 again, but the loop starts in the upper half of a word.
       (aligned in rvc mode: under norvc a 2 byte pad is silently skipped) */
    ALIGN4
    .global \p\()add32_8_mis
\p\()add32_8_mis:
    c.nop
    .option norvc
1:  .rept 8
    addi t1, t1, 1
    .endr
    LOOP_END32

    /* memory: 8 of each through a1 */
    ALIGN4
    .global \p\()lw8
\p\()lw8:
1:  .rept 8
    lw t1, 0(a1)
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()lhu8
\p\()lhu8:
1:  .rept 8
    lhu t1, 0(a1)
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()sw8
\p\()sw8:
1:  .rept 8
    sw zero, 0(a1)
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()sb8
\p\()sb8:
1:  .rept 8
    sb zero, 0(a1)
    .endr
    LOOP_END32

    /* a load whose result the next instruction needs */
    ALIGN4
    .global \p\()lwuse8
\p\()lwuse8:
1:  .rept 8
    lw t1, 0(a1)
    addi t1, t1, 1
    .endr
    LOOP_END32

    /* 8 alu instructions between loads: is a load's extra cycle hidden? */
    ALIGN4
    .global \p\()lwalu8
\p\()lwalu8:
1:  .rept 8
    lw t1, 0(a1)
    addi t2, t2, 1
    .endr
    LOOP_END32

    /* arithmetic */
    ALIGN4
    .global \p\()divbig8
\p\()divbig8:
    li t2, -1
    li t3, 7
1:  .rept 8
    divu t1, t2, t3
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()divsmall8
\p\()divsmall8:
    li t2, 5
    li t3, 7
1:  .rept 8
    divu t1, t2, t3
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()mul8
\p\()mul8:
    li t2, 12345
    li t3, 678
1:  .rept 8
    mul t1, t2, t3
    .endr
    LOOP_END32

    /* control flow: 8 branches to the next instruction */
    ALIGN4
    .global \p\()btaken8
\p\()btaken8:
1:  .rept 8
    beq zero, zero, .+4
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()bnot8
\p\()bnot8:
1:  .rept 8
    bne zero, zero, .+4
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()jal8
\p\()jal8:
1:  .rept 8
    jal zero, .+4
    .endr
    LOOP_END32

    .option pop
    .endm

    /* Second round, RAM code only: how stores and loads cost next to other
       instructions, and which misaligned branch targets pay */
    .macro TESTS2 p
    .option push
    .option norelax
    .option norvc

    ALIGN4
    .global \p\()swalu8
\p\()swalu8:
1:  .rept 8
    sw zero, 0(a1)
    addi t2, t2, 1
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()sw2alu2
\p\()sw2alu2:
1:  .rept 4
    sw zero, 0(a1)
    sw zero, 4(a1)
    addi t2, t2, 1
    addi t2, t2, 1
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()sw1alu7
\p\()sw1alu7:
1:  sw zero, 0(a1)
    .rept 7
    addi t2, t2, 1
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()lw2alu2
\p\()lw2alu2:
1:  .rept 4
    lw t1, 0(a1)
    lw t3, 4(a1)
    addi t2, t2, 1
    addi t2, t2, 1
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()lw4
\p\()lw4:
1:  .rept 4
    lw t1, 0(a1)
    .endr
    LOOP_END32

    ALIGN4
    .global \p\()lw1alu7
\p\()lw1alu7:
1:  lw t1, 0(a1)
    .rept 7
    addi t2, t2, 1
    .endr
    LOOP_END32

    /* the loop starts in the upper half of a word with a 16-bit instruction */
    ALIGN4
    .global \p\()mis16
\p\()mis16:
    .option rvc
    c.nop
1:  c.addi t2, 1
    .option norvc
    .rept 8
    addi t1, t1, 1
    .endr
    LOOP_END32

    /* writePixels' byte swap as gcc builds it (see chg_bench), 64 pixels an
       outer iteration; a1 = 512 bytes: the source, then the destination */

    ALIGN4
    .global \p\()swap_al
\p\()swap_al:
    mv t4, a1
    addi t5, a1, 256
3:  mv a1, t4
    mv a2, t5
    .option rvc
    li a4, 0
    mv a5, a2
    mv a3, a1
    li t3, 128
    c.nop
    .option norvc
    /* inner loop aligned to 4 */
2:  add a2, a3, a4
    .option rvc
    lhu a2, 0(a2)
    c.addi a5, 2
    c.addi a4, 2
    .option norvc
    srai t0, a2, 8
    sb t0, -2(a5)
    sb a2, -1(a5)
    bne a4, t3, 2b
    addi a0, a0, -1
    bnez a0, 3b
    ret

    ALIGN4
    .global \p\()swap_mis
\p\()swap_mis:
    mv t4, a1
    addi t5, a1, 256
3:  mv a1, t4
    mv a2, t5
    .option rvc
    li a4, 0
    mv a5, a2
    mv a3, a1
    li t3, 128
    .option norvc
    /* inner loop in the upper half of a word */
2:  add a2, a3, a4
    .option rvc
    lhu a2, 0(a2)
    c.addi a5, 2
    c.addi a4, 2
    .option norvc
    srai t0, a2, 8
    sb t0, -2(a5)
    sb a2, -1(a5)
    bne a4, t3, 2b
    addi a0, a0, -1
    bnez a0, 3b
    ret

    .option pop
    .endm

    .section .text.benchflash, "ax", @progbits
    TESTS f_

    .section .data.benchram, "awx", @progbits
    TESTS r_
    TESTS2 r_

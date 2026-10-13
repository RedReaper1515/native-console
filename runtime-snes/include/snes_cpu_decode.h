/*
 * 65C816 opcode classification, shared by the recompiler's analyzer and
 * emitter, the disassembler, and tools/snes/asm65816.py (which parses the
 * SNES_OPINFO lines below, so keep them one per line in this format).
 *
 * The length of an instruction depends on the CPU mode for the immediate
 * forms: an accumulator immediate is 2 bytes with M = 1 and 3 with M = 0,
 * an index immediate likewise with X. A static recompiler must therefore
 * know the mode of every instruction; snes_insn_length() takes it.
 */
#ifndef SNES_CPU_DECODE_H
#define SNES_CPU_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "snes_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNES_AM_IMP,    /* implied / accumulator / stack: 1 byte */
    SNES_AM_IMM8,   /* #const, always 8-bit (REP, SEP, BRK/COP signature, WDM) */
    SNES_AM_IMM_M,  /* #const, width follows M */
    SNES_AM_IMM_X,  /* #const, width follows X */
    SNES_AM_DP,     /* dp */
    SNES_AM_DPX,    /* dp,X */
    SNES_AM_DPY,    /* dp,Y */
    SNES_AM_IND,    /* (dp) */
    SNES_AM_INDX,   /* (dp,X) */
    SNES_AM_INDY,   /* (dp),Y */
    SNES_AM_INDL,   /* [dp] */
    SNES_AM_INDLY,  /* [dp],Y */
    SNES_AM_SR,     /* sr,S */
    SNES_AM_SRIY,   /* (sr,S),Y */
    SNES_AM_ABS,    /* abs */
    SNES_AM_ABSX,   /* abs,X */
    SNES_AM_ABSY,   /* abs,Y */
    SNES_AM_LONG,   /* long */
    SNES_AM_LONGX,  /* long,X */
    SNES_AM_ABSIND, /* (abs) — JMP */
    SNES_AM_ABSINDX,/* (abs,X) — JMP, JSR */
    SNES_AM_ABSINDL,/* [abs] — JML */
    SNES_AM_REL8,   /* branch */
    SNES_AM_REL16,  /* BRL, PER */
    SNES_AM_BLOCK,  /* MVN/MVP dst,src */
} SnesAddrMode;

typedef enum {
    SNES_FLOW_NEXT,       /* continues with the next instruction */
    SNES_FLOW_BRANCH_COND,/* relative branch, or continues */
    SNES_FLOW_BRANCH,     /* BRA, BRL: relative, always */
    SNES_FLOW_JUMP,       /* JMP abs: same bank */
    SNES_FLOW_JUMP_LONG,  /* JML long */
    SNES_FLOW_JUMP_IND,   /* JMP (abs), JMP (abs,X), JML [abs] */
    SNES_FLOW_CALL,       /* JSR abs */
    SNES_FLOW_CALL_LONG,  /* JSL long */
    SNES_FLOW_CALL_IND,   /* JSR (abs,X) */
    SNES_FLOW_RETURN,     /* RTS */
    SNES_FLOW_RETURN_LONG,/* RTL */
    SNES_FLOW_RETURN_INT, /* RTI: restores P, so the mode is unknown after */
    SNES_FLOW_SOFT_INT,   /* BRK, COP */
    SNES_FLOW_MODE_SET,   /* REP/SEP: next mode follows from the immediate */
    SNES_FLOW_MODE_XCE,   /* XCE: next mode follows from the carry, if known */
    SNES_FLOW_MODE_PLP,   /* PLP: mode unknown after */
    SNES_FLOW_BLOCK,      /* MVN/MVP: repeats itself */
    SNES_FLOW_WAIT,       /* WAI */
    SNES_FLOW_STOP,       /* STP */
} SnesFlow;

typedef struct SnesOpInfo {
    const char *mnemonic;
    uint8_t mode;  /* SnesAddrMode */
    uint8_t flow;  /* SnesFlow */
} SnesOpInfo;

#define SNES_OPINFO_TABLE(X) \
    X(0x00, "BRK", SNES_AM_IMM8, SNES_FLOW_SOFT_INT) \
    X(0x01, "ORA", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0x02, "COP", SNES_AM_IMM8, SNES_FLOW_SOFT_INT) \
    X(0x03, "ORA", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0x04, "TSB", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x05, "ORA", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x06, "ASL", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x07, "ORA", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0x08, "PHP", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x09, "ORA", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0x0A, "ASL", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x0B, "PHD", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x0C, "TSB", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x0D, "ORA", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x0E, "ASL", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x0F, "ORA", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0x10, "BPL", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0x11, "ORA", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0x12, "ORA", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0x13, "ORA", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0x14, "TRB", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x15, "ORA", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x16, "ASL", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x17, "ORA", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0x18, "CLC", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x19, "ORA", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0x1A, "INC", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x1B, "TCS", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x1C, "TRB", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x1D, "ORA", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x1E, "ASL", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x1F, "ORA", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0x20, "JSR", SNES_AM_ABS, SNES_FLOW_CALL) \
    X(0x21, "AND", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0x22, "JSL", SNES_AM_LONG, SNES_FLOW_CALL_LONG) \
    X(0x23, "AND", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0x24, "BIT", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x25, "AND", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x26, "ROL", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x27, "AND", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0x28, "PLP", SNES_AM_IMP, SNES_FLOW_MODE_PLP) \
    X(0x29, "AND", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0x2A, "ROL", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x2B, "PLD", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x2C, "BIT", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x2D, "AND", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x2E, "ROL", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x2F, "AND", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0x30, "BMI", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0x31, "AND", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0x32, "AND", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0x33, "AND", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0x34, "BIT", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x35, "AND", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x36, "ROL", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x37, "AND", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0x38, "SEC", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x39, "AND", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0x3A, "DEC", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x3B, "TSC", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x3C, "BIT", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x3D, "AND", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x3E, "ROL", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x3F, "AND", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0x40, "RTI", SNES_AM_IMP, SNES_FLOW_RETURN_INT) \
    X(0x41, "EOR", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0x42, "WDM", SNES_AM_IMM8, SNES_FLOW_NEXT) \
    X(0x43, "EOR", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0x44, "MVP", SNES_AM_BLOCK, SNES_FLOW_BLOCK) \
    X(0x45, "EOR", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x46, "LSR", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x47, "EOR", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0x48, "PHA", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x49, "EOR", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0x4A, "LSR", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x4B, "PHK", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x4C, "JMP", SNES_AM_ABS, SNES_FLOW_JUMP) \
    X(0x4D, "EOR", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x4E, "LSR", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x4F, "EOR", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0x50, "BVC", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0x51, "EOR", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0x52, "EOR", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0x53, "EOR", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0x54, "MVN", SNES_AM_BLOCK, SNES_FLOW_BLOCK) \
    X(0x55, "EOR", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x56, "LSR", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x57, "EOR", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0x58, "CLI", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x59, "EOR", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0x5A, "PHY", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x5B, "TCD", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x5C, "JML", SNES_AM_LONG, SNES_FLOW_JUMP_LONG) \
    X(0x5D, "EOR", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x5E, "LSR", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x5F, "EOR", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0x60, "RTS", SNES_AM_IMP, SNES_FLOW_RETURN) \
    X(0x61, "ADC", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0x62, "PER", SNES_AM_REL16, SNES_FLOW_NEXT) \
    X(0x63, "ADC", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0x64, "STZ", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x65, "ADC", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x66, "ROR", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x67, "ADC", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0x68, "PLA", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x69, "ADC", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0x6A, "ROR", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x6B, "RTL", SNES_AM_IMP, SNES_FLOW_RETURN_LONG) \
    X(0x6C, "JMP", SNES_AM_ABSIND, SNES_FLOW_JUMP_IND) \
    X(0x6D, "ADC", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x6E, "ROR", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x6F, "ADC", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0x70, "BVS", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0x71, "ADC", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0x72, "ADC", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0x73, "ADC", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0x74, "STZ", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x75, "ADC", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x76, "ROR", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x77, "ADC", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0x78, "SEI", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x79, "ADC", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0x7A, "PLY", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x7B, "TDC", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x7C, "JMP", SNES_AM_ABSINDX, SNES_FLOW_JUMP_IND) \
    X(0x7D, "ADC", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x7E, "ROR", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x7F, "ADC", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0x80, "BRA", SNES_AM_REL8, SNES_FLOW_BRANCH) \
    X(0x81, "STA", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0x82, "BRL", SNES_AM_REL16, SNES_FLOW_BRANCH) \
    X(0x83, "STA", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0x84, "STY", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x85, "STA", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x86, "STX", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0x87, "STA", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0x88, "DEY", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x89, "BIT", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0x8A, "TXA", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x8B, "PHB", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x8C, "STY", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x8D, "STA", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x8E, "STX", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x8F, "STA", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0x90, "BCC", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0x91, "STA", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0x92, "STA", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0x93, "STA", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0x94, "STY", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x95, "STA", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0x96, "STX", SNES_AM_DPY, SNES_FLOW_NEXT) \
    X(0x97, "STA", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0x98, "TYA", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x99, "STA", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0x9A, "TXS", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x9B, "TXY", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0x9C, "STZ", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0x9D, "STA", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x9E, "STZ", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0x9F, "STA", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0xA0, "LDY", SNES_AM_IMM_X, SNES_FLOW_NEXT) \
    X(0xA1, "LDA", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0xA2, "LDX", SNES_AM_IMM_X, SNES_FLOW_NEXT) \
    X(0xA3, "LDA", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0xA4, "LDY", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xA5, "LDA", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xA6, "LDX", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xA7, "LDA", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0xA8, "TAY", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xA9, "LDA", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0xAA, "TAX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xAB, "PLB", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xAC, "LDY", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xAD, "LDA", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xAE, "LDX", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xAF, "LDA", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0xB0, "BCS", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0xB1, "LDA", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0xB2, "LDA", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0xB3, "LDA", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0xB4, "LDY", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0xB5, "LDA", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0xB6, "LDX", SNES_AM_DPY, SNES_FLOW_NEXT) \
    X(0xB7, "LDA", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0xB8, "CLV", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xB9, "LDA", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0xBA, "TSX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xBB, "TYX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xBC, "LDY", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0xBD, "LDA", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0xBE, "LDX", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0xBF, "LDA", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0xC0, "CPY", SNES_AM_IMM_X, SNES_FLOW_NEXT) \
    X(0xC1, "CMP", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0xC2, "REP", SNES_AM_IMM8, SNES_FLOW_MODE_SET) \
    X(0xC3, "CMP", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0xC4, "CPY", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xC5, "CMP", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xC6, "DEC", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xC7, "CMP", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0xC8, "INY", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xC9, "CMP", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0xCA, "DEX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xCB, "WAI", SNES_AM_IMP, SNES_FLOW_WAIT) \
    X(0xCC, "CPY", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xCD, "CMP", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xCE, "DEC", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xCF, "CMP", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0xD0, "BNE", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0xD1, "CMP", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0xD2, "CMP", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0xD3, "CMP", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0xD4, "PEI", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xD5, "CMP", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0xD6, "DEC", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0xD7, "CMP", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0xD8, "CLD", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xD9, "CMP", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0xDA, "PHX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xDB, "STP", SNES_AM_IMP, SNES_FLOW_STOP) \
    X(0xDC, "JML", SNES_AM_ABSINDL, SNES_FLOW_JUMP_IND) \
    X(0xDD, "CMP", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0xDE, "DEC", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0xDF, "CMP", SNES_AM_LONGX, SNES_FLOW_NEXT) \
    X(0xE0, "CPX", SNES_AM_IMM_X, SNES_FLOW_NEXT) \
    X(0xE1, "SBC", SNES_AM_INDX, SNES_FLOW_NEXT) \
    X(0xE2, "SEP", SNES_AM_IMM8, SNES_FLOW_MODE_SET) \
    X(0xE3, "SBC", SNES_AM_SR, SNES_FLOW_NEXT) \
    X(0xE4, "CPX", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xE5, "SBC", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xE6, "INC", SNES_AM_DP, SNES_FLOW_NEXT) \
    X(0xE7, "SBC", SNES_AM_INDL, SNES_FLOW_NEXT) \
    X(0xE8, "INX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xE9, "SBC", SNES_AM_IMM_M, SNES_FLOW_NEXT) \
    X(0xEA, "NOP", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xEB, "XBA", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xEC, "CPX", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xED, "SBC", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xEE, "INC", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xEF, "SBC", SNES_AM_LONG, SNES_FLOW_NEXT) \
    X(0xF0, "BEQ", SNES_AM_REL8, SNES_FLOW_BRANCH_COND) \
    X(0xF1, "SBC", SNES_AM_INDY, SNES_FLOW_NEXT) \
    X(0xF2, "SBC", SNES_AM_IND, SNES_FLOW_NEXT) \
    X(0xF3, "SBC", SNES_AM_SRIY, SNES_FLOW_NEXT) \
    X(0xF4, "PEA", SNES_AM_ABS, SNES_FLOW_NEXT) \
    X(0xF5, "SBC", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0xF6, "INC", SNES_AM_DPX, SNES_FLOW_NEXT) \
    X(0xF7, "SBC", SNES_AM_INDLY, SNES_FLOW_NEXT) \
    X(0xF8, "SED", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xF9, "SBC", SNES_AM_ABSY, SNES_FLOW_NEXT) \
    X(0xFA, "PLX", SNES_AM_IMP, SNES_FLOW_NEXT) \
    X(0xFB, "XCE", SNES_AM_IMP, SNES_FLOW_MODE_XCE) \
    X(0xFC, "JSR", SNES_AM_ABSINDX, SNES_FLOW_CALL_IND) \
    X(0xFD, "SBC", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0xFE, "INC", SNES_AM_ABSX, SNES_FLOW_NEXT) \
    X(0xFF, "SBC", SNES_AM_LONGX, SNES_FLOW_NEXT)

static inline const SnesOpInfo *snes_opinfo(uint8_t opcode)
{
#define SNES_OPINFO_ENTRY(op, mn, am, fl) { mn, (uint8_t)(am), (uint8_t)(fl) },
    static const SnesOpInfo table[256] = { SNES_OPINFO_TABLE(SNES_OPINFO_ENTRY) };
#undef SNES_OPINFO_ENTRY
    return &table[opcode];
}

/* Bytes in the instruction (opcode included) for a mode (SNES_MODE_*). */
static inline unsigned snes_insn_length(uint8_t opcode, unsigned mode)
{
    switch ((SnesAddrMode)snes_opinfo(opcode)->mode) {
    case SNES_AM_IMP: return 1;
    case SNES_AM_IMM_M: return (mode & SNES_MODE_M8) ? 2 : 3;
    case SNES_AM_IMM_X: return (mode & SNES_MODE_X8) ? 2 : 3;
    case SNES_AM_ABS: case SNES_AM_ABSX: case SNES_AM_ABSY: case SNES_AM_ABSIND:
    case SNES_AM_ABSINDX: case SNES_AM_ABSINDL: case SNES_AM_REL16: case SNES_AM_BLOCK: return 3;
    case SNES_AM_LONG: case SNES_AM_LONGX: return 4;
    default: return 2;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SNES_CPU_DECODE_H */

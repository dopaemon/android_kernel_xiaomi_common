/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, X-Ring technologies Inc., All rights reserved.
 */

#ifndef	_ARM_ASM_H_
#define	_ARM_ASM_H_

//#include <arm/arch.h>

//#ifndef __arm64__
//#error Why are we  including this?
//#endif

/* There is another definition of ALIGN for .c sources */
#ifdef __ASSEMBLER__
#ifndef ALIGN
#define ALIGN 2
#endif
#endif /* ASSEMBLER */

#ifndef FALIGN
#define FALIGN ALIGN
#endif

#define LB(x, n) n
#if	__STDC__
#ifndef __NO_UNDERSCORES__
#define	LCL(x)	L ## x
#define EXT(x) _ ## x
#define LEXT(x) (_ ## x ## :)
#else
#define	LCL(x)	(.L ## x)
#define EXT(x) x
#define LEXT(x) (x ## :)
#endif
#define LBc(x, n) (n ## :)
#define LBb(x, n) (n ## b)
#define LBf(x, n) (n ## f)
#else /* __STDC__ */
#ifndef __NO_UNDERSCORES__
#define LCL(x) L/**/x
#define EXT(x) _/**/x
#define LEXT(x) (_/**/x/**/ : )
#else /* __NO_UNDERSCORES__ */
#define	LCL(x)	(.L/**/x)
#define EXT(x) x
#define LEXT(x) (x/**/ : )
#endif /* __NO_UNDERSCORES__ */
#define LBc(x, n) (n/**/ : )
#define LBb(x, n) (n/**/b)
#define LBf(x, n) (n/**/f)
#endif /* __STDC__ */

#define String	(.asciz)
#define Value	(.word)
#define Times(a, b) (a*b)
#define Divide(a, b) (a/b)

#ifdef __ASSEMBLER__
#if	MACH_KDB
#include <ddb/stab.h>
/*
 * This pseudo-assembler line is added so that there will be at least
 *	one N_SO entry in the symbol stable to define the current file name.
 */
#endif	/* MACH_KDB */

/*
 * Multiline macros must use .macro syntax for now,
 * as there is no ARM64 statement separator.
 */
.macro ENTRY
	.align FALIGN
	.globl _$0
	_$0 :
.endmacro

.macro ENTRY2
	.align FALIGN
	.globl _$0
	.globl _$1
	_$0 :
	_$1 :
.endmacro

.macro READ_THREAD
	mrs $0, TPIDR_EL1
.endmacro

.macro BRANCH_EXTERN
	b _$0
.endmacro

.macro CALL_EXTERN
	bl _$0
.endmacro

.macro MOV64
	movk $0, #((($1) >> 48) & 0x000000000000FFFF), lsl #48
	movk $0, #((($1) >> 32) & 0x000000000000FFFF), lsl #32
	movk $0, #((($1) >> 16) & 0x000000000000FFFF), lsl #16
	movk $0, #((($1) >> 00) & 0x000000000000FFFF), lsl #00
.endmacro

.macro MOV32
	movz $0, #((($1) >> 16) & 0x000000000000FFFF), lsl #16
	movk $0, #((($1) >> 00) & 0x000000000000FFFF), lsl #00
.endmacro

.macro ARM64_STACK_PROLOG
#if __has_feature(ptrauth_returns)
	pacibsp
#endif
.endmacro

.macro ARM64_STACK_EPILOG
#if __has_feature(ptrauth_returns)
	retab
#else
	ret
#endif
.endmacro

#define PUSH_FRAME			\
	stp fp, lr, [sp, #-16]!		%% \
	mov fp, sp			%%

#define POP_FRAME			\
	mov sp, fp			%% \
	ldp fp, lr, [sp], #16		%%

#define EXT(x) _ ## x

#ifdef XNU_KERNEL_PRIVATE
.macro PANIC_UNIMPLEMENTED
	bl EXT(panic_unimplemented)
.endmacro
#endif

#else /* NOT __ASSEMBLER__ */

/* These defines are here for .c files that wish to reference global symbols
 * within __asm__ statements.
 */
#ifndef __NO_UNDERSCORES__
#define CC_SYM_PREFIX "_"
#else
#define CC_SYM_PREFIX ""
#endif /* __NO_UNDERSCORES__ */
#endif /* __ASSEMBLER__ */

#ifdef __ASSEMBLER__

# define BRANCH_EXTERN(x)	(b	EXT(x))

#endif /* __ASSEMBLER__ */

#endif /* _ARM_ASM_H_ */

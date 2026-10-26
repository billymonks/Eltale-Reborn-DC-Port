@ The AICA firmware's entry: vectors at 0, then main with interrupts off. Nothing here uses an
@ interrupt; the clock is a channel's play position (main.c).
	.text
	.globl	reset
reset:
	b	start
	b	hang
	b	hang
	b	hang
	b	hang
	b	hang
	b	hang
	b	hang

hang:
	b	hang

start:
	@ Supervisor mode, IRQ and FIQ masked.
	mov	r0, #0xD3
	msr	CPSR_all, r0
	mov	sp, #0xF000
	bl	arm_main
	b	hang

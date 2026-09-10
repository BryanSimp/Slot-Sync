/* The last few instructions of a chainload, as position-independent code.
 *
 * A DOL's sections go to fixed addresses, and homebrew is linked at 0x80004000
 * -- so the sections of the .dol being loaded land on top of the loader that is
 * copying them. Doing the copy from a normal C function means overwriting the
 * loop while it runs. This stub exists to be memcpy'd somewhere out of the way
 * (MEM2) and jumped to, so the code doing the copying is never a target of it.
 *
 * Position-independent by construction: every branch is relative and internal,
 * nothing is read except through r3, and no function is called. Do not add a
 * call, a global, or a string literal here -- all three would resolve to
 * addresses that are being overwritten.
 *
 *   r3 = the plan, which lives with the staged image outside the target range:
 *          0(r3)          entry point
 *          4(r3)          section count
 *          8(r3) + 12n    dst, src, len   -- src == 0 means fill with zero
 */
	.text
	.globl	wii_dol_stub
	.globl	wii_dol_stub_end
	.type	wii_dol_stub, @function

wii_dol_stub:
	lwz	11, 0(3)		/* entry point, kept for the end */
	lwz	10, 4(3)		/* sections remaining */
	addi	12, 3, 8		/* first triple */

1:	cmpwi	10, 0
	beq	9f
	lwz	4, 0(12)		/* dst */
	lwz	5, 4(12)		/* src */
	lwz	6, 8(12)		/* len */
	addi	12, 12, 12
	addi	10, 10, -1
	cmpwi	6, 0
	beq	1b

	addi	7, 4, -1		/* pre-decrement for stbu */
	cmpwi	5, 0
	beq	4f

	addi	8, 5, -1		/* copy */
	mtctr	6
2:	lbzu	9, 1(8)
	stbu	9, 1(7)
	bdnz	2b
	b	5f

4:	li	9, 0			/* zero fill, for bss */
	mtctr	6
3:	stbu	9, 1(7)
	bdnz	3b

	/* The stores above sit in dcache, and the instructions they replace may
	 * still be in icache. Push one out and drop the other, a block at a
	 * time, before anything tries to execute what was just written. */
5:	mr	7, 4
	add	8, 4, 6
6:	dcbst	0, 7
	sync
	icbi	0, 7
	addi	7, 7, 32
	cmplw	7, 8
	blt	6b
	sync
	isync
	b	1b

9:	sync
	isync
	mtlr	11
	blr
wii_dol_stub_end:
	.size	wii_dol_stub, . - wii_dol_stub

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stub_clone3.S — minimal clone3 trampoline for the freestanding stub.
 *
 * x86_64 only.  Caller signature:
 *
 *   long fs_clone3_run(struct clone_args *args, size_t args_sz,
 *                       void (*entry)(void *arg), void *arg);
 *
 * Returns child tid (>0) in parent, never returns in child (entry must end
 * with SYS_exit; if it returns, we fall through to SYS_exit ourselves).
 *
 * Implementation: pin entry + arg into r12/r13 (callee-saved by SysV ABI,
 * also unmodified by the syscall instruction itself) so they survive the
 * clone3 syscall in the child's new context.  The kernel sets the child's
 * RSP to args->stack + args->stack_size before returning to userspace.
 *
 * This is the "10% of uwgsocks/preload/core/clone3_asm.S" version — no
 * sigaltstack, no SIGSYS, no futex wait between parent and child.
 */
	.text
	.globl	fs_clone3_run
	.type	fs_clone3_run, @function
fs_clone3_run:
	/* save callee-saved regs we use */
	pushq	%r12
	pushq	%r13

	movq	%rdx, %r12		/* entry_fn */
	movq	%rcx, %r13		/* entry_arg */

	movq	$435, %rax		/* SYS_clone3 */
	/* %rdi = args, %rsi = args_sz already set by caller (sysv ABI) */
	syscall

	testq	%rax, %rax
	js	.Lparent_err		/* clone3 returned negative errno */
	jnz	.Lparent_ok		/* clone3 returned tid > 0 in parent */

	/* CHILD: rsp is now args->stack + args->stack_size (kernel set it).
	 * r12 = entry, r13 = arg.  Call entry(arg). */
	movq	%r13, %rdi
	callq	*%r12
	/* If entry returns, exit the thread.  SYS_exit (60), arg = 0. */
	movq	$60, %rax
	xorq	%rdi, %rdi
	syscall
	/* unreachable */
	ud2

.Lparent_ok:
.Lparent_err:
	popq	%r13
	popq	%r12
	retq
	.size	fs_clone3_run, .-fs_clone3_run

	/* Mark the stack non-executable so the static binary has no
	 * PT_GNU_STACK with X bit set. */
	.section .note.GNU-stack,"",@progbits

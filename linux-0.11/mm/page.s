/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */
 // 在memory.c完成

.globl _page_fault	// 声明全局变量		

_page_fault:
	xchgl %eax,(%esp)	// 取出错码到eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%edx		// 置内核数据段选择符
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	movl %cr2,%edx		// 取引起页面异常的线性地址
	pushl %edx
	pushl %eax
	testl $1,%eax
	jne 1f
	call _do_no_page
	jmp 2f
1:	call _do_wp_page	// 调用缺页处理函数 memory.c
2:	addl $8,%esp		
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

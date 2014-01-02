/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call': 
 * 系统调用返回( 'ret_from_system_call' )时堆栈的内容：
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - %fs
 *	14(%esp) - %es
 *	18(%esp) - %ds
 *	1C(%esp) - %eip
 *	20(%esp) - %cs
 *	24(%esp) - %eflags
 *	28(%esp) - %oldesp
 *	2C(%esp) - %oldss
 */


// 对于所有系统调用的实现函数，内核把他们按照系统调用功能号顺序排列成一张
// 函数指针表（include/linux/sys.h）
// 然后在 中断int80 根据用户提供的功能号调用对应系统调用函数进行处理

SIG_CHLD	= 17		// 定义SIG_CHLD信号（子进程停止或结束）

EAX		= 0x00			// 堆栈中各个寄存器的偏移位置
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
FS		= 0x10
ES		= 0x14
DS		= 0x18
EIP		= 0x1C
CS		= 0x20
EFLAGS		= 0x24
OLDESP		= 0x28
OLDSS		= 0x2C
// 任务结构（task_struct）中变量的偏移量 include/linux/sched.h
state	= 0		# these are offsets into the task-struct. 	// 进程状态码
counter	= 4			// 任务运行时间计数（递减），运行时间片
priority = 8		// 运行优先级，开始时counter = priority 越大则	运行时间越长										
signal	= 12		// 信号位图
sigaction = 16		# MUST be 16 (=len of sigaction)	// sigaction结构长度必须是16字节
blocked = (33*16)	// 受阻塞信号位图偏移量

# offsets within sigaction
// 定义在sigaction结构中的偏移量， include/signal.h
sa_handler = 0		// 信号处理过程描述符
sa_mask = 4			// 信号屏蔽码
sa_flags = 8		// 信号集
sa_restorer = 12	// 恢复函数指针， kernel/signal.c

nr_system_calls = 72	// linux 0.11版内核中的系统调用总数

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

// 错误的系统调用号
.align 2				// 内存4字节对齐
bad_sys_call:
	movl $-1,%eax		// eax 置 -1，退出中断
	iret

// 重新执行调度程序入口，调度程序schedule在 kernel/sched.c
// 当调度程序schedule()返回时就从 ret_from_sys_call处继续执行
.align 2
reschedule:
	pushl $ret_from_sys_call	// 将ret_from_sys_call的地址入栈
	jmp _schedule

// int 0x80 linux系统调用入口点
.align 2
_system_call:
	cmpl $nr_system_calls-1,%eax	// 调用号如果超出范围就是 之前 bad_sys_call
	ja bad_sys_call
	push %ds		// 保留原段寄存器值
	push %es
	push %fs
// 一个系统调用最多带3个参数，也可以不带参数
	pushl %edx
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
	movl $0x10,%edx		# set up ds,es to kernel space
	mov %dx,%ds			// ds, es指向内核数据段（全局描述符表中数据段描述符）
	mov %dx,%es
// fs指向局部数据段，见 fork.c 中copy_mem()函数
	movl $0x17,%edx		# fs points to local data space
	mov %dx,%fs
// 调用地址=[_sys_call_table + %eax * 4]
// sys_call_table[]是一个指针数组，定义在include/linux/sys.h中
// 设置了所有72个系统调用C处理函数的地址
	call _sys_call_table(,%eax,4)	// 间接调用指定功能C函数
	pushl %eax						// 把系统调用返回值入栈
// 查看当前任务的运行状态，如果不在就绪状态(state != 0)就去执行调度程序
// 如果在就绪状态，但时间片用完(counter = 0),那么也去执行调度程序
// 例如当后台进程组中的进程执行装段读写操作，那么默认条件下该后台进程组所有的进程会收到
// SIGTTIN 或者 SIGTTOU 信号，导致进程组中所有进程处于停止状态
// 而当前进程会立即返回
	movl _current,%eax
	cmpl $0,state(%eax)		# state
	jne reschedule			// 重新执行调度程序
	cmpl $0,counter(%eax)		# counter
	je reschedule			// 重新执行调度程序

// 以下代码执行冲系统调用C函数返回后，对信号进行识别处理
// 其他中断服务程序退出后也将跳转到这里进行处理后才退出中断过程
ret_from_sys_call:
// 先判断当前任务是否是初始任务task0
// 如果是就直接返回
	movl _current,%eax		# task[0] cannot have signals
	cmpl _task,%eax
	je 3f
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ?
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ?
	jne 3f
	movl signal(%eax),%ebx
	movl blocked(%eax),%ecx
	notl %ecx
	andl %ebx,%ecx
	bsfl %ecx,%ecx
	je 3f
	btrl %ecx,%ebx
	movl %ebx,signal(%eax)
	incl %ecx
	pushl %ecx
	call _do_signal
	popl %eax
3:	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	pop %fs
	pop %es
	pop %ds
	iret

.align 2
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp _math_error

.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore
	pushl %ebp
	pushl %esi
	pushl %edi
	call _math_emulate
	popl %edi
	popl %esi
	popl %ebp
	ret
// int32 --- (int 0x20)时钟中断处理程序，中断频率被设置为100Hz（include/linux/sched.h）
// #define HZ 100， 定时芯片在kernel/sched.c被初始化
// jiffies每10毫秒加1
// 
.align 2
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	incl _jiffies
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20
	movl CS(%esp),%eax
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...
	jmp ret_from_sys_call

.align 2
_sys_execve:
	lea EIP(%esp),%eax
	pushl %eax
	call _do_execve
	addl $4,%esp
	ret

// sys_fork()调用，创建子进程 include/linux/sys.h
.align 2
_sys_fork:
	call _find_empty_process	// 调用find_empty_process() kernel/fork.c
	testl %eax,%eax				// 在eax中返回进程号pid，若返回负数退出
	js 1f
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process			// 调用copy_process() 		kernel/fork.c
	addl $20,%esp
1:	ret

_hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1
	jmp 1f			# give port chance to breathe
1:	jmp 1f
1:	xorl %edx,%edx
	xchgl _do_hd,%edx
	testl %edx,%edx
	jne 1f
	movl $_unexpected_hd_interrupt,%edx
1:	outb %al,$0x20
	call *%edx		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret

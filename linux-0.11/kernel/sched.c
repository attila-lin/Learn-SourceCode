/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */
// 任务调度的相关函数
// 我觉得很重要

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>	// 内核头文件，含有一些内核常用函数的原型定义
#include <linux/sys.h>		// 系统调用头文件，含有72个系统调用C函数处理程序，sys_ 开头
#include <linux/fdreg.h>		
#include <asm/system.h>		// 系统头文件，定义了 设置 或 修改描述符/中断门 等嵌入式汇编宏 ！！最后面的时钟中断门 系统调用中断门
#include <asm/io.h>			// io头文件，定义硬件端口输入/输出宏汇编语句
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))	// 信号nr在信号位图中对应的二进制数值 如：信号5 = 00010000b
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))	// 除了SIGKILL和SIGSTOP外都是可阻塞的
// 显示任务号nr的进程号，进程状态和内核堆栈空闲字节数（大约）
void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct); // j是指定任务数据结构末

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])	// 指定任务数据结构以后等于0的字节数
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}
// 显示所有任务的任务号，进程号，进程状态和内核堆栈空间字节数
// 最大进程数量64个，定义在include/kernel/sched.h
void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)	// #define NR_TASKS 64
		if (task[i])
			show_task(i,task[i]);
}
// 输入时钟频率，linux希望是100Hz，每10ms发出一层时钟中断
#define LATCH (1193180/HZ)

extern void mem_use(void);

// 有些系统调用通过asm
extern int timer_interrupt(void); // 时钟中断 kernel/system_call.s
extern int system_call(void);		// 系统调用中断 也在system_call.s

// 每个进程的内核态堆栈结构 ？？为什么要用union
union task_union {		//任务联合（任务结构成员和stack字符数组成员）
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,}; // 定义初始进程数据 

// 以下是系统时钟
long volatile jiffies=0;	// volatile好像没用,尽量保存在通用寄存器
long startup_time=0;		// 从1970：0：0：0开始计数秒数 long 可能会造成bug 在2038年
struct task_struct *current = &(init_task.task);	// 当前进程指针
struct task_struct *last_task_used_math = NULL;		// 使用过协处理器任务的指针 ？？？？？

struct task_struct * task[NR_TASKS] = {&(init_task.task), }; // 定义任务指针数组

// 以下代码定义用户堆栈
// 1K项 4K字节
long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 }; // ss被设置为内核数据段选择符 0x10
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
// 辅助处理器（Coprocessor），也翻译为协处理器，是为了协助中央处理器进行对其无法执行或执行效率、效果低下的处理工作而研究开发使用处理器。这些中央处理器无法执行的工作有很多，比如设备间的信号传输、接入设备的管理等；而执行效率、效果低下的有图形处理、声频处理等。为了进行这些处理，各种辅助处理器就诞生了。需要说明的是，由于现在的计算机中，整数运算器与浮点运算器已经整合在一起，因此浮点处理器已经不算是辅助处理器。而内建于CPU中的协处理器，同样不算是辅助处理器，除非它是独立存在。[來源請求]
// from wiki
// 作用：将当前协处理器内容保存到老协处理器状态数组中，并将当前任务的协处理器内容进行加载
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
// 调度函数！！！
// 任务0是闲置任务，它的state不用，不能被杀死
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */
// 检测alarm,唤醒任何已经得到信号的可中断任务

// 遍历任务数组，从最后一个开始
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) { // 跳过空指针
			if ((*p)->alarm && (*p)->alarm < jiffies) { // 如果设置过alarm，并过期
					(*p)->signal |= (1<<(SIGALRM-1));	// 置SIGALRM信号
					(*p)->alarm = 0;					// 清alarm
				}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE) // 如果被阻塞的信号还有其他信号
				(*p)->state=TASK_RUNNING;	// 而且任务可中断，则置就绪态
		}

/* this is the scheduler proper: */
// 主要的调度程序
// 从最后一个任务开始循环，
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p) // 跳过空任务
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c) // 比较counter 取大的 
				c = (*p)->counter, next = i;
		}
		if (c) break; // break，进行任务切换
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) // 不能切换就重新比较
			if (*p)									
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;				// counter的计算方式
	}
	switch_to(next); // 是在sched.h中的宏
// #define switch_to(n) {\
// struct {long a,b;} __tmp; \
// __asm__("cmpl %%ecx,_current\n\t" \
// 	"je 1f\n\t" \
// 	"movw %%dx,%1\n\t" \
// 	"xchgl %%ecx,_current\n\t" \
//	"ljmp %0\n\t" \
//	"cmpl %%ecx,_last_task_used_math\n\t" \
//	"jne 1f\n\t" \
//	"clts\n" \
//	"1:" \
//	::"m" (*&__tmp.a),"m" (*&__tmp.b), \
//	"d" (_TSS(n)),"c" ((long) task[n])); \
// }
}

// 系统调用pause
// state设置为可中断的等待状态，并重新调度
// 将导致当前进程进入睡眠状态，直到收到信号
int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}


// 将当前进程置为不可中断的等待状态，让睡眠队列头指针指向当前任务
// 只有明确唤醒才返回
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p) // 指针无效退出
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	// tmp指向已经在等待队列上的任务 
	// 将当前任务插入到等待队列
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)	// 如果之前还有等待状态。将其置为就绪
		tmp->state=0;
}

// 将当前任务置为可中断的等待状态，并加入等待队列
void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)	
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL; // 有错？*p = tmp
	if (tmp)
		tmp->state=0;
}
// 唤醒*p指向的任务
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
// 关于软盘的。。。暂时不看
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;
	current->counter=0;
	if (!cpl) return;
	schedule();
}


// 系统调用
int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}


// 初始化！！！！！！
void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");

// 全局描述符表中设置初始进程 
// gdt是描述符表数组	head.h
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
// 清任务数组和描述符表项 定义在head.h
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */

	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl"); // 复位NT标志
	ltr(0);
	lldt(0);
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt); // 设置时钟中断门
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);		// 设置系统调用中断门
	// 在include/asm/system.h
}

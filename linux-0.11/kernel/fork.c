/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>
// 写页面验证。不可写则复制页面 在mm/memory.c
extern void write_verify(unsigned long address);

long last_pid=0; // 最新进程号。由get_empty_process()自动生成

// 进程空间区域写验证
// 对当前进程逻辑地址 addr 到 addr+size 这一段范围以页为单位执行写操作前检测
// 以页为单位
// 所以先找出 addr 所在页的开始地址 start 。
// 然后在 start 上加上进程数据段基地址，使start变成CPU 4G线性空间中的地址
// 最后循环调用 write_verify（）
// 如果只读，执行复制页面操作
void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000; //找到
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}
// 复制内存页表
int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f);
	data_limit=get_limit(0x17);
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * 0x4000000;
	p->start_code = new_code_base;
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
 // fork子程序。
 // 复制系统进程信息task[n] 设置必要寄存器。复制整个数据段
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;

// 为新任务分配内存
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;	// 将当前进程任务结构内容复制到申请的内存页面p开始处
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	// 修改复制来的进程结构
	p->state = TASK_UNINTERRUPTIBLE; //设为不可中断等待状态，防止被执行
	p->pid = last_pid;	// 新进程号
	p->father = current->pid;	// 父进程为当前进程
	p->counter = p->priority;	// 运行时间片段
	p->signal = 0;	// 信号位图置0
	p->alarm = 0;	// 报警定时值为0
	p->leader = 0;		/* process leadership doesn't inherit */
					// 领导权不继承
	p->utime = p->stime = 0;	// 用户态时间和核心态运行时间
	p->cutime = p->cstime = 0;	// 子进程用户态和核心态运行时间
	p->start_time = jiffies;	// 进程开始运行时间
	// 修改任务状态段TSS数据
	// 新的一页内存所以（PAGE_SIZE + (long) p）让esp0刚好指向该页顶端
	p->tss.back_link = 0;	//
	p->tss.esp0 = PAGE_SIZE + (long) p; // 任务内核态栈指针
	p->tss.ss0 = 0x10;	// 内核态栈段选择符
	p->tss.eip = eip;	// 指令代码指针
	p->tss.eflags = eflags; 
	p->tss.eax = 0;	// fork()返回时新进程会返回0
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff; // 段寄存器仅16位有效
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	// 父进程有文件打开。那文件打开次数+1
	// 子进程共享打开的文件
	// 当前进程pwd root exectable引用次数均+1
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	// 
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->state = TASK_RUNNING;	/* do this last, just in case */
								// 现在才置为就绪态
	return last_pid;
}
// 获取不重复的进程号
int find_empty_process(void)
{
	int i;
// last_pid是全局变量
	repeat:
		if ((++last_pid)<0) last_pid=1; // 担心溢出
		for(i=0 ; i<NR_TASKS ; i++)	
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	for(i=1 ; i<NR_TASKS ; i++) // 不从0开始
		if (!task[i])
			return i;
	return -EAGAIN;
}

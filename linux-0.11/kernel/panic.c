/*
 *  linux/kernel/panic.c
 *
 *  (C) 1991  Linus Torvalds
 */
// 显示内核错误并使系统进入死循环
/*
 * This function is used through-out the kernel (includeinh mm and fs)
 * to indicate a major problem.
 */
#include <linux/kernel.h>
#include <linux/sched.h>

void sys_sync(void);	/* it's really int */

volatile void panic(const char * s)
{
	printk("Kernel panic: %s\n\r",s);
	if (current == task[0])
		printk("In swapper task - not syncing\n\r");
	else
		sys_sync(); // 文件系统同步
	for(;;);
}

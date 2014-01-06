/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */
// 对高速缓冲区进行操作和管理
// 高速缓冲区位于代码块和主内存区间
// 高速缓冲区在块设备和内核其他应用之间起桥梁作用

// 缓冲块的缓冲头 buffer_head 结构在 include/linux/fs.h
// 双向链表

// 通过不让中断处理过程改变缓冲区，让调用者指行。避免竞争条件


#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end; // 内核代码的末端？？？？
struct buffer_head * start_buffer = (struct buffer_head *) &end;
// 用散列表
struct buffer_head * hash_table[NR_HASH];	// NR_HASH = 07
// 空闲缓冲块链表头指针
static struct buffer_head * free_list;
// 等待空闲缓冲块而睡眠的任务队列
static struct task_struct * buffer_wait = NULL;
// 系统含有的缓冲块个数
int NR_BUFFERS = 0;

// 等待指定缓冲块解锁
static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();	// 关中断 include/asm/system.h:#define cli() __asm__ ("cli"::)
	while (bh->b_lock)	// 如果上锁 就睡眠并等解锁
		sleep_on(&bh->b_wait); // b_wait 指向等待该缓冲区解锁的任务
		// kernel/sched.c:void sleep_on(struct task_struct **p)
	sti();	// 开中断
}

// 设备数据同步
int sys_sync(void)
{
	int i;	
	struct buffer_head * bh;

// 调用i节点同步函数，把内存i节点表中所有修改过的i节点写入高速缓冲
// 然后扫描所有高速缓冲，对已被修改的缓冲产生写盘请求，将缓冲区数据写入盘中
	sync_inodes();		/* write out inodes into buffers */  // fs/inode.c:void sync_inodes(void)
	bh = start_buffer;	// bh指向缓冲区开始处
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);	// 等待缓冲区解锁
		if (bh->b_dirt)	// 如果脏了
			ll_rw_block(WRITE,bh);	// 产生写设备块请求
			// kernel/blk_drv/ll_rw_blk.c:void ll_rw_block(int rw, struct buffer_head * bh)
	}
	return 0;
}

// 对指定设备进行高速缓冲数据与设备上数据的同步操作
int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

// 先对参数指定的设备进行数据同步，让设备数据和高速缓冲区数据同步
// 方法：
// 扫描高速缓冲区的所有缓冲块，对指定设备的缓冲块，检测是否上锁，
// 如果被上锁就睡眠等其解锁，然后判断该缓冲块是否还是指定设备的缓冲块并且被修改过
// 如果是就执行写盘操作
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev) 	// 不是设备dev的缓冲区
			continue;
		wait_on_buffer(bh);		// 等待缓冲区解锁
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
			// kernel/blk_drv/ll_rw_blk.c:void ll_rw_block(int rw, struct buffer_head * bh)
	}
// 再将i节点数据写入高速缓冲，让i节点表inode_table中inode和缓冲区信息同步
	sync_inodes();
// 高速缓冲区数据更新后，再和设备数据同步
// 采用两次同步操作为了提高内核执行效率
// 第一次：让内核中许多“脏块”变干净，使i节点的同步操作能高效执行
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}
// 使指定设备在高速缓冲区中的数据无效
// 扫描高速缓冲区内所有缓冲块，对指定设备的缓冲块复位其有效更新和已修改标志
void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)	// 如果不是指定设备
			continue;
		wait_on_buffer(bh);		// 等待解锁
		if (bh->b_dev == dev)	// 重新判断
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
// 检查一个软盘是否更换
// 如果已经更换就使高速缓冲中与该软驱对应的所有缓冲区无效
void check_disk_change(int dev)
{
	int i;
	// 检测是不是软盘
	if (MAJOR(dev) != 2)
		return;
	if (!floppy_change(dev & 0x03)) // 检查是否更换 blk/floppy.c
		return;
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}
// hash函数定义和hash表项的计算式
// 寻找缓冲区有两个条件 dev block
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

// 从hash队列和空闲队列中移除缓冲块
// hash是 双向链表
// 空闲是  双向循环链表
static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
// 如果该缓冲区是该队列中的头一个块，让hash表的对应项指向本队列的下一个缓冲区
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
		
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	// 如果空闲链表头指向本缓冲区，让其指向下一个缓冲区
	if (free_list == bh)
		free_list = bh->b_next_free;
}
// 将缓冲块插入空闲链表尾部，同时放入hash列表
static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}
// 利用hash找给定设备和指定块号的缓冲区块
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;
// 搜索hash
	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
 // 利用hash在高速缓冲区寻找指定缓冲块，找到就上锁并返回头指针
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
 // 宏用于同时判断缓冲区的修改标志和锁定标志
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	// 如果指定块在高速缓冲，返回对应头指定
	if (bh = get_hash_table(dev,block))
		return bh;
	// 扫描空闲数据块链表
	tmp = free_list;
	do {
		if (tmp->b_count) // 如果正被使用，count不为0
			continue;
		// 如果hd为空，
		if (!bh || BADNESS(tmp)<BADNESS(bh)) { //？？？？？？？？？？？
			bh = tmp;
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	wait_on_buffer(bh);
	if (bh->b_count)
		goto repeat;
	while (bh->b_dirt) {
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	// 占用此缓冲块
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	// 移出hash 和 空闲
	remove_from_queues(bh);
	// 更新到指定的设备和其上的指定块
	bh->b_dev=dev;
	bh->b_blocknr=block;
	// 插入新位置
	insert_into_queues(bh);
	return bh;
}
// 释放指定缓冲块
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait); // kernel/sched.c:void wake_up(struct task_struct **p)
	// buffer_wait？？？？？？在哪里
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
// 从设备读取数据块，然后保存到缓冲区
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block))) // 申请高速缓冲块
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)	// 如果这个缓冲块是有效的，就是已更新的，就直接用
		return bh;
	// 不是有效的
	ll_rw_block(READ,bh); // 先调用读写
	wait_on_buffer(bh);
	if (bh->b_uptodate)	// 再判断
		return bh;
	brelse(bh);	// 否则说明读设备操作失败
	return NULL;
}
// 复制内存块
#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
 // 一次读取4个缓冲块到内存指定位置
 // 页面数据的地址
 // b[4]是含4个设备数据块号的数组
 // 函数仅用于 mm/memory.c 中的do_no_page()
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			if (bh[i] = getblk(dev,b[i]))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]); // 产生读设备块请求
		} else
			bh[i] = NULL;
	// 将4个缓冲块上的内容顺序复制到指定地址
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]); // 释放
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
 // 从指定设备读取指定的块
 // 参数个数可变
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args; // VA_LIST 是在C语言中解决变参问题的一组宏，所在头文件：#include <stdarg.h>
	struct buffer_head * bh, *tmp;

	// 取可变参数表第一个参数
	// 从高速缓冲区中取出指定设备和块号的缓冲块
	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}
// 缓冲区初始化
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;
// 根据参数提供的缓冲区高端位置确定实际缓冲区高端位置 
// 在init/main.c中的调用  
// 				buffer_init(buffer_memory_end);				// fs/buffer.c
// 如果高端等于1MB，是因为640KB-1MB被显示内存和BIOS占用。
// 所以实际可用缓冲区内存高端位置是640KB，否则缓冲区高端大于1MB
	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
		
// 这里初始化缓冲区
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;	// 使用该缓冲区的设备号
		h->b_dirt = 0;	// 脏标志
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;	// 指向对应缓冲块数据块
		h->b_prev_free = h-1;	// 指向前一项
		h->b_next_free = h+1;	// 指向下一项
		h++;					//
		NR_BUFFERS++;			// 缓冲区块数累加
		if (b == (void *) 0x100000)	// 如果b递减到1MB，跳过384KB
			b = (void *) 0xA0000;	// 直接指向640KB
	}
	h--;	// h指向最后一个有效缓冲块头
	free_list = start_buffer;	// 空闲链表头指向第一个缓冲块
	free_list->b_prev_free = h;	
	h->b_next_free = free_list;
	// 初始化hash表
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	

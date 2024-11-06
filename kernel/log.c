#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

/**
  * @brief 初始化文件系统的日志系统
  * @param dev 设备号，表示日志所在的磁盘设备
  * @param sb 超级块结构体的指针，用于获取日志的起始位置和大小
  * @retval 无
  */
void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE) // 检查日志头结构的大小是否超过了块大小
    panic("initlog: too big logheader"); // 如果日志头大小大于单个块的大小，则无法存储，触发异常

  initlock(&log.lock, "log"); // 初始化日志锁，用于保护日志操作的互斥性
  log.start = sb->logstart; // 日志的起始块号
  log.size = sb->nlog; // 日志占用的块数
  log.dev = dev; // 日志所在的设备号
  recover_from_log(); // 从磁盘日志恢复文件系统的状态，确保文件系统在崩溃后处于一致状态
}

/**
  * @brief 将已提交的块从日志复制到它们的目标位置（即原始位置），以完成事务的真正写入
  * @param recovering 恢复标志，如果为 1 表示正在崩溃恢复过程中，不需要解锁缓冲区
  * @retval 无
  */
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) { // 遍历日志中记录的所有块
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // 读取日志块
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // 读取目标位置
    memmove(dbuf->data, lbuf->data, BSIZE);  // 将块内容复制到目标位置
    bwrite(dbuf);  // 将目标块写回磁盘
    if(recovering == 0) // 如果不在崩溃恢复过程中，则解锁目标块
      bunpin(dbuf);

    // 释放日志块和目标块的缓冲区，以便其他进程可以访问
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

/**
  * @brief 将内存中的日志头信息写入磁盘，这是当前事务真正提交的时刻
  * @param 无
  * @retval 无
  */
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start); // 读取日志的头块，将其存储在缓冲区 buf 中
  struct logheader *hb = (struct logheader *) (buf->data); // 将缓冲区的数据区域转换为日志头结构
  int i;
  hb->n = log.lh.n; // 将内存中的日志头信息拷贝到缓冲区中，以准备写入磁盘
  for (i = 0; i < log.lh.n; i++) { // 将日志中修改的块数量写入日志头
    hb->block[i] = log.lh.block[i]; // 记录每个已修改的块号
  }
  bwrite(buf); // 将缓冲区 buf 中的日志头写入磁盘上的日志区域
  brelse(buf); // 释放缓冲区，以便其他进程可以访问
}

/**
  * @brief 从日志恢复文件系统的状态，用于确保崩溃后文件系统的一致性
  * @param 无
  * @retval 无
  */
static void
recover_from_log(void)
{
  read_head(); // 读取日志头，将磁盘上的日志头信息读入内存中的日志头结构
  install_trans(1); // 如果日志中有提交的事务，将内容从日志复制到磁盘
  log.lh.n = 0; // 将日志中的事务计数设置为0，表示清空了日志中的记录
  write_head(); // 清空日志内容
}

/**
  * void begin_op()
  * @brief： 开始一个文件系统操作的事务，在每个文件系统系统调用开始时调用
  * @param： 无
  * @retval： 无
  */  
void
begin_op(void)
{
  acquire(&log.lock); // 获取日志锁，防止其他线程同时修改日志状态
  while(1){ // 循环直到成功开始操作
    if(log.committing){ // 如果当前正在进行提交操作
      sleep(&log, &log.lock); // 等待提交完成，释放锁并进入休眠状态
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // 检查日志空间是否足够，计算当前操作需要的空间
      // log.lh.n 是当前日志头中已记录的块数
      // log.outstanding 是当前未完成操作的数量
      // MAXOPBLOCKS 是单个操作允许的最大块数
      // 如果当前操作会使日志空间耗尽，则等待提交完成
      sleep(&log, &log.lock); // 释放锁并进入休眠状态
    } else {
      log.outstanding += 1; // 增加当前未完成操作的计数
      release(&log.lock); // 释放日志锁，允许其他线程访问
      break; // 成功开始操作，跳出循环
    }
  }
}

/**
  * @brief 结束一个文件系统操作。如果没有其他挂起操作，则提交事务。
  * @brief 在每个文件系统系统调用结束时调用，如果这是最后一个未完成的操作，没有其他挂起操作，则提交事务
  * @param 无
  * @retval 无
  */
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock); // 获取日志锁
  log.outstanding -= 1; // 减少未完成操作的计数
  if(log.committing) // 确保在提交过程中没有其他提交正在进行中
    panic("log.committing");
  if(log.outstanding == 0){ // 若所有挂起操作已完成
    do_commit = 1; // 标记需要进行提交
    log.committing = 1; // 设置提交标志，表示提交事务即将开始
  } else {
    // 可能有其他调用 begin_op() 的进程在等待日志空间。
    // 减少 log.outstanding 计数可以释放一些空间。
    wakeup(&log);  // 唤醒等待 log 空间的其他进程
  }
  release(&log.lock); // 释放日志锁

  if(do_commit){ // 如果标记为需要提交事务，则执行提交
    // 调用 commit()，在不持有锁的情况下进行提交，
    // 因为在持有锁时不允许睡眠。
    commit();
    acquire(&log.lock); // 提交完成后重新获取日志锁
    log.committing = 0; // 清除提交标志，表示提交已完成
    wakeup(&log); // 唤醒等待日志可用的其他进程
    release(&log.lock); // 释放日志锁
  }
}

/**
  * @brief 将缓存中已修改的块复制到日志区域
  * @param 无
  * @retval 无
  */
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) { // 遍历当前事务中的所有修改块，将它们从缓存复制到日志区域
    struct buf *to = bread(log.dev, log.start+tail+1); // 获取日志块
    struct buf *from = bread(log.dev, log.lh.block[tail]); // 获取缓存块
    memmove(to->data, from->data, BSIZE); // 将缓存块的数据复制到日志块
    bwrite(to);  // 将日志块写入磁盘上的日志区域
    brelse(from); // 释放缓存块
    brelse(to); // 释放日志块
  }
}

/**
  * @brief 提交当前事务，将修改写入磁盘日志并安装到实际位置
  * @param 无
  * @retval 无
  */
static void
commit()
{
  if (log.lh.n > 0) { // 如果当前事务包含待提交的块
    write_log();     // 将缓存中的已修改块写入日志区域
    write_head();    // 将日志头写入磁盘，即正式提交操作
    install_trans(0); // 将日志中的数据安装到实际位置（即写入磁盘的主存储位置）
    log.lh.n = 0; // 清空事务中的块计数，表示事务已完成
    write_head();    // 清除日志头，以擦除日志中的事务信息
  }
}

// 调用者已修改 b->data 并已完成对该缓冲区的操作。
// 记录块号并通过增加引用计数（refcnt）来将其固定在缓存中。
// commit()/write_log() 将会负责执行磁盘写入操作。
//
// log_write() 取代了 bwrite() 的功能；典型的用法如下：
//   bp = bread(...)      // 从磁盘读取一个块
//   修改 bp->data[]      // 对缓冲区数据进行修改
//   log_write(bp)        // 将缓冲区数据写入日志
//   brelse(bp)           // 释放缓冲区

/**
  * @brief 将缓冲区中的数据块加入日志，用于事务提交
  * @param b 缓冲区指针，指向需要记录到日志的块
  * @retval 无
  */
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1) // 确保事务的日志块数量不会超过日志容量限制
    panic("too big a transaction");
  if (log.outstanding < 1) // 确保在事务未结束的情况下调用 log_write
    panic("log_write outside of trans");

  acquire(&log.lock); // 获取日志锁，防止并发修改日志
  for (i = 0; i < log.lh.n; i++) { // 检查该数据块是否已经在日志中，以避免重复记录（日志吸收）
    if (log.lh.block[i] == b->blockno)  // 如果该块号已经在日志中
      break; // 则跳出循环，不需要再添加
  }
  log.lh.block[i] = b->blockno; // 将该块号记录到日志中

  // 如果该块是新的（不在日志中的块），则增加日志块数目
  if (i == log.lh.n) {  // 是否是一个新块？
    bpin(b); // 将缓冲区数据块固定在缓存中，防止其在事务完成前被驱逐
    log.lh.n++; // 增加日志头中的块数
  }
  release(&log.lock); // 释放日志锁
}


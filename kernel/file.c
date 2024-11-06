//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

/**
  * void filewrite()
  * @brief： 将数据写入指定文件f。
  * @param f: 指向要写入的文件结构体的指针。
  * @param addr: 用户虚拟地址，指向要写入的数据的内存地址。
  * @param n: 要写入的字节数。
  * @retval： 返回写入的字节数，成功时返回写入字节数，失败时返回 -1。
  */  
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0) // 检查文件是否可写
    return -1;

  if(f->type == FD_PIPE){ // 如果是管道
    ret = pipewrite(f->pipe, addr, n); // 调用管道写函数
  } else if(f->type == FD_DEVICE){ // 如果是设备文件
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)  // 检查设备号的有效性及其写函数的存在
      return -1; // 如果无效，返回 -1
    ret = devsw[f->major].write(1, addr, n); // 调用设备的写函数
  } else if(f->type == FD_INODE){ // 如果是常规文件（inode）
 
    // 一次写入几个块，以避免超出最大日志事务大小，
    // 包括 i-node、indirect block、allocation blocks，以及用于非对齐写入的 2 个额外块。
    // 这部分逻辑应该放在更底层，因为 writei() 可能会写入像控制台这样的设备。
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE; // 计算单次写入的最大字节数
    int i = 0; // 已写入的字节计数
    while(i < n){ // 只要还有字节需要写入
      int n1 = n - i; // 剩余需要写入的字节数
      if(n1 > max) // 如果剩余字节数超过最大值，限制写入字节数
        n1 = max;

      begin_op(); // 开始文件操作事务
      ilock(f->ip); // 锁定 inode，防止并发修改
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0) // 调用 writei() 将数据写入文件
        f->off += r; // 更新文件偏移量
      iunlock(f->ip); // 解锁 inode
      end_op(); // 结束文件操作事务

      if(r != n1){ // 如果写入字节数不等于请求的字节数
        // writei 返回错误
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1); // 如果所有字节都写入成功，返回 n，否则返回 -1
  } else {
    panic("filewrite"); // 如果文件类型未知，发生 panic
  }

  return ret; // 返回写入的字节数或 -1
}


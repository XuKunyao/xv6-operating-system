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

// 向文件 f 写入数据。
// addr 是用户虚拟地址。
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0; // 声明变量 r 用于存储操作结果，ret 用于记录成功写入的字节数

  // 如果文件不具有可写权限，返回 -1
  if(f->writable == 0)
    return -1;

  // 根据文件类型执行不同的写入操作
  if(f->type == FD_PIPE){
    // 如果文件类型是管道，调用 pipewrite 函数进行写入
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    // 如果文件类型是设备
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      // 检查设备编号是否有效以及该设备是否支持写入
      return -1;
    // 调用设备的写入函数进行写入
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // 如果文件类型是 inode（表示常规文件）
    // 为了避免超过最大日志事务大小，逐块写入
    // 包括 i-node、间接块、分配块，以及用于非对齐写入的 2 块空间
    // 这里 max 是每次写入的最大字节数
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE; 
    int i = 0; // 初始化写入的字节数

    while(i < n){ // 当写入字节数小于 n 时，继续写入
      int n1 = n - i; // 剩余要写入的字节数
      if(n1 > max) // 如果剩余字节数大于 max，调整为 max
        n1 = max;

      begin_op(); // 开始一个新的操作
      ilock(f->ip); // 锁定 inode
      // 写入数据到文件中，writei 返回实际写入的字节数
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r; // 更新文件偏移量
      iunlock(f->ip); // 解锁 inode
      end_op(); // 结束操作

      if(r < 0) // 如果写入失败，退出循环
        break;
      if(r != n1) // 如果实际写入字节数少于预期，触发 panic
        panic("short filewrite");
      i += r; // 增加已写入的字节数
    }
    ret = (i == n ? n : -1); // 如果成功写入的字节数等于 n，设置 ret 为 n，否则设置为 -1
  } else {
    panic("filewrite"); // 如果文件类型不匹配，触发 panic
  }

  return ret; // 返回成功写入的字节数或 -1 表示错误
}



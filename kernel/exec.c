#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

/**
  * int exec(char *path, char **argv)
  * @brief: 加载并执行指定路径的程序
  * @param: path - 程序路径
  * @param: argv - 参数数组
  * @retval: 成功返回参数个数，失败返回 -1
  */
int
exec(char *path, char **argv)
{
  char *s, *last; // 字符串指针
  int i, off; // 循环变量和偏移量
  uint64 argc, sz = 0, sp, ustack[MAXARG+1], stackbase; // 参数计数、程序大小、栈指针、用户栈和栈基地址
  struct elfhdr elf; // ELF头结构
  struct inode *ip; // 索引节点指针
  struct proghdr ph; // 程序头结构
  pagetable_t pagetable = 0, oldpagetable; // 页表和旧页表
  struct proc *p = myproc(); // 获取当前进程

  begin_op(); // 开始文件操作

  if((ip = namei(path)) == 0){ // 通过路径获取文件索引节点
    end_op(); // 结束文件操作
    return -1; // 返回 -1 表示失败
  }
  ilock(ip); // 锁定索引节点

  // 检查 ELF 头
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad; // 如果读取失败，跳转到错误处理
  if(elf.magic != ELF_MAGIC)
    goto bad; // 如果魔数不匹配，跳转到错误处理
  
  // 获取进程的页表
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad; // 如果页表获取失败，跳转到错误处理

  // 将程序加载到内存中
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad; // 如果读取程序头失败，跳转到错误处理
    if(ph.type != ELF_PROG_LOAD)
      continue; // 如果不是加载类型，跳过
    if(ph.memsz < ph.filesz)
      goto bad; // 如果内存大小小于文件大小，跳转到错误处理
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad; // 检查地址溢出
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad; // 如果分配失败，跳转到错误处理
    sz = sz1; // 更新当前大小
    if(ph.vaddr % PGSIZE != 0)
      goto bad; // 如果虚拟地址未对齐，跳转到错误处理
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad; // 加载程序段失败，跳转到错误处理
  }
  iunlockput(ip); // 解锁并释放索引节点
  end_op(); // 结束文件操作
  ip = 0;// 清空索引节点指针

  p = myproc(); // 获取当前进程
  uint64 oldsz = p->sz; // 保存旧的进程大小

  // 在下一个页面边界分配两个页面
  // 使用第二个作为用户栈
  sz = PGROUNDUP(sz); // 向上对齐
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad; // 如果分配失败，跳转到错误处理
  sz = sz1; // 更新当前大小
  uvmclear(pagetable, sz-2*PGSIZE); // 清除栈的第二页
  sp = sz; // 更新栈指针
  stackbase = sp - PGSIZE; // 计算栈基地址

  // 压入参数字符串，并准备用户栈的其余部分
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad; // 如果参数超过最大值，跳转到错误处理
    sp -= strlen(argv[argc]) + 1; // 更新栈指针
    sp -= sp % 16; // RISC-V 栈必须是 16 字节对齐
    if(sp < stackbase)
      goto bad; // 如果栈指针小于基地址，跳转到错误处理
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad; // 如果复制失败，跳转到错误处理
    ustack[argc] = sp; // 保存参数的地址
  }
  ustack[argc] = 0; // 以 null 结尾

  // 压入 argv[] 指针数组
  sp -= (argc+1) * sizeof(uint64); // 更新栈指针
  sp -= sp % 16; // 对齐栈指针
  if(sp < stackbase)
    goto bad; // 如果栈指针小于基地址，跳转到错误处理
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad; // 如果复制失败，跳转到错误处理

  // 将参数传递给用户的 main(argc, argv)
  // argc 通过系统调用返回值返回，存放在 a0 中
  p->trapframe->a1 = sp; // 将栈指针存入 trapframe

  // 保存程序名称以供调试
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1; // 查找最后一个斜杠
  safestrcpy(p->name, last, sizeof(p->name)); // 将程序名称复制到进程结构中
    
  // 提交用户映像
  oldpagetable = p->pagetable; // 保存旧页表
  p->pagetable = pagetable; // 更新页表
  p->sz = sz; // 更新进程大小
  p->trapframe->epc = elf.entry;  // 初始化程序计数器 = main
  p->trapframe->sp = sp; // 初始化栈指针
  proc_freepagetable(oldpagetable, oldsz); // 释放旧页表

  return argc; // 返回参数个数，存入 a0，作为 main(argc, argv) 的第一个参数

 bad: // 错误处理
  if(pagetable)
    proc_freepagetable(pagetable, sz); // 释放页表
  if(ip){
    iunlockput(ip); // 解锁并释放索引节点
    end_op(); // 结束文件操作
  }
  return -1; // 返回 -1 表示失败
}

/**
  * static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
  * @brief: 将程序段加载到指定的虚拟地址
  * @brief: // Load a program segment into pagetable at virtual address va.
            // va must be page-aligned
            // and the pages from va to va+sz must already be mapped.
            // Returns 0 on success, -1 on failure.
  * @param: pagetable - 进程的页表
  * @param: va - 虚拟地址
  * @param: ip - 索引节点指针
  * @param: offset - 偏移量
  * @param: sz - 段大小
  * @retval: 成功返回 0，失败返回 -1
  */
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n; // 循环变量和读取字节数
  uint64 pa; // 物理地址

  if((va % PGSIZE) != 0)
    panic("loadseg: va must be page aligned"); // 如果虚拟地址未对齐，触发错误

  for(i = 0; i < sz; i += PGSIZE){ // 遍历每一页
    pa = walkaddr(pagetable, va + i); // 获取物理地址
    if(pa == 0)
      panic("loadseg: address should exist"); // 如果地址不存在，触发错误
    if(sz - i < PGSIZE)
      n = sz - i; // 计算剩余字节数
    else
      n = PGSIZE; // 每次读取一页
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n) // 从文件中读取数据
      return -1; // 如果读取失败，返回 -1
  }
  
  return 0; // 返回
}

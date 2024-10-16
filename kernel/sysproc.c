#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  backtrace();
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_sigalarm(void)
{
  int ticks; // 接受sigalarm系统调用的第0个参数
  uint64 handler; // 接受系统调用的第1个参数
  if(argint(0, &ticks) < 0) // argint从a0寄存器中获取第一个参数传递给ticks
    return -1;
  if(argaddr(1, &handler) < 0) // argaddr从a1寄存器中获取第二个参数传递给handler
    return -1;

  struct proc *p = myproc(); // 返回当前正在运行的进程结构体指针
  p->ticks = ticks;
  p->ticks_cnt = 0;
  p->handler = handler;
  
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  *p->trapframe = *p->ticks_trapframe; // 恢复上下文，ticks_trapframe存储的是alarm之前的寄存器信息
  p->handler_off = 0; // 关闭handler函数标志位防止重复调用
  return 0;
}
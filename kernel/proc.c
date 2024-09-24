#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];// CPU 结构数组

struct proc proc[NPROC];// 进程结构数组

struct proc *initproc;// 初始化进程指针

int nextpid = 1;// 下一个进程 ID
struct spinlock pid_lock;// PID 锁

extern void forkret(void);// fork 后的返回函数
static void wakeup1(struct proc *chan);// 唤醒单个进程
static void freeproc(struct proc *p);// 释放进程结构

extern char trampoline[]; // trampoline.S

/**
  * void procinit()
  * @brief: 初始化进程表，在引导时调用。
  * @param: NULL
  * @retval: NULL
  */
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");// 初始化 PID 锁
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");// 初始化每个进程的锁

      // 为进程的内核栈分配一个页面。
      // 将其映射到高地址内存，后面跟着一个无效的保护页面。
      char *pa = kalloc();// 分配内存
      if(pa == 0)
        panic("kalloc");// 分配失败则崩溃
      uint64 va = KSTACK((int) (p - proc));// 计算每个进程的内核栈的虚拟地址
      kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);// 将虚拟地址va映射到物理地址pa
      p->kstack = va;// 设置进程的内核栈
  }
  kvminithart();// 初始化虚拟内存
}

/**
  * int cpuid()
  * @brief: 返回当前 CPU 的 ID。
  * @brief: Must be called with interrupts disabled,
            to prevent race with process being moved
            to a different CPU.
  * @param: NULL
  * @retval: 当前 CPU 的 ID。
  */
int
cpuid()
{
  int id = r_tp();
  return id;
}

/**
  * struct cpu* mycpu(void)
  * @brief： 返回当前 CPU 的 cpu 结构
  * @brief： Return this CPU's cpu struct.
             Interrupts must be disabled.
  * @param： NULL
  * @retval： 当前 CPU 的 cpu 结构指针
  */
struct cpu*
mycpu(void) {
  int id = cpuid();// 获取 CPU ID
  struct cpu *c = &cpus[id];// 获取对应的 cpu 结构
  return c;
}

/**
  * struct proc* myproc(void)
  * @brief： 返回当前进程的 proc 结构，如果没有则返回 NULL
  * @brief： Return the current struct proc *, or zero if none.
  * @param： NULL
  * @retval： 当前进程的 proc 结构指针或 NULL
  */
struct proc*
myproc(void) {
  push_off();// 禁用中断
  struct cpu *c = mycpu();// 获取当前 CPU
  struct proc *p = c->proc;// 获取当前进程
  pop_off();// 恢复中断
  return p;
}

/**
  * int allocpid()
  * @brief： 分配一个新的进程 ID
  * @param： NULL
  * @retval： 分配的进程 ID
  */
int
allocpid() {
  int pid;
  
  acquire(&pid_lock);// 获取 PID 锁
  pid = nextpid;// 获取当前下一个 PID
  nextpid = nextpid + 1;// 更新下一个 PID
  release(&pid_lock);// 释放 PID 锁

  return pid;// 返回分配的 PID
}

/**
  * static struct proc* allocproc(void)
  * @brief： 在进程表中查找一个未使用的进程
  * @brief： Look in the process table for an UNUSED proc.
             If found, initialize state required to run in the kernel,
             and return with p->lock held.
             If there are no free procs, or a memory allocation fails, return 0.
  * @param： NULL
  * @retval： 返回找到的进程指针，失败时返回 NULL
  */
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);// 获取进程锁
    if(p->state == UNUSED) {// 如果进程状态为未使用
      goto found;// 跳转到找到的标记
    } else {
      release(&p->lock);// 释放锁
    }
  }
  return 0;// 没有可用进程，返回 NULL

found:
  p->pid = allocpid();// 分配进程 ID

  // 分配一个 trapframe 页
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    release(&p->lock);// 分配失败，释放锁
    return 0;// 返回 NULL
  }

  // 初始化空的用户页表
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 设置新的上下文以开始执行 forkret
  // 该函数返回到用户空间
  memset(&p->context, 0, sizeof(p->context));// 清空上下文
  p->context.ra = (uint64)forkret;// 设置返回地址
  p->context.sp = p->kstack + PGSIZE;// 设置堆栈指针

  return p;// 返回分配的进程
}

/**
  * static void freeproc(struct proc *p)
  * @brief： 释放进程结构及其关联的数据，包括用户页面
  * @brief： free a proc structure and the data hanging from it,
             including user pages.
             p->lock must be held.
  * @param： p：待释放的进程指针
  * @retval： NULL
  */
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe); // 释放 trapframe
  p->trapframe = 0; // 设置为 NULL
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz); // 释放页表
  p->pagetable = 0; // 设置为 NULL
  p->sz = 0; // 用户内存大小设为 0
  p->pid = 0; // 设置 PID 为 0
  p->parent = 0; // 设置父进程为 0
  p->name[0] = 0; // 清空进程名称
  p->chan = 0; // 设置通道为 0
  p->killed = 0; // 设置被杀标志为 0
  p->xstate = 0; // 清空退出状态
  p->state = UNUSED; // 设置状态为未使用
  p->trace_mask = 0; // 清空跟踪掩码
}

/**
  * pagetable_t proc_pagetable(struct proc *p)
  * @brief： 为给定进程创建用户页表，初始无用户内存，但包含 trampoline 页面
  * @brief： Create a user page table for a given process,
             with no user memory, but with trampoline pages.
  * @param： p：目标进程指针
  * @retval： 创建的页表指针
  */
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // 创建空的页表
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // 在最高的用户虚拟地址处映射 trampoline 代码。
  // 仅供管理员使用，在用户空间传递时使用，所以不设置 PTE_U。
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);// 释放页表
    return 0;
  }

  // 将 trapframe 映射到 TRAMPOLINE 下面，为 trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);// 取消映射 trampoline
    uvmfree(pagetable, 0);// 释放页表
    return 0;
  }

  return pagetable;// 返回创建的页表
}

/**
  * void proc_freepagetable(pagetable_t pagetable, uint64 sz)
  * @brief： 释放进程的页表，并释放其引用的物理内存
  * @param： pagetable：待释放的页表指针
  * @param： sz：进程大小
  * @retval： NULL
  */
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);// 取消映射 trampoline
  uvmunmap(pagetable, TRAPFRAME, 1, 0);// 取消映射 trapframe
  uvmfree(pagetable, sz);// 释放页表和引用的物理内存
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

/**
  * void userinit()
  * @brief：设置第一个用户进程
  * @param：NULL
  * @retval：NULL
  */
void
userinit(void)
{
  struct proc *p;

  p = allocproc();// 分配进程结构体
  initproc = p;// 设置初始进程
  
  // 分配一个用户页并将 initcode 的指令和数据拷贝到该页
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;// 设置进程大小为页面大小

  // 准备从内核第一次返回到用户态
  p->trapframe->epc = 0;      // 用户程序计数器设置为0
  p->trapframe->sp = PGSIZE;  // 设置用户栈指针为页面大小

  safestrcpy(p->name, "initcode", sizeof(p->name));// 进程名设为 "initcode"
  p->cwd = namei("/");// 设置当前工作目录为根目录

  p->state = RUNNABLE;// 将进程状态设为可运行

  release(&p->lock);// 释放进程锁
}

/**
  * int growproc(int n)
  * @brief：增加或减少用户内存
  * @param：n：要增加或减少的字节数，正值增加，负值减少
  * @retval：成功返回 0，失败返回 -1
  */
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);// 释放内存
  }
  p->sz = sz;// 更新进程的内存大小
  return 0;// 成功返回 0
}

/**
  * int fork()
  * @brief：创建一个新的子进程，复制父进程
  * @brief：Sets up child kernel stack to return as if from fork() system call.
  * @param：NULL
  * @retval：子进程的 PID，失败返回 -1
  */
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // 分配新进程结构体
  if((np = allocproc()) == 0){
    return -1;
  }

  // 将父进程的用户内存复制到子进程
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  np->parent = p;

  // 复制保存的用户寄存器
  *(np->trapframe) = *(p->trapframe);

  // 在子进程中使 fork 返回 0
  np->trapframe->a0 = 0;
  
  // 在子进程中拷贝trace_mask
  np->trace_mask = p->trace_mask;

  // 增加打开文件的引用计数
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);// 复制当前工作目录

  safestrcpy(np->name, p->name, sizeof(p->name));// 复制进程名

  pid = np->pid;// 获取子进程的 PID

  np->state = RUNNABLE;// 设置子进程为可运行状态

  release(&np->lock);// 释放子进程锁

  return pid;// 返回子进程的 PID
}
/**
  * void reparent(struct proc *p)
  * @brief： 将进程 p 的孤儿进程转交给 init 进程
  * @brief： Pass p's abandoned children to init.
  *          Caller must hold p->lock.
  * @param： p：指向父进程的指针
  * @retval： NULL
  */
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    // 此代码在不持有 pp->lock 的情况下使用 pp->parent。
    // 先获取锁可能会导致死锁
    // 如果 pp 或 pp 的子进程也在 exit() 中
    // 并且即将尝试锁住 p。
    if(pp->parent == p){
      // pp->parent 在检查与获取锁之间不能改变
      // 因为只有父进程会改变它，而我们就是父进程。
      acquire(&pp->lock);
      pp->parent = initproc;// 更改父进程为 init
      // 我们应该在这里唤醒 init，但那会需要
      // initproc->lock，这将是死锁，因为我们持有
      // init 的一个子进程的锁 (pp)。这就是
      // exit() 始终在获取任何锁之前唤醒 init 的原因。
      release(&pp->lock);
    }
  }
}

/**
  * void exit(int status)
  * @brief： 退出当前进程，不返回
  * @brief： An exited process remains in the zombie state
  *          until its parent calls wait().
  * @param： status：进程退出状态
  * @retval： NULL
  */
void
exit(int status)
{
  struct proc *p = myproc();// 获取当前进程

  if(p == initproc)
    panic("init exiting");// 防止 init 进程退出

  // 关闭所有打开的文件。
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);// 关闭文件
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);// 释放当前工作目录
  end_op();
  p->cwd = 0;

  // 我们可能会将一个子进程重新父化为 init。我们无法精确
  // 唤醒 init，因为在获取任何其他进程锁后，我们不能获取
  // 它的锁。因此无论是否需要，都唤醒 init。init 可能会
  // 错过这个唤醒，但这似乎是无害的。
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);// 唤醒 init 进程
  release(&initproc->lock);

  // 获取 p->parent 的副本，以确保我们解锁的
  // 是我们锁定的相同父进程。在等待父进程锁时，
  // 我们的父进程可能会把我们给 init。我们可能会与
  // 一个正在退出的父进程竞争，但结果将是一个无害的
  // 虚假唤醒到一个死掉或错误的进程；进程结构
  // 永远不会被重新分配为其他任何东西。
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // 我们需要父进程的锁以唤醒它
  // 从 wait() 中返回。父-子规则规定我们必须先锁定它。
  acquire(&original_parent->lock);

  acquire(&p->lock);

 // 将任何子进程交给 init。
  reparent(p);// 重新父化子进程

 // 父进程可能在 wait() 中处于休眠状态。
  wakeup1(original_parent);// 唤醒父进程

  p->xstate = status;// 设置退出状态
  p->state = ZOMBIE;// 更改状态为僵尸

  release(&original_parent->lock);

  // 进入调度程序，永不返回。
  sched();
  panic("zombie exit");
}

/**
  * int wait(uint64 addr)
  * @brief： 等待子进程退出并返回其 PID
  * @param： addr：退出状态地址
  * @retval： 子进程的 PID，或 -1 如果没有子进程
  */
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // 在整个过程中保持 p->lock 以避免
  // 来自子进程退出的丢失唤醒。
  acquire(&p->lock);

  for(;;){
    // 扫描表以查找已退出的子进程。
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      // 此代码在不持有 np->lock 的情况下使用 np->parent。
      // 先获取锁将导致死锁，
      // 因为 np 可能是祖先，我们已经持有 p->lock。
      if(np->parent == p){
        // np->parent 在检查与获取锁之间不能改变
        // 因为只有父进程会改变它，而我们就是父进程。
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // 找到一个僵尸进程
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);// 释放进程
          release(&np->lock);
          release(&p->lock);
          return pid;// 返回子进程的 PID
        }
        release(&np->lock);
      }
    }

    // 如果没有子进程就没有必要等待。
    if(!havekids || p->killed){
      release(&p->lock);
      return -1;
    }
    
    // 等待子进程退出。
    sleep(p, &p->lock);  //DOC: wait-sleep
  }
}

/**
  * void scheduler(void)
  * @brief： 每个 CPU 的进程调度程序
  * @param： NULL
  * @retval： NULL
  */

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;// 初始化当前进程
  for(;;){
    // 避免死锁，确保设备可以中断。
    intr_on();
    
    int found = 0;// 标记是否找到可运行的进程
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {// 如果进程状态可运行
        // 切换到选定的进程。进程负责
        // 释放其锁，然后在返回之前重新获取它。
        p->state = RUNNING;// 设置状态为运行
        c->proc = p;// 设置当前进程
        swtch(&c->context, &p->context);// 切换上下文

        // 进程现在完成运行。
        // 它应该在回来之前更改其 p->state。
        c->proc = 0;

        found = 1;// 找到一个可运行的进程
      }
      release(&p->lock);
    }
    if(found == 0) {// 如果没有找到可运行的进程
      intr_on();
      asm volatile("wfi");// 进入等待状态
    }
  }
}

/**
  * void sched(void)
  * @brief： 切换到调度程序，必须持有 p->lock
  * @param： NULL
  * @retval： NULL
  */

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock"); // 锁错误
  if(mycpu()->noff != 1)
    panic("sched locks"); // 锁数量错误
  if(p->state == RUNNING)
    panic("sched running"); // 正在运行的状态错误
  if(intr_get())
    panic("sched interruptible"); // 中断状态错误

  intena = mycpu()->intena; // 保存中断状态
  swtch(&p->context, &mycpu()->context); // 切换上下文
  mycpu()->intena = intena; // 恢复中断状态
}

/**
  * void yield(void)
  * @brief： 放弃 CPU 控制权，以便其他进程可以运行
  * @param： NULL
  * @retval： NULL
  */
void
yield(void)
{
  struct proc *p = myproc(); // 获取当前进程
  acquire(&p->lock); // 获取当前进程锁
  p->state = RUNNABLE; // 设置状态为可运行
  sched(); // 调用调度程序
  release(&p->lock); // 释放锁
}

/**
  * void forkret(void)
  * @brief： fork 子进程首次调度时的返回
  * @brief： A fork child's very first scheduling by scheduler() will swtch to forkret.
  * @param： NULL
  * @retval： NULL
  */
void
forkret(void)
{
  static int first = 1; // 静态变量，标记首次调用

  // 仍然持有 p->lock 来自调度程序。
  release(&myproc()->lock); // 释放当前进程锁

  if (first) {
    // 文件系统初始化必须在常规进程的上下文中运行
    // (例如，因为它调用 sleep)，因此不能从 main() 中运行。
    first = 0;
    fsinit(ROOTDEV); // 初始化文件系统
  }

  usertrapret(); // 返回用户态
}

/**
  * void sleep(void *chan, struct spinlock *lk)
  * @brief： 在指定通道上休眠
  * @brief： Atomically release lock and sleep on chan.
  * @brief： Reacquires lock when awakened.
  * @param： chan：通道指针，lk：自旋锁
  * @retval： NULL
  */
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc(); // 获取当前进程
  
  // 必须获取 p->lock 以更改 p->state 然后调用 sched。
  // 一旦我们持有 p->lock，我们就可以
  // 确保不会错过任何唤醒
  // (wakeup 锁定 p->lock)，
  // 所以释放 lk 是可以的。
  if(lk != &p->lock){  //DOC: sleeplock0
    acquire(&p->lock);  //DOC: sleeplock1
    release(lk); // 释放自旋锁
  }

  // 进入休眠状态
  p->chan = chan; // 设置通道
  p->state = SLEEPING; // 设置状态为休眠

  sched(); // 调用调度程序

  // 清理工作
  p->chan = 0; // 清空通道

  // 重新获取原始锁
  if(lk != &p->lock){
    release(&p->lock); // 释放当前进程锁
    acquire(lk); // 重新获取自旋锁
  }
}

/**
  * void wakeup(void *chan)
  * @brief： 唤醒所有在指定通道上等待的进程
  * @brief： Must be called without any p->lock.
  * @param： chan：通道指针
  * @retval： NULL
  */
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock); // 获取进程锁
    if(p->state == SLEEPING && p->chan == chan) { // 如果进程正在休眠且通道匹配
      p->state = RUNNABLE; // 设置状态为可运行
    }
    release(&p->lock); // 释放锁
  }
}

/**
  * static void wakeup1(struct proc *chan)
  * @brief： 唤醒在指定通道上等待的单个进程
  * @brief： Wake up p if it is sleeping in wait(); used by exit().
             Caller must hold p->lock.
  * @param： chan：通道指针
  * @retval： NULL
  */
static void
wakeup1(struct proc *p)
{
  if(!holding(&p->lock))
    panic("wakeup1"); // 锁错误
  if(p->chan == p && p->state == SLEEPING) {// 如果进程正在休眠且通道匹配
    p->state = RUNNABLE; // 设置状态为可运行
  }
}

/**
  * int kill(int pid)
  * @brief： 杀死具有给定 PID 的进程
  * @brief： The victim won't exit until it tries to return
  * @brief： to user space (see usertrap() in trap.c).
  * @param： pid：进程 ID
  * @retval： 0 成功，-1 失败
  */
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock); // 获取进程锁
    if(p->pid == pid){ // 找到目标进程
      p->killed = 1; // 设置杀死标志
      if(p->state == SLEEPING){
        //从sleep()唤醒进程
        p->state = RUNNABLE; // 设置状态为可运行
      }
      release(&p->lock); // 释放锁
      return 0; // 返回成功
    }
    release(&p->lock);
  }
  return -1; // 返回失败
}

/**
  * int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
  * @brief： 根据用户地址或内核地址复制
  * @brief： Copy to either a user address, or kernel address,depending on usr_dst.
  * @param： user_dst：是否为用户地址，dst：目标地址，src：源地址，len：长度
  * @retval： 0 成功，-1 失败
  */
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc(); // 获取当前进程
  if(user_dst){
    return copyout(p->pagetable, dst, src, len); // 复制到用户地址
  } else {
    memmove((char *)dst, src, len); // 复制到内核地址
    return 0; // 返回成功
  }
}

/**
  * int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
  * @brief： 根据用户地址或内核地址复制
  * @brief： Copy from either a user address, or kernel address,depending on usr_src.
  * @param： dst：目标地址，user_src：是否为用户地址，src：源地址，len：长度
  * @retval： 0 成功，-1 失败
  */
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc(); // 获取当前进程
  if(user_src){
    return copyin(p->pagetable, dst, src, len); // 从用户地址复制
  } else {
    memmove(dst, (char*)src, len); // 从内核地址复制
    return 0;
  }
}

/**
  * void procdump(void)
  * @brief： 打印进程列表，用于调试
  * @brief： Runs when user types ^P on console.
  * @brief： No lock to avoid wedging a stuck machine further.
  * @param： NULL
  * @retval： NULL
  */
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue; // 跳过未使用的进程
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state]; // 获取状态
    else
      state = "???"; // 状态未知
    printf("%d %s %s", p->pid, state, p->name); // 打印进程信息
    printf("\n");
  }
}

/**
  * uint64 acquire_nproc(void)
  * @brief： 获取当前活动进程数量
  * @retval： 活动进程数量
  */
uint64
acquire_nproc(void)
{
  struct proc *p;
  int cnt = 0; // 计数器

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);// 获取进程锁
    if(p->state != UNUSED) {
      cnt++; // 计数
    }
    release(&p->lock); // 释放锁
  }
  return cnt; // 返回活动进程数量
}
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
      uint64 va = KSTACK((int) (p - proc));// 计算内核栈的虚拟地址
      kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);// 映射物理地址到虚拟地址
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

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
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
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  np->parent = p;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;
  
  // 在子进程中拷贝trace_mask
  np->trace_mask = p->trace_mask;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    // this code uses pp->parent without holding pp->lock.
    // acquiring the lock first could cause a deadlock
    // if pp or a child of pp were also in exit()
    // and about to try to lock p.
    if(pp->parent == p){
      // pp->parent can't change between the check and the acquire()
      // because only the parent changes it, and we're the parent.
      acquire(&pp->lock);
      pp->parent = initproc;
      // we should wake up init here, but that would require
      // initproc->lock, which would be a deadlock, since we hold
      // the lock on one of init's children (pp). this is why
      // exit() always wakes init (before acquiring any locks).
      release(&pp->lock);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&original_parent->lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // hold p->lock for the whole time to avoid lost
  // wakeups from a child's exit().
  acquire(&p->lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      // this code uses np->parent without holding np->lock.
      // acquiring the lock first would cause a deadlock,
      // since np might be an ancestor, and we already hold p->lock.
      if(np->parent == p){
        // np->parent can't change between the check and the acquire()
        // because only the parent changes it, and we're the parent.
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&p->lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &p->lock);  //DOC: wait-sleep
  }
}

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
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;

        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      intr_on();
      asm volatile("wfi");
    }
  }
}

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
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  if(lk != &p->lock){  //DOC: sleeplock0
    acquire(&p->lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &p->lock){
    release(&p->lock);
    acquire(lk);
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
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
    }
    release(&p->lock);
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
    panic("wakeup1");
  if(p->chan == p && p->state == SLEEPING) {
    p->state = RUNNABLE;
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
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
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

uint64
acquire_nproc(void)
{
  struct proc *p;
  int cnt = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != UNUSED) {
      cnt++;
    }
    release(&p->lock);
  }
  return cnt;
}
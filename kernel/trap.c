#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){  // 如果是设备中断，则标记设备中断已处理
    // ok
  } else if(r_scause() == 13 || r_scause() == 15){ // 如果 `scause` 表示页错误（13 或 15），并且是写时拷贝 (COW) 引发的页错误
    uint64 va = r_stval();  // 从r_stval寄存器获取触发页错误的虚拟地址
    if(is_cow_fault(p->pagetable, va)){ // 检查该页是否为 COW 页
      if(cow_alloc(p->pagetable, va) < 0){ // 如果是 COW 页，调用cow_alloc分配新的物理页并复制内容
        printf("usertrap():cow_alloc failed!"); // 如果分配失败，打印错误信息并杀掉进程
        p->killed = 1;
      }
    } else{ // 如果不是 COW 页，打印错误信息并杀掉进程
      printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->killed = 1;
    }
  } else { // 处理未预期的异常情况，打印调试信息并终止进程
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// 检查是否是外部中断或软件中断，并进行处理。
// 如果是定时器中断，则返回 2；
// 如果是其他设备中断，则返回 1；
// 如果未识别中断，则返回 0
int
devintr()
{
  uint64 scause = r_scause(); // 读取中断原因寄存器 scause

  if((scause & 0x8000000000000000L) && // 检查 scause 寄存器的高位是否为 1，并且低 8 位是否为 9，
     (scause & 0xff) == 9){ // 表示这是来自 PLIC 的 supervisor 外部中断

    int irq = plic_claim();  // irq 表示哪个设备发出了中断

    if(irq == UART0_IRQ){ // 根据中断号处理对应的设备中断
      uartintr(); // 处理 UART 中断
    } else if(irq == VIRTIO0_IRQ){ 
      virtio_disk_intr(); // 处理 VirtIO 磁盘中断
    } else if(irq){ // 如果是意外的中断，打印错误信息
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // PLIC 允许每个设备在同一时间最多只引发一个中断；
    // 通知 PLIC 该设备现在可以再次产生中断
    if(irq)
      plic_complete(irq);

    return 1; // 返回 1，表示处理了一个设备中断
  } else if(scause == 0x8000000000000001L){ // 检查是否为来自机器模式定时器中断的软件中断
    // 处理来自 timervec in kernelvec.S的软件中断

    if(cpuid() == 0){
      clockintr(); // 仅在 CPU 0 上处理时钟中断
    }
    
    w_sip(r_sip() & ~2); // 通过清除 sip 中的 SSIP 位来确认软件中断

    return 2; // 返回 2，表示处理了定时器中断
  } else {
    return 0; // 返回 0，表示未识别的中断
  }
}


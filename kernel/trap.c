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

// 设置内核中断处理函数以接收异常和中断。
void
trapinithart(void)
{
  // 设置异常向量寄存器的值为kernelvec函数的地址。
  w_stvec((uint64)kernelvec);
}

//
// 处理来自用户空间的中断、异常或系统调用。
// 此函数从trampoline.S中调用
//
void
usertrap(void)
{
  // 初始化设备中断标识
  int which_dev = 0;

  // 检查当前的处理状态是否是来自用户模式，如果不是，则触发panic
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // 由于现在处于内核态，因此将中断和异常的处理向量设置为kerneltrap
  w_stvec((uint64)kernelvec);

  // 获取当前进程的结构体指针
  struct proc *p = myproc();
  
  // 保存用户程序计数器的值，以便恢复时可以继续执行
  p->trapframe->epc = r_sepc();
  
  // 检查异常原因寄存器的值是否为8（表示系统调用）
  if(r_scause() == 8){
    // 处理系统调用

    // 如果进程已经被标记为被终止，则退出
    if(p->killed)
      exit(-1);

    // sepc寄存器指向ecall指令，但我们需要返回到下一条指令
    p->trapframe->epc += 4;

    // 在处理中断时会修改sstatus及相关寄存器，因此在此之前不启用中断
    intr_on();

    // 调用系统调用处理函数
    syscall();
  } else if((which_dev = devintr()) != 0){
    // 如果是设备中断，则标记设备中断已处理
    // ok
  } else {
    // 处理未预期的异常情况，打印调试信息并终止进程
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  // 如果进程被标记为已终止，则退出
  if(p->killed)
    exit(-1);

  // 如果这是一个计时器中断，则放弃CPU以允许其他进程运行
  if(which_dev == 2){
    if(p->ticks > 0){
      p->ticks_cnt ++; // 用于跟踪自上一次调用（或直到下一次调用）到进程的报警处理程序间经历了多少滴答
      if(p->ticks_cnt > p->ticks && p->handler_off == 0){ // 达到规定的ticks个时间间隔且handler未被使用
        p->ticks_cnt = 0; // ticks_cnt归零
        *p->ticks_trapframe = *p->trapframe; // 保存现场用于之后恢复
        p->handler_off = 1; // 设置标志位表示此时正在使用handler函数，避免重复调用
        p->trapframe->epc = p->handler; // epc为用户进程的pc，设置为handler后用户下一步将会执行handler函数
      }
    }
    yield();
  }
  // 返回用户态
  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // 我们即将把中断处理的目标从 kerneltrap() 切换到 usertrap()，
  // 因此在回到用户空间之前关闭中断，在用户空间中 usertrap() 才是正确的
  intr_off();

  // 将系统调用、中断和异常发送到 trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // 设置在进程下次重新进入内核时uservec 将要使用的 trapframe 的值
  p->trapframe->kernel_satp = r_satp();         // 内核页表
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // 进程的内核栈
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid 用于 cpuid()

  // 设置 trampoline.S 的 sret 指令会用到的寄存器，以便跳转到用户空间
  
  // 将 S 上次特权模式（S Previous Privilege）设置为用户模式
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // 将 SPP 清零，表示进入用户模式
  x |= SSTATUS_SPIE; // 在用户模式下启用中断
  w_sstatus(x);

  // 将 S 异常程序计数器（S Exception Program Counter）设置为保存的用户程序计数器（user pc）
  w_sepc(p->trapframe->epc);

  // 告诉 trampoline.S 需要切换到的用户页表 
  uint64 satp = MAKE_SATP(p->pagetable);

  // 跳转到位于内存顶端的 trampoline.S，
  // 它会切换到用户页表，恢复用户寄存器，
  // 并通过 sret 切换到用户模式
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

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}


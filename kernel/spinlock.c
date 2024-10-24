// 互斥自旋锁（Mutual exclusion spin locks）

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

// 初始化自旋锁
// 参数：lk 是要初始化的锁，name 是锁的名字，用于调试
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name; // 为锁设置名称
  lk->locked = 0; // 将锁设置为未锁定状态
  lk->cpu = 0; // 当前持有锁的 CPU 为空（无 CPU 持有）
}

// 获取锁
// 自旋（不断循环）直到锁被获取
void
acquire(struct spinlock *lk)
{
  push_off(); // 关闭中断，以避免死锁
  if(holding(lk)) // 如果当前 CPU 已经持有锁，则抛出 panic 错误
    panic("acquire");

  // 在 RISC-V 架构上，sync_lock_test_and_set 会变成一个原子交换操作：
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  // 上述汇编指令表示将 a5 中的值（1）与 lk->locked 交换，
  // 如果返回值是 0，则表示成功获取锁；如果返回非 0，则表示锁已被其他进程持有，需要继续等待
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // 告诉 C 编译器和处理器不要将内存读取或存储操作移到这条指令之前，
  // 确保关键区的内存操作在获取锁之后才发生
  // 在 RISC-V 上，这会生成一个 fence 指令
  __sync_synchronize();

  // 记录锁的持有者信息，供 holding() 函数和调试使用
  lk->cpu = mycpu();
}

// 释放锁
void
release(struct spinlock *lk)
{
  if(!holding(lk)) // 如果当前 CPU 没有持有该锁，则抛出 panic 错误
    panic("release");

  lk->cpu = 0; // 清除锁的持有者信息

  // 告诉 C 编译器和 CPU 不要将内存读取或存储操作移到这条指令之前，
  // 确保关键区的所有存储操作在释放锁之前对其他 CPU 可见，
  // 并且关键区内的所有加载操作在锁释放之前完成
  // 在 RISC-V 上，这会生成一个 fence 指令
  __sync_synchronize();

  // 释放锁，相当于将 lk->locked 设置为 0
  // 这段代码没有使用 C 语言的赋值操作，因为 C 标准暗示赋值可能会被实现为多个存储指令
  // 在 RISC-V 架构上，sync_lock_release 变成了一个原子交换操作：
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  // 将零值存储到 lk->locked 中，释放锁
  __sync_lock_release(&lk->locked);

  pop_off(); // 恢复中断状态
}

// 检查当前 CPU 是否持有该锁
// 中断必须处于关闭状态
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu()); // 判断锁是否已锁定，且当前 CPU 是否持有该锁
  return r;
}

// push_off/pop_off 和 intr_off()/intr_on() 类似，但它们是成对操作的：
// 两次 pop_off() 必须对应两次 push_off()
// 如果一开始中断是关闭的，那么 push_off() 和 pop_off() 不会影响中断状态

// 关闭中断，并记录之前的中断状态
void
push_off(void)
{
  int old = intr_get(); // 获取当前的中断状态（是否打开）

  intr_off(); // 关闭中断
  if(mycpu()->noff == 0) // 如果当前 CPU 没有禁用任何中断，则保存之前的中断状态
    mycpu()->intena = old;
  mycpu()->noff += 1; // 计数禁用中断的次数
}

void
pop_off(void) // 恢复中断状态
{
  struct cpu *c = mycpu(); 
  if(intr_get()) // 如果中断状态在恢复期间意外打开，则抛出 panic 错误
    panic("pop_off - interruptible");
  if(c->noff < 1) // 如果计数器低于 1，意味着 pop_off 操作不匹配，抛出 panic 错误
    panic("pop_off");
  c->noff -= 1; // 减少禁用中断的计数器
  if(c->noff == 0 && c->intena) // 当所有 push_off() 操作都被取消，并且之前的中断状态为打开时，重新打开中断
    intr_on();
}

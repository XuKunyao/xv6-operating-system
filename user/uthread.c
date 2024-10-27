#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2

#define STACK_SIZE  8192 // 定义线程的栈大小为 8192 字节
#define MAX_THREAD  4    // 最大线程数量设定为 4


struct thread {
  uint64 ra;
  uint64 sp;
  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;

  char       stack[STACK_SIZE]; /* 每个线程的栈空间 */
  int        state;             /* FREE, RUNNING, RUNNABLE */

};
struct thread all_thread[MAX_THREAD]; // all_thread 数组存储最多 4 个线程
struct thread *current_thread; // 指针指向当前正在运行的线程
extern void thread_switch(uint64, uint64); // 外部汇编函数，用于在线程之间进行上下文切换
              
void 
thread_init(void)
{
  // main() 是线程 0，将第一次调用 thread_schedule()
  // 它需要一个堆栈，以便第一次 thread_switch() 可以保存线程 0 的状态。
  // thread_schedule() 将不会再次运行主线程，因为它的状态被设置为 RUNNING，
  // 并且 thread_schedule() 只会选择 RUNNABLE（可运行的）线程
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* 寻找另一个可运行的线程 */
  next_thread = 0;
  t = current_thread + 1; // 从当前线程后面开始查找
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD) // 如果超过数组末尾则从头开始
      t = all_thread;
    if(t->state == RUNNABLE) { // 如果找到 RUNNABLE 状态的线程
      next_thread = t; // 将其作为下一个线程
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) { // 如果没有找到可运行的线程，则退出
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* 是否需要切换线程?  */
    next_thread->state = RUNNING; // 将下一个线程设置为 RUNNING
    t = current_thread; // 保存当前线程
    current_thread = next_thread; // 切换当前线程指针
    /* YOUR CODE HERE
     * 使用 thread_switch 函数在 t 和 next_thread 之间切换
     * thread_switch 负责保存当前线程的寄存器状态并加载下一个线程的寄存器状态
     */
    thread_switch((uint64)t, (uint64)next_thread);
  } else
    next_thread = 0; // 如果没有线程切换，设置 next_thread 为 0
}

/* 创建一个新线程并将其置为可运行状态 */
void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) { // 查找一个空闲线程槽
    if (t->state == FREE) break; // 找到空闲线程槽
  }
  t->state = RUNNABLE; // 将找到的线程槽状态设置为 RUNNABLE
  // YOUR CODE HERE
  // 这里可以设置栈和寄存器初始化，将 func 的地址作为线程的入口地址
  t->ra = (uint64)func;//令返回地址寄存器保存测试函数入口地址，从thread_switch返回时可切换到对应线程
  t->sp = (uint64) (t->stack + STACK_SIZE);//为线程准备的栈
}

/* 将当前线程的状态改为 RUNNABLE 并让出 CPU */
void 
thread_yield(void)
{
  current_thread->state = RUNNABLE; // 将当前线程状态设置为 RUNNABLE
  thread_schedule(); // 调用调度器查找并切换到其他线程
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

void 
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while(b_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

int 
main(int argc, char *argv[]) 
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  thread_schedule();
  exit(0);
}

// 实现物理内存分配器，用于用户进程、内核栈、页表页和管道缓冲区。
// 分配整个4096字节的页面。

#include "types.h" // 包含类型定义
#include "param.h" // 包含参数定义
#include "memlayout.h" // 包含内存布局定义
#include "spinlock.h" // 包含自旋锁定义
#include "riscv.h" // 包含RISC-V架构相关定义
#include "defs.h" // 包含通用定义

void freerange(void *pa_start, void *pa_end);

extern char end[]; // 内核结束后的第一个地址，由kernel.ld定义。

// 定义链表节点结构run，用于记录空闲内存页
struct run {
  struct run *next; // 指向下一空闲页
};

// 定义管理内存分配的结构kmem，包含一个自旋锁和一个空闲内存页的链表
struct {
  struct spinlock lock; // 用于保护链表的锁
  struct run *freelist; // 空闲内存页的链表
} kmem;

/**
  * void kinit()
  * @brief： 初始化内存分配器
  * @param： NULL
  * @retval：NULL
  */
void
kinit()
{
  initlock(&kmem.lock, "kmem"); // 初始化kmem的锁
  freerange(end, (void*)PHYSTOP); // 释放从内核结束地址end之后直到物理内存终止地址PHYSTOP的这段内存页
}

/**
  * void freerange(void *pa_start, void *pa_end)
  * @brief： 释放从 pa_start 到 pa_end 范围内的物理内存页
  * @param： pa_start - 内存范围的起始地址
  * @param： pa_end - 内存范围的结束地址
  * @retval： NULL
  */
void
freerange(void *pa_start, void *pa_end)
{
  char *p; // 指针p用于迭代内存页
  p = (char*)PGROUNDUP((uint64)pa_start); // 将起始地址向上取整至页边界，以确保从完整的页开始释放
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) // 遍历每个页，直到结束地址
    kfree(p); // 释放当前页并加入到空闲链表中
}

/**
  * void kfree(void *pa)
  * @brief： 释放一页物理内存
  * @param： pa - 要释放的物理内存页的起始地址
  * @retval： NULL
  */
void
kfree(void *pa)
{
  struct run *r; // 定义运行链表节点

  // 检查页面地址是否按页大小对齐，且地址在合法范围内
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree"); // 不符合条件则触发panic

  memset(pa, 1, PGSIZE);// 用垃圾数据填充，用于在调试时发现潜在的内存问题（例如悬挂指针）

  r = (struct run*)pa; // 将pa强制转换为run结构体指针

  acquire(&kmem.lock); // 获取锁以保护共享资源
  r->next = kmem.freelist; // 新释放的节点指向当前空闲链表头
  kmem.freelist = r; // 链表头更新为新释放节点
  release(&kmem.lock); // 释放锁
}

/**
  * void *kalloc()
  * @brief： 分配一个 4096 字节的物理页
  * @param： NULL
  * @retval： 返回分配的物理页的指针，若分配失败返回 NULL
  */
void *
kalloc(void)
{
  struct run *r; // 定义运行链表节点

  acquire(&kmem.lock); // 获取锁以保护共享资源
  r = kmem.freelist; // 从空闲链表中取出一个页面
  if(r)
    kmem.freelist = r->next; // 更新空闲链表头为下一个空闲页
  release(&kmem.lock); // 释放锁

  if(r)
    memset((char*)r, 5, PGSIZE); // 用垃圾数据5填充，也是为了捕获潜在的内存问题
  return (void*)r; // 返回分配的内存页
}

/**
  * uint64 acquire_freemem()
  * @brief： 计算当前系统中剩余的空闲内存量
  * @param： NULL
  * @retval： 返回剩余的空闲内存大小（单位为字节）
  */
uint64
acquire_freemem(){
  struct run *r; // 定义运行链表节点
  uint64 cnt = 0; // 用于计数空闲页数量

  acquire(&kmem.lock); // 获取锁以保护共享资源
  r = kmem.freelist; // 从空闲链表头开始
  while(r){ // 遍历空闲链表
    r = r->next; // 移动到下一个节点
    cnt++; // 增加计数
  }
  release(&kmem.lock); // 释放锁

  return cnt * 4096; // 返回总空闲字节数
}
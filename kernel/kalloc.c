// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  char *ref_page; // 记录每一页的引用计数
  int page_cnt; // 记录页的总数
  char *end_; // 调试发现不能将end当成一个指针进行加减操作，所以重新定义一个end_(end是一个不完全类型)
} kmem;


/**
  * int pagecnt(void *pa_start, void *pa_end)
  * @brief： 计算指定物理内存范围内的页数量
  * @brief： 模仿freerange()函数
  * @param： pa_start - 内存范围的起始地址
  * @param： pa_end - 内存范围的结束地址
  * @retval：最后返回计算得到的页数cnt
  */
int
pagecnt(void *pa_start, void *pa_end)
{
  char *p;
  int cnt = 0;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    cnt ++;
  return cnt;
}

/**
  * void kinit()
  * @brief： 初始化内存分配器
  * @param： NULL
  * @retval：NULL
  */
void
kinit()
{
  initlock(&kmem.lock, "kmem"); // 初始化锁
  kmem.page_cnt = pagecnt(end, (void *)PHYSTOP); // 计算从end到PHYSTOP一共多少页
  kmem.ref_page = end; // ref_page指向引用计数的起始位置(这里的end是在开头extern char end[]定义的)
  for(int i = 0; i < kmem.page_cnt; ++i){
    kmem.ref_page[i] = 0; // 初始化从end到PHYSTOP每一页的引用计数为0
  }
  kmem.end_ = kmem.ref_page + kmem.page_cnt; // 记录引用计数的结束位置
  freerange(kmem.end_, (void*)PHYSTOP); // 释放end_到PHYSTOP之间的物理内存
}

/**
  * int page_index(uint64 pa)
  * @brief： 计算给定物理地址对应的页面索引，即第几页
  * @param： 物理地址pa
  * @retval：页面索引res
  */
int
page_index(uint64 pa){
  pa = PGROUNDDOWN(pa); // 将地址向下对齐到页边界
  int res = (pa - (uint64)kmem.end_) / PGSIZE; // 计算页面索引
  if (res < 0 || res >= kmem.page_cnt){ // 如果页面索引小于0或者大于最大页面数则panic
    panic("page_index illegal!"); 
  }
  return res;
}

// 引用计数增加
void
incr(void *pa){
  int index = page_index((uint64)pa);
  acquire(&kmem.lock);
  kmem.ref_page[index]++;
  release(&kmem.lock);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa) {
  int index = page_index((uint64)pa);
  acquire(&kmem.lock);
  if (kmem.ref_page[index] > 1) { // 若引用计数值大于1，此时不能直接释放物理内存
    kmem.ref_page[index]--; // 引用计数值减1后直接返回，不需要继续执行下面的释放内存操作
    release(&kmem.lock); // 保证返回前先释放掉锁
    return;
  } 
  if (kmem.ref_page[index] == 1) { // 若引用计数值等于1，证明没有子进程同时使用该页面
    kmem.ref_page[index]--; // 引用计数值减1后继续执行下面的释放物理内存操作
  }
  
  // 检查是否是有效的物理地址
  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP) {
    panic("kfree");
  }

  // 填充以捕获悬空引用
  memset(pa, 1, PGSIZE);
  
  struct run *r = (struct run*)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    incr(r); // 设置引用计数初始值
  }
  return (void*)r;
}

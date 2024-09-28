#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * 内核的页表
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld 设置此值为内核代码的结束位置

extern char trampoline[]; // trampoline.S

/*
 * 为内核创建一个直接映射的页
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc(); // 分配内存以存储内核页表
  memset(kernel_pagetable, 0, PGSIZE); // 将分配的内存清零

  // uart寄存器
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W); // 将UART0映射到页表

  // virtio mmio磁盘接口
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W); // 将VIRTIO0映射到页表

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W); // 将CLINT映射到页表

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W); // 将PLIC映射到页表

  // 映射内核文本（可执行且只读）
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X); // 将内核代码映射

  // 映射内核数据和我们将使用的物理内存RAM
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W); // 映射内核数据区

  // 为陷阱入口/出口映射trampoline到内核中的最高虚拟地址
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X); // 映射trampoline代码
}

/**
  * void kvminithart()
  * @brief： 切换到内核页表并启用分页。
  * @brief： Switch h/w page table register to the kernel's page table,and enable paging.
  * @param： 无
  * @retval： 无
  */
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));  // 设置页表寄存器为内核页表。
  sfence_vma();  // 刷新虚拟地址映射。
}

/**
  * pte_t * walk(pagetable_t pagetable, uint64 va, int alloc)
  * @brief： 查找虚拟地址va在页表中的页表项（PTE）。
  * @brief： // Return the address of the PTE in page table pagetable
             // that corresponds to virtual address va.  If alloc!=0,
             // create any required page-table pages.
             //
             // The risc-v Sv39 scheme has three levels of page-table
             // pages. A page-table page contains 512 64-bit PTEs.
             // A 64-bit virtual address is split into five fields:
             //   39..63 -- must be zero.
             //   30..38 -- 9 bits of level-2 index.
             //   21..29 -- 9 bits of level-1 index.
             //   12..20 -- 9 bits of level-0 index.
             //    0..11 -- 12 bits of byte offset within the page.
  * @param： pagetable - 页表； va - 虚拟地址； alloc - 是否分配新的页表页。
  * @retval： 对应PTE的地址，若未找到则返回0。
  */
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA) // 检查虚拟地址是否超出最大值
    panic("walk"); // 报错并终止

  for(int level = 2; level > 0; level--) { // 遍历页表层级
    pte_t *pte = &pagetable[PX(level, va)]; // 获取当前层级的PTE
    if(*pte & PTE_V) { // 如果PTE有效
      pagetable = (pagetable_t)PTE2PA(*pte); // 进入下一层页表
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0) // 如果需要分配且分配失败
        return 0; // 返回0表示未找到
      memset(pagetable, 0, PGSIZE); // 清空分配的页表页
      *pte = PA2PTE(pagetable) | PTE_V; // 设置PTE为新分配的页表页并标记为有效
    }
  }
  return &pagetable[PX(0, va)]; // 返回最终层级的PTE
}

/**
  * uint64 walkaddr(pagetable_t pagetable, uint64 va)
  * @brief： 查找虚拟地址va对应的物理地址。
  * @brief： Can only be used to look up user pages.
  * @param： pagetable - 页表； va - 虚拟地址。
  * @retval： 物理地址，若未映射则返回0。
  */
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte; // 页表项指针
  uint64 pa; // 物理地址

  if(va >= MAXVA) // 检查虚拟地址是否超出最大值
    return 0; // 返回0表示无效

  pte = walk(pagetable, va, 0);  // 查找对应的PTE。
  if(pte == 0)  // 如果未找到PTE。
    return 0;  // 返回0表示无效。
  if((*pte & PTE_V) == 0)  // 如果PTE无效。
    return 0;  // 返回0表示无效。
  if((*pte & PTE_U) == 0)  // 如果PTE未标记为用户可访问。
    return 0;  // 返回0表示无效。
  pa = PTE2PA(*pte);  // 获取物理地址。
  return pa;  // 返回物理地址。
}

/**
  * void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
  * @brief： 向内核页表添加映射，仅在引导时使用。
  * @brief： // add a mapping to the kernel page table.
             // only used when booting.
             // does not flush TLB or enable paging.
  * @param： va - 虚拟地址； pa - 物理地址； sz - 映射大小； perm - 权限。
  * @retval： 无
  */
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0) // 调用mappages进行映射
    panic("kvmmap"); // 如果映射失败，则报错并终止
}

/**
  * uint64 kvmpa(uint64 va)
  * @brief： 将内核虚拟地址转换为物理地址，仅在堆栈上需要。
  * @brief： // translate a kernel virtual address to
             // a physical address. only needed for
             // addresses on the stack.
             // assumes va is page aligned.
  * @param： va - 虚拟地址。
  * @retval： 物理地址。
  */
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE; // 计算页内偏移
  pte_t *pte; // 页表项指针
  uint64 pa; // 物理地址
  
  pte = walk(kernel_pagetable, va, 0); // 查找PTE
  if(pte == 0)
    panic("kvmpa"); // 如果未找到PTE，则报错并终止
  if((*pte & PTE_V) == 0)
    panic("kvmpa"); // 如果PTE无效，则报错并终止
  pa = PTE2PA(*pte); // 获取物理地址
  return pa+off; // 返回物理地址加上页内偏移
}

/**
  * int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
  * @brief： 为虚拟地址va开始的区域创建PTE，映射到物理地址pa。
  * @brief： // Create PTEs for virtual addresses starting at va that refer to
             // physical addresses starting at pa. va and size might not
             // be page-aligned. Returns 0 on success, -1 if walk() couldn't
             // allocate a needed page-table page.
  * @param： pagetable - 页表； va - 虚拟地址； size - 大小； pa - 物理地址； perm - 权限。
  * @retval： 成功返回0，失败返回-1。
  */
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last; // a为当前地址，last为最后地址
  pte_t *pte; // 页表项指针

  a = PGROUNDDOWN(va); // 将虚拟地址向下取整到页面边界
  last = PGROUNDDOWN(va + size - 1); // 计算最后一个页面的边界
  for(;;){ // 无限循环，直到所有页面映射完成
    if((pte = walk(pagetable, a, 1)) == 0) // 查找当前地址的PTE
      return -1; // 如果未找到，返回-1
    if(*pte & PTE_V) // 如果PTE已经有效
      panic("remap"); // 报错并终止
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

/**
  * void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
  * @brief: 从虚拟地址 va 开始移除 npages 页的映射。
  * @brief: // Remove npages of mappings starting from va. va must be
            // page-aligned. The mappings must exist.
            // Optionally free the physical memory.
  * @param: pagetable - 进程的页表
  * @param: va - 起始虚拟地址
  * @param: npages - 要移除的页数
  * @param: do_free - 是否释放物理内存的标志
  * @retval: 无返回值
  */
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a; // 用于遍历的地址变量
  pte_t *pte; // 页表项指针

  if((va % PGSIZE) != 0) // 检查虚拟地址是否对齐
    panic("uvmunmap: not aligned"); // 如果未对齐，触发错误
  
  // 遍历要移除的每一页
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // 获取页表项
      panic("uvmunmap: walk"); // 如果页表项不存在，触发错误
    if((*pte & PTE_V) == 0) // 检查该页是否被映射
      panic("uvmunmap: not mapped"); // 如果未映射，触发错误
    if(PTE_FLAGS(*pte) == PTE_V) // 检查是否为叶节点
      panic("uvmunmap: not a leaf"); // 如果不是叶节点，触发错误
    if(do_free) { // 如果需要释放物理内存
      uint64 pa = PTE2PA(*pte); // 获取物理地址
      kfree((void*)pa); // 释放物理内存
    }
    *pte = 0; // 清空页表项
  }
}

/**
  * pagetable_t uvmcreate()
  * @brief: 创建一个空的用户页表。
  * @retval: 返回页表指针，如果内存不足则返回 0
  */
pagetable_t
uvmcreate()
{
  pagetable_t pagetable; // 定义页表变量
  pagetable = (pagetable_t) kalloc(); // 分配内存
  if(pagetable == 0) // 检查是否分配成功
    return 0; // 如果失败，返回 0
  memset(pagetable, 0, PGSIZE); // 初始化页表
  return pagetable; // 返回页表指针
}

/**
  * void uvminit(pagetable_t pagetable, uchar *src, uint sz)
  * @brief: 将用户初始化代码加载到页表的地址 0。
  * @brief: // Load the user initcode into address 0 of pagetable,
            // for the very first process.
            // sz must be less than a page.
  * @param: pagetable - 进程的页表
  * @param: src - 源初始化代码地址
  * @param: sz - 代码大小，必须小于一页
  * @retval: 无返回值
  */
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem; // 用于存储分配的内存

  if(sz >= PGSIZE) // 检查代码大小是否超过一页
    panic("inituvm: more than a page"); // 如果超过，触发错误
  mem = kalloc(); // 分配内存
  memset(mem, 0, PGSIZE); // 初始化分配的内存
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U); // 映射页表
  memmove(mem, src, sz); // 将初始化代码复制到分配的内存
}

/**
  * uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
  * @brief: 从 oldsz 增长到 newsz，分配页表项和物理内存。
  * @brief: // Allocate PTEs and physical memory to grow process from oldsz to
            // newsz, which need not be page aligned.  Returns new size or 0 on error.
  * @param: pagetable - 进程的页表
  * @param: oldsz - 原始大小
  * @param: newsz - 新的大小
  * @retval: 返回新的大小，如果出错则返回 0
  */
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem; // 用于存储分配的内存
  uint64 a; // 用于遍历的地址变量

  if(newsz < oldsz) // 如果新大小小于旧大小，返回旧大小
    return oldsz;

  oldsz = PGROUNDUP(oldsz); // 将旧大小向上对齐到页面大小
  for(a = oldsz; a < newsz; a += PGSIZE) { // 遍历新大小
    mem = kalloc(); // 分配内存
    if(mem == 0) { // 检查内存是否分配成功
      uvmdealloc(pagetable, a, oldsz); // 如果失败，撤销分配
      return 0; // 返回 0 表示错误
    }
    memset(mem, 0, PGSIZE); // 初始化分配的内存
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0) { // 映射到页表
      kfree(mem); // 如果映射失败，释放内存
      uvmdealloc(pagetable, a, oldsz); // 撤销分配
      return 0; // 返回 0 表示错误
    }
  }
  return newsz; // 返回新的大小
}

/**
  * uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
  * @brief: 解除分配用户页面，将进程大小从 oldsz 变更为 newsz。
  * @brief: // Deallocate user pages to bring the process size from oldsz to
            // newsz.  oldsz and newsz need not be page-aligned, nor does newsz
            // need to be less than oldsz.  oldsz can be larger than the actual
            // process size.  Returns the new process size.
  * @param: pagetable - 进程的页表
  * @param: oldsz - 原始大小
  * @param: newsz - 新的大小
  * @retval: 返回新的进程大小
  */
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz) // 如果新大小大于等于旧大小，直接返回旧大小
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){ // 如果新的大小小于旧的大小，解除分配
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE; // 计算需要解除分配的页数
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1); // 解除映射
  }

  return newsz; // 返回新的大小
}

/**
  * void freewalk(pagetable_t pagetable)
  * @brief: 递归释放页表页面，所有叶节点映射必须已被移除。
  * @brief: // Recursively free page-table pages.
            // All leaf mappings must already have been removed.
  * @param: pagetable - 需要释放的页表
  * @retval: 无返回值
  */
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){// 遍历页表
    pte_t pte = pagetable[i]; // 获取当前页表项
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){ // 检查是否为有效页表项且不是叶节点
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte); // 获取子页表的物理地址
      freewalk((pagetable_t)child); // 递归释放子页表
      pagetable[i] = 0; // 清空当前页表项
    } else if(pte & PTE_V) { // 如果当前页表项有效
      panic("freewalk: leaf"); // 如果是叶节点，触发错误
    }
  }
  kfree((void*)pagetable); // 释放当前页表
}

/**
  * void uvmfree(pagetable_t pagetable, uint64 sz)
  * @brief: 释放用户内存页面，然后释放页表页面。
  * @param: pagetable - 进程的页表
  * @param: sz - 需要释放的大小
  * @retval: 无返回值
  */
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0) // 如果大小大于 0，解除映射
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable); // 释放页表
}

/**
  * int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
  * @brief: 将父进程的页表复制到子进程，复制内存和页表。
  * @brief: // Given a parent process's page table, copy
            // its memory into a child's page table.
            // Copies both the page table and the
            // physical memory.
            // returns 0 on success, -1 on failure.
            // frees any allocated pages on failure.
  * @param: old - 父进程的页表
  * @param: new - 子进程的页表
  * @param: sz - 需要复制的大小
  * @retval: 成功返回 0，失败返回 -1
  */
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte; // 页表项指针
  uint64 pa, i; // 物理地址和循环变量
  uint flags; // 页表项标志
  char *mem; // 用于存储分配的内存

  for(i = 0; i < sz; i += PGSIZE) { // 遍历每一页
    if((pte = walk(old, i, 0)) == 0) // 获取父进程的页表项
      panic("uvmcopy: pte should exist"); // 如果不存在，触发错误
    if((*pte & PTE_V) == 0) // 检查页表项是否有效
      panic("uvmcopy: page not present"); // 如果无效，触发错误
    pa = PTE2PA(*pte); // 获取物理地址
    flags = PTE_FLAGS(*pte); // 获取页表项标志
    if((mem = kalloc()) == 0) // 分配内存
      goto err; // 如果失败，跳转到错误处理
    memmove(mem, (char*)pa, PGSIZE); // 复制物理内存到新分配的内存
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) { // 映射到子进程页表
      kfree(mem); // 如果映射失败，释放内存
      goto err; // 跳转到错误处理
    }
  }
  return 0; // 返回 0 表示成功

 err:
  uvmunmap(new, 0, i / PGSIZE, 1); // 解除子进程的映射
  return -1;
}

/**
  * void uvmclear(pagetable_t pagetable, uint64 va)
  * @brief: 标记页表项无效，禁止用户访问。
  * @brief: // mark a PTE invalid for user access.
            // used by exec for the user stack guard page.
  * @param: pagetable - 进程的页表
  * @param: va - 虚拟地址
  * @retval: 无返回值
  */
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte; // 页表项指针
  
  pte = walk(pagetable, va, 0); // 获取页表项
  if(pte == 0) // 检查页表项是否存在
    panic("uvmclear"); // 如果不存在，触发错误
  *pte &= ~PTE_U; // 将用户访问标志清除
}

/**
  * int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
  * @brief: 从内核复制数据到用户。
  * @brief: // Copy from kernel to user.
            // Copy len bytes from src to virtual address dstva in a given page table.
            // Return 0 on success, -1 on error.
  * @param: pagetable - 进程的页表
  * @param: dstva - 目标虚拟地址
  * @param: src - 源数据地址
  * @param: len - 要复制的字节数
  * @retval: 成功返回 0，失败返回 -1
  */
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0; // 用于遍历的字节数、虚拟地址和物理地址

  while(len > 0) { // 当还有剩余字节需要复制
    va0 = PGROUNDDOWN(dstva); // 获取对齐的虚拟地址
    pa0 = walkaddr(pagetable, va0); // 获取物理地址
    if(pa0 == 0) // 检查物理地址是否有效
      return -1; // 如果无效，返回 -1
    n = PGSIZE - (dstva - va0); // 计算当前页可以复制的字节数
    if(n > len) // 如果可复制字节数大于剩余字节数
      n = len; // 将可复制字节数调整为剩余字节数
    memmove((void *)(pa0 + (dstva - va0)), src, n); // 复制数据

    len -= n; // 更新剩余字节数
    src += n; // 更新源地址
    dstva = va0 + PGSIZE; // 更新目标虚拟地址
  }
  return 0;
}

/**
  * int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
  * @brief: 从用户复制数据到内核。
  * @brief: // Copy from user to kernel.
            // Copy len bytes to dst from virtual address srcva in a given page table.
            // Return 0 on success, -1 on error.
  * @param: pagetable - 进程的页表
  * @param: dst - 目标地址
  * @param: srcva - 源虚拟地址
  * @param: len - 要复制的字节数
  * @retval: 成功返回 0，失败返回 -1
  */
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0; // 用于遍历的字节数、虚拟地址和物理地址

  while(len > 0) { // 当还有剩余字节需要复制
    va0 = PGROUNDDOWN(srcva); // 获取对齐的虚拟地址
    pa0 = walkaddr(pagetable, va0); // 获取物理地址
    if(pa0 == 0) // 检查物理地址是否有效
      return -1; // 如果无效，返回 -1
    n = PGSIZE - (srcva - va0); // 计算当前页可以复制的字节数
    if(n > len) // 如果可复制字节数大于剩余字节数
      n = len; // 将可复制字节数调整为剩余字节数
    memmove(dst, (void *)(pa0 + (srcva - va0)), n); // 复制数据

    len -= n; // 更新剩余字节数
    dst += n; // 更新目标地址
    srcva = va0 + PGSIZE; // 更新源虚拟地址
  }
  return 0; // 返回 0 表示成功
}

/**
  * int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
  * @brief: 从用户复制一个以 null 结尾的字符串到内核。
  * @brief: // Copy a null-terminated string from user to kernel.
            // Copy bytes to dst from virtual address srcva in a given page table,
            // until a '\0', or max.
            // Return 0 on success, -1 on error.
  * @param: pagetable - 进程的页表
  * @param: dst - 目标地址
  * @param: srcva - 源虚拟地址
  * @param: max - 最多复制的字节数
  * @retval: 成功返回 0，失败返回 -1
  */
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

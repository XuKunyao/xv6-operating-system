// 物理内存布局

// qemu -machine virt 的内存布局如下，
// 基于 qemu 的 hw/riscv/virt.c 文件：
//
// 00001000 -- 启动 ROM，由 qemu 提供
// 02000000 -- CLINT（Core Local Interruptor，核心本地中断器）
// 0C000000 -- PLIC（Platform-Level Interrupt Controller，平台级中断控制器）
// 10000000 -- uart0（串口0的起始地址）
// 10001000 -- virtio 磁盘（virtio 设备的磁盘接口）
// 80000000 -- 启动 ROM 跳转到此处（机器模式下）
//              -kernel 会加载内核到此处
// 80000000 之后的未使用 RAM。

// 内核使用物理内存的方式如下：
// 80000000 -- entry.S，然后是内核的代码段和数据段
// end -- 内核页分配区域的起始地址
// PHYSTOP -- 内核使用的 RAM 结束位置

// qemu 将 UART 的寄存器映射到物理内存中的这个地址。
#define UART0 0x10000000L
#define UART0_IRQ 10 // UART 的中断号为 10

// virtio 的 MMIO（内存映射 I/O）接口地址
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// 本地中断控制器的基地址，包含定时器
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid)) // 定时比较器的地址，每个 hart（硬件线程）有一个
#define CLINT_MTIME (CLINT + 0xBFF8) // 从启动以来的时间周期数

// qemu 将可编程中断控制器（PLIC）映射到这个地址
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)  // 中断优先级寄存器的基地址
#define PLIC_PENDING (PLIC + 0x1000)  // 中断待处理寄存器的地址
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)  // 使能主模式中断，基于 hart
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)  // 使能从模式中断，基于 hart
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)  // 主模式的优先级寄存器，基于 hart
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)  // 从模式的优先级寄存器，基于 hart
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)  // 主模式的中断请求，基于 hart
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)  // 从模式的中断请求，基于 hart

// 内核期望 RAM 从物理地址 0x80000000 到 PHYSTOP 供内核和用户页面使用。
#define KERNBASE 0x80000000L  // 内核基地址
#define PHYSTOP (KERNBASE + 128*1024*1024)  // 内核使用的 RAM 结束地址（128MB 内存）

// 将 trampoline 页映射到最高地址，
// 该页在用户和内核空间中均可访问。
#define TRAMPOLINE (MAXVA - PGSIZE)  // trampoline 映射的位置

// 将内核栈映射到 trampoline 之下，
// 每个栈都被无效的保护页包围。
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)  // 每个进程的内核栈的位置

// 用户内存布局。
// 从地址 0 开始：
//   代码段
//   初始数据段和 BSS 段
//   固定大小的栈
//   可扩展的堆
//   ...
//   TRAPFRAME（p->trapframe，由 trampoline 使用）
//   TRAMPOLINE（与内核中的 trampoline 使用相同的页）
#define TRAPFRAME (TRAMPOLINE - PGSIZE)  // 用户 trapframe 的位置
#define USYSCALL (TRAPFRAME - PGSIZE)

struct usyscall{
    int pid; // Process ID
};
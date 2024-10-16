// 获取当前核（core）的编号
static inline uint64
r_mhartid()
{
  uint64 x;
  // 使用 RISC-V 的 "csrr" 指令从 mhartid 寄存器中读取核编号，并存储到 x 中
  asm volatile("csrr %0, mhartid" : "=r" (x) );
  return x;
}

// 机器状态寄存器，mstatus

#define MSTATUS_MPP_MASK (3L << 11) // 上一次执行的模式（previous mode）
#define MSTATUS_MPP_M (3L << 11) // 机器模式
#define MSTATUS_MPP_S (1L << 11) // 内核模式
#define MSTATUS_MPP_U (0L << 11) // 用户模式
#define MSTATUS_MIE (1L << 3)    // 机器模式下的中断使能

// 读取 mstatus 寄存器的值
static inline uint64
r_mstatus()
{
  uint64 x;
  // 使用 "csrr" 指令从 mstatus 寄存器中读取值，并存储到 x 中
  asm volatile("csrr %0, mstatus" : "=r" (x) );
  return x;
}

// 写入 mstatus 寄存器
static inline void 
w_mstatus(uint64 x)
{
  // 使用 "csrw" 指令将值 x 写入到 mstatus 寄存器中
  asm volatile("csrw mstatus, %0" : : "r" (x));
}

// 机器异常程序计数器 (mepc)，保存从异常返回时的指令地址
static inline void 
w_mepc(uint64 x)
{
  asm volatile("csrw mepc, %0" : : "r" (x));
}

// 内核态状态寄存器，sstatus

#define SSTATUS_SPP (1L << 8)  // 上一次执行的模式，1=内核态，0=用户模式
#define SSTATUS_SPIE (1L << 5) // 内核态下的先前中断使能
#define SSTATUS_UPIE (1L << 4) // 用户模式下的先前中断使能
#define SSTATUS_SIE (1L << 1)  // 内核中断使能
#define SSTATUS_UIE (1L << 0)  // 用户中断使能

// 读取 sstatus 寄存器的值
static inline uint64
r_sstatus()
{
  uint64 x;
  asm volatile("csrr %0, sstatus" : "=r" (x) );
  return x;
}

// 写入 sstatus 寄存器
static inline void 
w_sstatus(uint64 x)
{
  asm volatile("csrw sstatus, %0" : : "r" (x));
}

// 读取管理者中断挂起寄存器 (sip)
static inline uint64
r_sip()
{
  uint64 x;
  asm volatile("csrr %0, sip" : "=r" (x) );
  return x;
}

// 写入管理者中断挂起寄存器 (sip)
static inline void 
w_sip(uint64 x)
{
  asm volatile("csrw sip, %0" : : "r" (x));
}

// 内核中断使能
#define SIE_SEIE (1L << 9) // 外部中断
#define SIE_STIE (1L << 5) // 计时器中断
#define SIE_SSIE (1L << 1) // 软件中断

// 读取 sie 寄存器的值
static inline uint64
r_sie()
{
  uint64 x;
  asm volatile("csrr %0, sie" : "=r" (x) );
  return x;
}

// 写入 sie 寄存器
static inline void 
w_sie(uint64 x)
{
  asm volatile("csrw sie, %0" : : "r" (x));
}

// 机器模式下的中断使能
#define MIE_MEIE (1L << 11) // 外部中断
#define MIE_MTIE (1L << 7)  // 计时器中断
#define MIE_MSIE (1L << 3)  // 软件中断

// 读取 mie 寄存器的值
static inline uint64
r_mie()
{
  uint64 x;
  asm volatile("csrr %0, mie" : "=r" (x) );
  return x;
}

// 写入 mie 寄存器
static inline void 
w_mie(uint64 x)
{
  asm volatile("csrw mie, %0" : : "r" (x));
}

// 机器异常程序计数器 (sepc)，保存从异常返回时的指令地址
static inline void 
w_sepc(uint64 x)
{
  asm volatile("csrw sepc, %0" : : "r" (x));
}

// 读取 sepc 寄存器的值
static inline uint64
r_sepc()
{
  uint64 x;
  asm volatile("csrr %0, sepc" : "=r" (x) );
  return x;
}

// 读取机器异常委派寄存器 (medeleg)
static inline uint64
r_medeleg()
{
  uint64 x;
  asm volatile("csrr %0, medeleg" : "=r" (x) );
  return x;
}

// 写入机器异常委派寄存器 (medeleg)
static inline void 
w_medeleg(uint64 x)
{
  asm volatile("csrw medeleg, %0" : : "r" (x));
}

// 读取机器中断委派寄存器 (mideleg)
static inline uint64
r_mideleg()
{
  uint64 x;
  asm volatile("csrr %0, mideleg" : "=r" (x) );
  return x;
}

// 写入机器中断委派寄存器 (mideleg)
static inline void 
w_mideleg(uint64 x)
{
  asm volatile("csrw mideleg, %0" : : "r" (x));
}

// 写入内核态陷阱向量基地址寄存器，低两位是模式
static inline void 
w_stvec(uint64 x)
{
  asm volatile("csrw stvec, %0" : : "r" (x));
}

// 读取 stvec 寄存器的值
static inline uint64
r_stvec()
{
  uint64 x;
  asm volatile("csrr %0, stvec" : "=r" (x) );
  return x;
}

// 写入机器模式的中断向量寄存器 (mtvec)
static inline void 
w_mtvec(uint64 x)
{
  asm volatile("csrw mtvec, %0" : : "r" (x));
}

// RISC-V 的 Sv39 页表方案
#define SATP_SV39 (8L << 60)

// 生成 SATP 寄存器的值
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

// 写入 supervisor 地址转换和保护寄存器 (satp)，保存页表的地址
static inline void 
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r" (x));
}

// 读取 satp 寄存器的值
static inline uint64
r_satp()
{
  uint64 x;
  asm volatile("csrr %0, satp" : "=r" (x) );
  return x;
}

// 内核模式的Scratch寄存器，用于trampoline.S中的早期trap处理程序
static inline void 
w_sscratch(uint64 x)
{
  asm volatile("csrw sscratch, %0" : : "r" (x));
}

// 写机器模式的Scratch寄存器
static inline void 
w_mscratch(uint64 x)
{
  asm volatile("csrw mscratch, %0" : : "r" (x));
}

// 内核模式陷阱原因寄存器，存储导致陷阱的原因
static inline uint64
r_scause()
{
  uint64 x;
  asm volatile("csrr %0, scause" : "=r" (x) );
  return x;
}

// 内核模式陷阱值寄存器，保存与陷阱有关的值（例如地址或数据）
static inline uint64
r_stval()
{
  uint64 x;
  asm volatile("csrr %0, stval" : "=r" (x) );
  return x;
}

// 机器模式的计数器启用寄存器
static inline void 
w_mcounteren(uint64 x)
{
  asm volatile("csrw mcounteren, %0" : : "r" (x));
}

static inline uint64
r_mcounteren()
{
  uint64 x;
  asm volatile("csrr %0, mcounteren" : "=r" (x) );
  return x;
}

// 机器模式的时间周期计数器
static inline uint64
r_time()
{
  uint64 x;
  asm volatile("csrr %0, time" : "=r" (x) );
  return x;
}

// 启用设备中断
static inline void
intr_on()
{
  // 打开SSTATUS_SIE标志，启用内核模式的中断
  w_sstatus(r_sstatus() | SSTATUS_SIE);
}

// 禁用设备中断
static inline void
intr_off()
{
  // 清除SSTATUS_SIE标志，禁用内核模式的中断
  w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

// 判断设备中断是否启用
static inline int
intr_get()
{
  uint64 x = r_sstatus();
  // 检查SSTATUS_SIE标志是否启用，返回1表示启用，0表示禁用
  return (x & SSTATUS_SIE) != 0;
}

// 读取堆栈指针
static inline uint64
r_sp()
{
  uint64 x;
  // 将当前堆栈指针（sp）的值存入x并返回
  asm volatile("mv %0, sp" : "=r" (x) );
  return x;
}

// 读取线程指针（tp），保存当前的hartid（核ID）
// 保存cpus[]中的索引
static inline uint64
r_tp()
{
  uint64 x;
  // 将线程指针tp的值存入x并返回
  asm volatile("mv %0, tp" : "=r" (x) );
  return x;
}

// 写入线程指针（tp）
static inline void 
w_tp(uint64 x)
{
  // 将x的值写入线程指针tp
  asm volatile("mv tp, %0" : : "r" (x));
}

// 读取返回地址寄存器（ra）
static inline uint64
r_ra()
{
  uint64 x;
  asm volatile("mv %0, ra" : "=r" (x) );
  return x;
}

// 读取栈顶指针寄存器(fp)
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}

// 刷新TLB (Translation Lookaside Buffer)
static inline void
sfence_vma()
{
  // sfence.vma zero, zero 表示刷新所有的TLB条目
  asm volatile("sfence.vma zero, zero");
}


#define PGSIZE 4096 // 每页大小为4096字节
#define PGSHIFT 12  // 页面内偏移的位数

#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1)) // 将sz四舍五入到页面边界
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE-1))           // 将a下舍入到页面边界

// 页表项标志位
#define PTE_V (1L << 0) // 有效位
#define PTE_R (1L << 1) // 读权限
#define PTE_W (1L << 2) // 写权限
#define PTE_X (1L << 3) // 执行权限
#define PTE_U (1L << 4) // 用户模式可访问

// 将物理地址右移12位后，移到PTE合适的位置
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)

// 从PTE中提取物理地址
#define PTE2PA(pte) (((pte) >> 10) << 12)

// 从PTE中提取标志位
#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// 从虚拟地址中提取三个9位的页表索引
#define PXMASK          0x1FF // 9位掩码
#define PXSHIFT(level)  (PGSHIFT+(9*(level))) // 计算每级页表的偏移量
#define PX(level, va) ((((uint64) (va)) >> PXSHIFT(level)) & PXMASK)

// 超过最高虚拟地址的一个值
// MAXVA 实际上比Sv39允许的最大值少一位，以避免处理带符号扩展的虚拟地址
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))

// 页表项类型定义
typedef uint64 pte_t;           // 页表项
typedef uint64 *pagetable_t;    // 页表，包含512个页表项

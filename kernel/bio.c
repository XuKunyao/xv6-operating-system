// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  // struct spinlock lock;
  struct spinlock eviction_lock;
  struct buf buf[NBUF];
  // struct buf head;
  struct buf bufmap[NBUFMAP_BUCKET]; // 定义哈希表
  struct spinlock bufmap_locks[NBUFMAP_BUCKET]; // 给每个散列桶单独设置锁
} bcache;

void
binit(void)
{
  // 初始化 bufmap 哈希桶的每个锁，并设置每个桶的链表头
  for(int i=0;i<NBUFMAP_BUCKET;i++) { 
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap"); // 初始化每个桶的锁
    bcache.bufmap[i].next = 0;  // 将每个 bufmap[i] 的 next 指针初始化为 0，表示桶当前为空
  }

  // 初始化缓存缓冲区（buffers）
  for(int i=0;i<NBUF;i++){
    struct buf *b = &bcache.buf[i]; // 获取当前缓冲块的指针
    initsleeplock(&b->lock, "buffer"); // 初始化每个缓存块的睡眠锁
    b->lastuse = 0; // 将最少使用(LRU)缓存buf初始化为 0
    b->refcnt = 0; // 引用计数初始化为 0
    // 将当前缓冲块加入到 bufmap[0] 桶中
    // 初始状态下，所有缓冲块都放在 bufmap[0] 中
    b->next = bcache.bufmap[0].next; // 将当前缓冲块的 next 指针指向 bufmap[0] 的第一个块
    bcache.bufmap[0].next = b; // 将 bufmap[0] 的 next 指针指向当前缓冲块
  }

  // 初始化全局驱逐锁（eviction_lock），用于缓存驱逐过程的同步控制
  // 该锁在驱逐过程中确保只有一个线程能执行驱逐逻辑，防止并发冲突
  initlock(&bcache.eviction_lock, "bcache_eviction");
}

/**
  * static struct buf* bget()
  * @brief: 在设备 dev 上查找指定的 blockno 缓存块，如果找不到该缓存块，则分配一个新的缓冲区。
  * @param dev: 设备号，表示目标设备。
  * @param blockno: block 的编号，用于标识该设备上的具体 block。
  * @retval: 无论哪种情况都返回一个指向包含该 block 的已锁定缓冲区指针（struct buf*）。
  */
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  // 1.生成哈希键
  uint key = BUFMAP_HASH(dev, blockno);  // 使用哈希函数计算桶的位置（key）
  // 2.获取目标桶的锁
  acquire(&bcache.bufmap_locks[key]);  // 获取目标桶的锁
  // 3.查找块缓存
  for(b = bcache.bufmap[key].next; b; b = b->next){ // 判断该 block 是否已经被缓存
    if(b->dev == dev && b->blockno == blockno){  // 如果找到匹配的缓存块
      b->refcnt++;  // 增加引用计数
      release(&bcache.bufmap_locks[key]);  // 释放桶锁
      acquiresleep(&b->lock);  // 获取该缓存块的睡眠锁，确保独占访问
      return b;
    }
  }

  // 若未缓存，则继续处理
  // 获取合适的缓存块进行复用需要遍历所有桶，这意味着我们需要获取所有桶的锁。
  // 然而，持有一个桶的锁的同时尝试获取另一个桶的锁是不安全的，
  // 因为它可能会导致循环等待，产生死锁。
  // 5.获取驱逐锁
  release(&bcache.bufmap_locks[key]);
  // 我们释放当前桶的锁，这样遍历所有桶时不会导致循环等待和死锁。
  // 但是这样做的副作用是，其他 CPU 可能会在同一时刻请求相同的 blockno，
  // 并且可能会在多个 CPU 上多次创建相同的缓存块。
  // 因此，在获取驱逐锁（eviction_lock）后，我们再次检查缓存是否已存在，
  // 确保不会创建重复的缓存块。

  acquire(&bcache.eviction_lock);  // 获取全局驱逐锁，确保该过程的独占性

  // 6.再次查找块缓存
  // 再次检查该 block 是否已经被缓存
  // 此时持有驱逐锁，不会发生并发的驱逐和复用过程，
  // 因此可以在没有桶锁的情况下安全地遍历 `bcache.bufmap[key]`。
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){  // 如果找到匹配的缓存块
      acquire(&bcache.bufmap_locks[key]);  // 获取桶锁以安全地更新引用计数
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);  // 释放桶锁
      release(&bcache.eviction_lock);  // 释放驱逐锁
      acquiresleep(&b->lock);  // 获取该缓存块的睡眠锁
      return b;
    }
  }

  // 7.寻找可替换的缓存块
  // 若依然未缓存，此时我们仅持有驱逐锁，没有任何桶锁，
  // 所以可以安全地获取任何桶锁而不产生循环等待和死锁。
  // 遍历所有桶，找到最少最近使用的缓存块
  // 最终将该缓存块的对应桶锁保持锁定状态。
  struct buf *before_least = 0; 
  uint holding_bucket = -1;
  for(int i = 0; i < NBUFMAP_BUCKET; i++){
    // 在获取每个桶锁前，我们要么没有持有任何锁，要么只持有左侧桶的锁
    // 这样不会产生循环等待，因此不会产生死锁。
    acquire(&bcache.bufmap_locks[i]);
    int newfound = 0;  // 用于记录是否在当前桶找到新的最近最少使用的缓存块
    for(b = &bcache.bufmap[i]; b->next; b = b->next) {
      if(b->next->refcnt == 0 && (!before_least || b->next->lastuse < before_least->next->lastuse)) {
        before_least = b;  // 更新为找到的最近最少使用的缓存块
        newfound = 1;  // 标记找到新的最近最少使用的缓存块
      }
    }
    if(!newfound) {
      release(&bcache.bufmap_locks[i]);  // 如果该桶没有合适的缓存块，释放该桶锁
    } else {
      if(holding_bucket != -1) release(&bcache.bufmap_locks[holding_bucket]);  // 释放原先持有的桶锁
      holding_bucket = i;
      // 保持当前桶的锁
    }
  }
  if(!before_least) {
    panic("bget: no buffers");  // 如果没有找到合适的缓存块，报告错误
  }
  b = before_least->next;  // 获取最少最近使用的缓存块
  
  // 8.驱逐并插入新的块
  if(holding_bucket != key) {
    // 从原桶中删除该缓存块
    before_least->next = b->next;
    release(&bcache.bufmap_locks[holding_bucket]);  // 释放原桶的锁
    // 重新哈希并将缓存块添加到目标桶中
    acquire(&bcache.bufmap_locks[key]);
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }
  
  // 设置缓存块的属性
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;  // 设置引用计数为 1
  b->valid = 0;  // 初始化缓存块为无效
  release(&bcache.bufmap_locks[key]);  // 释放目标桶锁
  release(&bcache.eviction_lock);  // 释放驱逐锁
  acquiresleep(&b->lock);  // 获取缓存块的睡眠锁
  return b;  // 返回锁定的缓存块
}


/**
  * struct buf* bread()
  * @brief: 从设备的指定 block 中读取数据，返回一个包含该 block 内容的已锁定缓冲区
  * @param dev: 设备号，表示从哪个设备读取数据
  * @param blockno: 目标 block 的编号
  * @retval: 返回一个指向包含该 block 内容的缓冲区指针（struct buf*）
  */
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b; // 定义缓冲区指针 b

  b = bget(dev, blockno); // 尝试获取包含目标 block 的缓冲区（若缓冲区未存在则创建一个）
  if(!b->valid) {  // 如果缓冲区内容无效（未加载该 block 内容）
    virtio_disk_rw(b, 0); // 从设备读取该 block 内容到缓冲区
    b->valid = 1; // 将缓冲区标记为有效
  }
  return b; // 返回包含指定 block 内容的缓冲区
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

/**
  * @brief: 释放一个已锁定的缓冲区，将其移动到最近使用的链表头部
  * @param: b - 需要释放的缓冲区
  * @retval: 无返回值
  */ 
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock)) // 检查当前进程是否持有该缓冲区的睡眠锁，如果没有持有锁则触发错误
    panic("brelse");

  releasesleep(&b->lock); // 释放缓冲区的睡眠锁，以便其他进程可以访问该缓冲区

  uint key = BUFMAP_HASH(b->dev, b->blockno);// 使用哈希函数计算该缓存块在缓存桶数组中的位置key以便对其对应的桶锁进行操作

  acquire(&bcache.bufmap_locks[key]); // 获取对应桶的锁以保证对引用计数的修改是线程安全的
  b->refcnt--; // 减少缓存块的引用计数
  if (b->refcnt == 0) { // 如果引用计数为 0，说明该缓存块当前没有被任何进程使用
    b->lastuse = ticks; // ticks 是一个全局时钟计数器，这里是用于LRU策略
  }
  release(&bcache.bufmap_locks[key]);  // 释放桶锁，完成操作
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}



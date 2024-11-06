// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit()
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

/**
  * struct inode* ialloc()
  * @brief: 在指定设备上分配一个新的 inode，并将其类型设定为指定的 type。
  * @param dev: 设备号，表示在哪个设备上分配 inode
  * @param type: inode 的类型 (例如，T_FILE 表示文件，T_DIR 表示目录)
  * @retval: 返回一个已分配但未加锁、已引用的 inode 指针；如果没有空闲 inode 可用，则会终止系统
  */
struct inode*
ialloc(uint dev, short type)
{
  int inum; // 定义变量 inum，用于表示 inode 编号
  struct buf *bp; // 定义一个指向缓冲区的指针
  struct dinode *dip; // 定义一个指向磁盘 inode 结构的指针

  for(inum = 1; inum < sb.ninodes; inum++){ // 循环遍历所有 inode（从 1 开始，避免使用 0 号 inode）
    bp = bread(dev, IBLOCK(inum, sb)); // 读取包含该 inode 的磁盘 block
    dip = (struct dinode*)bp->data + inum%IPB; // 定位到 inum 对应的 dinode 在 block 中的具体位置
    if(dip->type == 0){  // 检查该 inode 是否为空闲状态（类型为 0 表示空闲）
      memset(dip, 0, sizeof(*dip)); // 清零该 dinode 的内容，初始化 inode
      dip->type = type; // 设置 inode 的类型为指定的 type 值，标记为已分配
      log_write(bp); // 将已分配的 inode 信息写入日志，以便写回磁盘    
      brelse(bp); // 释放缓冲区
      return iget(dev, inum); // 获取并返回新分配的 inode（未加锁但已分配并被引用）
    }
    brelse(bp); // 若当前 inode 已被占用，释放缓冲区继续下一个 inode
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&icache.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&icache.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&icache.lock);
  }

  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode 内容
//
// 与每个 inode 相关联的内容（数据）存储在磁盘上的块中
// 前 NDIRECT 个块号直接存储在 ip->addrs[] 中
// 下一个 NINDIRECT 块号存储在块 ip->addrs[NDIRECT] 中（即间接块）

/**
  * static uint bmap()
  * @brief 获取 inode 中第 bn 个块的磁盘块地址，如果没有该块则分配一个
  * @param ip 指向 inode 的指针
  * @param bn 要访问的块编号（从 0 开始）
  * @retval 第 bn 个块在磁盘上的块地址
  */
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){ // 若 bn 小于 NDIRECT，则 bn 对应直接块
    if((addr = ip->addrs[bn]) == 0) // 如果地址为 0，说明该块未分配
      ip->addrs[bn] = addr = balloc(ip->dev); // 分配一个新块，并存储其地址
    return addr; // 返回该直接块的地址
  }
  bn -= NDIRECT; // 若块编号超出直接块范围，则调整索引为间接块的编号

  // 单重间接块处理
  if(bn < NINDIRECT){ // 如果 bn 小于 NINDIRECT，则 bn 对应间接块
     // 加载间接块地址，若尚未分配则分配一个新的
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev); // 分配一个间接块的存储地址
    bp = bread(ip->dev, addr); // 读取间接块
    a = (uint*)bp->data; // 将间接块数据视为地址数组
    if((addr = a[bn]) == 0){ // 若间接块中该位置为 0，表示尚未分配
      a[bn] = addr = balloc(ip->dev); // 为该位置分配新的磁盘块
      log_write(bp); // 将间接块的修改写入日志
    }
    brelse(bp); // 释放缓冲区
    return addr; // 返回该间接块中的目标块地址
  }

  // 双重间接块处理
  bn -= NINDIRECT; // 转换为双重间接块的编号，前面bn已经减过一次 NDIRECT 了

  if(bn < NINDIRECT * NINDIRECT){ // 如果 bn 小于 NINDIRECT * NINDIRECT，则 bn 对应双重间接块
    if((addr = ip->addrs[NDIRECT+1]) == 0) // 检查双重间接块的一级间接块地址是否已分配
      ip->addrs[NDIRECT+1] = addr = balloc(ip->dev); // 若未分配，则为双重间接块的一级间接块分配新地址
    bp = bread(ip->dev, addr);  // 读取双重间接块的一级间接块内容
    a = (uint*)bp->data; // 将缓冲区内容作为一级地址数组
    if((addr = a[bn/NINDIRECT]) == 0){ // 计算一级间接块索引，若地址为空则分配新块
      a[bn/NINDIRECT] = addr = balloc(ip->dev); // 为一级间接块分配新的地址
      log_write(bp); // 将一级间接块的修改写入日志
    }
    brelse(bp); // 释放一级间接块的缓冲区

    bn %= NINDIRECT; // 获取双重间接块中二级间接块的索引
    bp = bread(ip->dev, addr); // 读取二级间接块
    a = (uint*)bp->data; // 将缓冲区内容作为二级地址数组
    if((addr = a[bn]) == 0){ // 如果二级间接块中的该地址为空
      a[bn] = addr = balloc(ip->dev); // 分配新块，并将地址写入二级间接块
      log_write(bp); // 将二级间接块的修改写入日志
    }
    brelse(bp); // 释放缓冲区
    return addr; // 返回该间接块中的目标块地址
  }

  panic("bmap: out of range");// 若块编号超出范围，报错
}


/**
  * void itrunc(struct inode *ip)
  * @brief: 释放 inode 对应的数据块，从而清空文件内容
  * @brief: 截断 inode（丢弃其内容）,调用者必须持有 ip->lock
  * @param: ip 需要被截断内容的 inode 指针
  * @retval: 无返回值
  */
void
itrunc(struct inode *ip)
{
  int i, j; // i 和 j 是循环变量，用于遍历直接块和间接块中的数据块
  struct buf *bp; // 缓冲区指针，用于加载间接块
  uint *a; // 指向间接块数据的指针，将间接块数据视为地址数组

  // 处理直接块（NDIRECT 个直接块）
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){ // 检查直接块地址是否有效（非 0）
      bfree(ip->dev, ip->addrs[i]); // 释放该块，将块标记为空闲
      ip->addrs[i] = 0; // 将 inode 的地址设为 0，表示未分配
    }
  }

  // 处理单重间接块
  if(ip->addrs[NDIRECT]){ // 检查间接块地址是否有效
    bp = bread(ip->dev, ip->addrs[NDIRECT]); // 从磁盘中读取单重间接块的内容
    a = (uint*)bp->data; // 将缓冲区数据视为地址数组
    for(j = 0; j < NINDIRECT; j++){ // 遍历间接块中的每一个数据块地址
      if(a[j]) // 如果间接块中的数据块地址非 0，表示已分配
        bfree(ip->dev, a[j]); // 释放该数据块
    }
    brelse(bp); // 释放缓冲区，解除锁定
    bfree(ip->dev, ip->addrs[NDIRECT]); // 释放单重间接块本身
    ip->addrs[NDIRECT] = 0; // 将 inode 的间接块地址设为 0，表示未分配
  }
  
  // 处理双重间接块
  if(ip->addrs[NDIRECT+1]){ // 检查是否存在双重间接块
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]); // 从磁盘中读取双重间接块的内容
    a = (uint*)bp->data; // 将缓冲区数据视为地址数组
    for(j = 0; j < NINDIRECT; j++){ // 遍历双重间接块中的每一个单重间接块
      if(a[j]) { // 若地址非 0，表示该单重间接块已分配
        struct buf *bp2 = bread(ip->dev, a[j]); // 读取该单重间接块的内容
        uint *a2 = (uint*)bp2->data; // 将其数据视为地址数组
        for(int k = 0; k < NINDIRECT; k++){ // 遍历单重间接块中的每一个数据块地址
          if(a2[k]) // 若地址非 0，表示块已分配
            bfree(ip->dev, a2[k]); // 释放该数据块
        }
        brelse(bp2); // 释放内层缓冲区
        bfree(ip->dev, a[j]); // 释放单重间接块本身
      }
    }
    brelse(bp); // 释放双重间接块缓冲区
    bfree(ip->dev, ip->addrs[NDIRECT+1]); // 释放双重间接块本身
    ip->addrs[NDIRECT+1] = 0; // 将 inode 的双重间接块地址设为 0
  }

  ip->size = 0; // 将文件大小设为 0
  iupdate(ip); // 将 inode 的更改（即块释放及大小变化）写入磁盘中
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

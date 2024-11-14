//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

/**
  * uint64 sys_mmap(void)
  * @brief: 实现 mmap 系统调用，将文件映射到进程的虚拟地址空间中。
  * @param: 无直接参数，参数从系统调用参数中获取。
  *         参数0: addr（用户提供的虚拟地址，当前实现不使用此地址）
  *         参数1: sz（映射的大小，以字节为单位）
  *         参数2: prot（保护标志，例如读、写、执行权限）
  *         参数3: flags（映射的属性，例如 MAP_SHARED 或 MAP_PRIVATE）
  *         参数4: fd（文件描述符）
  *         参数5: offset（文件偏移量）
  * @retval: 成功返回映射的虚拟地址；失败返回 -1。
  */
uint64
sys_mmap(void)
{
  uint64 addr, sz, offset;
  int prot, flags, fd; struct file *f;

  // 从系统调用参数中获取 addr, sz, prot, flags, fd, offset
  if(argaddr(0, &addr) < 0 || argaddr(1, &sz) < 0 || argint(2, &prot) < 0
    || argint(3, &flags) < 0 || argfd(4, &fd, &f) < 0 || argaddr(5, &offset) < 0 || sz == 0)
    return -1; // 如果参数无效或 sz 为 0，返回 -1 表示失败
  
  // 检查文件的读写权限与映射权限是否兼容
  if((!f->readable && (prot & (PROT_READ))) // 文件不可读但请求了读权限
     || (!f->writable && (prot & PROT_WRITE) && !(flags & MAP_PRIVATE))) // 文件不可写且请求了写权限
    return -1;
  
  sz = PGROUNDUP(sz); // 将映射大小向上取整到页边界

  struct proc *p = myproc();
  struct vma *v = 0; // 用于存储找到的空闲 VMA
  uint64 vaend = MMAPEND; // 结束地址，非包含性
  
  // mmaptest从未传递非零的 addr 参数，因此这里忽略 addr，
  // 并找到一个新的未映射的虚拟地址区域来映射文件。
  // 我们的实现将文件映射在trapframe下方，从高地址向低地址映射。

  // 在进程的 vmas 中找到一个空闲的 VMA，并在查找过程中计算映射文件的位置
  for(int i=0;i<NVMA;i++) {
    struct vma *vv = &p->vmas[i];
    if(vv->valid == 0) { // 如果找到无效（未使用）的 vma
      if(v == 0) {       // 这一个判断的目的是只将v设置为第一个可用的 vma
        v = &p->vmas[i]; // 记录找到的空闲 vma
        v->valid = 1; // 将此 vma 标记为有效
      }
    } else if(vv->vastart < vaend) { // 找到有效的 vma，调整映射终止地址
      vaend = PGROUNDDOWN(vv->vastart); // 使终止地址对齐到页边界
    }
  }

  if(v == 0){ // 如果没有找到空闲的 vma，触发 panic 错误
    panic("mmap: no free vma");
  }
  
  v->vastart = vaend - sz; // 将映射的起始地址设置为 vaend 向下偏移 sz
  v->sz = sz;              // 映射的大小
  v->prot = prot;          // 映射的保护标志
  v->flags = flags;        // 映射的标志（共享或私有）
  v->f = f;                // assume f->type == FD_INODE
  v->offset = offset;      // 设置文件偏移量

  filedup(v->f); // 增加文件的引用计数

  return v->vastart; // 返回映射的虚拟地址起始地址
}

/**
  * struct vma *findvma()
  * @brief： 查找并返回与给定虚拟地址对应的vma结构
  * @param： p 进程指针，va 虚拟地址
  * @retval： 返回匹配的vma结构指针，如果没有找到，返回0
  */
struct vma *findvma(struct proc *p, uint64 va) {
  for(int i=0; i<NVMA; i++) {  // 遍历进程的所有vma条目
    struct vma *vv = &p->vmas[i];  // 获取当前vma结构
    if(vv->valid == 1 && va >= vv->vastart && va < vv->vastart + vv->sz) {  
      // 如果该vma是有效的，并且虚拟地址va位于该vma的映射范围内
      return vv;  // 返回匹配的vma
    }
  }
  return 0;  // 如果没有找到匹配的vma，返回0
}

/**
  * int vmatrylazytouch()
  * @brief： 检查某个vma是否为惰性分配，并且在使用前需要修改
  * @brief： 如果需要修改，执行修改操作，将虚拟地址映射到物理页面，并读取映射文件的数据
  * @param： va 虚拟地址
  * @retval： 如果页面需要修改并已成功加载，返回1；如果不需要或发生错误，返回0
  */
int vmatrylazytouch(uint64 va) {
  struct proc *p = myproc();  // 获取当前进程的指针
  struct vma *v = findvma(p, va);  // 查找包含虚拟地址va的vma
  if(v == 0) {  // 如果没有找到相应的vma
    return 0;  // 返回0，表示没有必要进行修改操作
  }

  // printf("vma映射: %p => %d\n", va, v->offset + PGROUNDDOWN(va - v->vastart));

  // 分配物理页面
  void *pa = kalloc();  // 调用kalloc分配一页物理内存
  if(pa == 0) {  // 如果分配失败
    panic("vmalazytouch: kalloc");  // 调用panic终止程序
  }
  memset(pa, 0, PGSIZE);  // 将分配的物理页面清零

  // 从磁盘读取数据
  begin_op();  // 开始一个磁盘操作
  ilock(v->f->ip);  // 锁定文件的inode（防止其他进程修改文件）
  // 从被映射文件中读取数据到物理页面中
  readi(v->f->ip, 0, (uint64)pa, v->offset + PGROUNDDOWN(va - v->vastart), PGSIZE); 
  iunlock(v->f->ip);  // 解锁文件的inode
  end_op();  // 完成磁盘操作

  // 设置适当的权限，并将页面映射到虚拟地址
  int perm = PTE_U;  // 默认设置为用户权限
  if(v->prot & PROT_READ)  // 如果vma的保护属性包括读取
    perm |= PTE_R;  // 设置为可读
  if(v->prot & PROT_WRITE)  // 如果vma的保护属性包括写入
    perm |= PTE_W;  // 设置为可写
  if(v->prot & PROT_EXEC)  // 如果vma的保护属性包括执行
    perm |= PTE_X;  // 设置为可执行

  // 将物理页面映射到虚拟地址va
  if(mappages(p->pagetable, va, PGSIZE, (uint64)pa, PTE_R | PTE_W | PTE_U) < 0) {  
    panic("vmalazytouch: mappages");  // 如果映射失败，调用panic终止程序
  }

  return 1;  // 成功修改页面并映射，返回1
}

/**
  * uint64 sys_munmap()
  * @brief： 解除内存映射，释放指定的虚拟地址空间
  * @param： 无
  * @retval： 如果成功，返回0；如果失败，返回-1
  */
uint64
sys_munmap(void)
{
  uint64 addr, sz; // 定义虚拟地址和大小变量

  if(argaddr(0, &addr) < 0 || argaddr(1, &sz) < 0 || sz == 0) // 获取参数：虚拟地址 addr 和大小 sz
    return -1; // 如果获取失败或大小为 0，则返回 -1

  struct proc *p = myproc(); // 获取当前进程的指针

  struct vma *v = findvma(p, addr); // 查找与给定地址 addr 匹配的 vma（虚拟内存区域）
  if(v == 0) { // 如果找不到对应的 vma，表示该地址不在当前进程的有效虚拟内存区域内，返回 -1
    return -1;
  }

  if(addr > v->vastart && addr + sz < v->vastart + v->sz) {
    // 如果尝试在内存范围内“挖坑”，即试图解除映射内存的一部分区域（不连续），返回 -1
    return -1;
  }

  uint64 addr_aligned = addr;
  if(addr > v->vastart) { // 如果地址不对齐（即不是页对齐），则将地址对齐到页边界
    addr_aligned = PGROUNDUP(addr); // 将地址向上对齐到下一个页边界
  }

  // 计算实际需要解除映射的字节数
  int nunmap = sz - (addr_aligned-addr); // 实际解除映射的字节数
  if(nunmap < 0)
    nunmap = 0; // 防止字节数小于 0
  
  vmaunmap(p->pagetable, addr_aligned, nunmap, v); // 为映射的文件执行解除映射操作

  // 如果解除映射的地址范围涉及映射区域的前半部分
  if(addr <= v->vastart && addr + sz > v->vastart) { // 如果要解除的区域在映射区域的前半部分
    v->offset += addr + sz - v->vastart; // 更新文件偏移量
    v->vastart = addr + sz; // 更新虚拟内存区域的起始地址
  }
  v->sz -= sz; // 更新虚拟内存区域的大小

  if(v->sz <= 0) { // 如果该 vma 的大小为 0 或小于等于 0，表示该 vma 已完全解除映射
    fileclose(v->f); // 关闭文件
    v->valid = 0; // 将 vma 标记为无效
  }

  return 0; // 返回 0，表示解除映射成功
}
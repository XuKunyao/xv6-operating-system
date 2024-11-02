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

/**
  * static struct inode* create()
  * @brief: 创建指定路径的文件或目录，若路径存在同名文件且类型匹配则直接返回inode。
  * @param path: 文件或目录的路径名
  * @param type: 创建项的类型 (T_FILE表示文件，T_DIR表示目录)
  * @param major: 若创建设备文件，表示设备的主编号
  * @param minor: 若创建设备文件，表示设备的次编号
  * @retval: 返回指向新创建或已存在的inode的指针；若创建失败则返回0
  */
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ]; // 存储路径中最后的文件或目录名

  if((dp = nameiparent(path, name)) == 0) // 获取路径中的父目录的inode，并将文件/目录名存入name中
    return 0;

  ilock(dp); // 锁定父目录的inode以进行安全操作

  if((ip = dirlookup(dp, name, 0)) != 0){ // 检查父目录中是否已经存在相同名称的文件或目录
    iunlockput(dp); // 若已存在，解锁并释放父目录的inode
    ilock(ip); // 锁定已有的inode以进行下一步操作
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE)) // 若类型是文件且已有的inode类型也为文件或设备，则可以直接使用已有的inode
      return ip; // 返回已存在的inode
    iunlockput(ip); // 若类型不匹配，解锁并释放inode
    return 0; // 返回失败
  }

  if((ip = ialloc(dp->dev, type)) == 0) // 若同名文件不存在，分配一个新的inode
    panic("create: ialloc"); // 若分配失败则终止

  ilock(ip); // 锁定新分配的inode并初始化相关字段
  ip->major = major; // 设置主设备号
  ip->minor = minor; // 设置次设备号
  ip->nlink = 1; // 设置链接计数
  iupdate(ip);  // 将inode更新写入磁盘

  if(type == T_DIR){  // 若创建的是目录，则初始化.和..目录项
    dp->nlink++; // 父目录链接数加1，用于引用..
    iupdate(dp);  // 更新父目录信息到磁盘
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0) // 将新文件/目录链接到父目录
    panic("create: dirlink");

  iunlockput(dp); // 解锁并释放父目录的inode

  return ip; // 返回新创建的inode指针
}

uint64
sys_open(void)
{
  char path[MAXPATH]; // 定义路径字符数组
  int fd, omode; // 定义文件描述符(fd)变量，打开模式(omode)变量
  struct file *f; // 定义指向文件(file)
  struct inode *ip; // 定义inode的指针
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0) // 检查传入参数：读取路径名字符串到path，读取打开模式到omode
    return -1; // 若参数不正确，则返回错误

  begin_op(); // 开启文件系统操作（设置事务）

  if(omode & O_CREATE){ // 检查是否需要创建文件
    ip = create(path, T_FILE, 0, 0); // 若需要创建文件，调用create函数创建新文件的inode并返回inode指针
    if(ip == 0){ // 若创建失败
      end_op();  // 结束文件系统操作
      return -1; // 返回错误
    }
  } else { // 如果不需要创建文件
    if((ip = namei(path)) == 0){ // 使用namei函数查找路径对应的inode
      end_op(); // 若路径不存在，结束文件系统操作
      return -1; // 返回错误
    }
    ilock(ip); // 锁定inode
    if(ip->type == T_DIR && omode != O_RDONLY){ // 如果inode是目录且打开模式不是只读，则返回错误（目录不可写）
      iunlockput(ip); // 释放inode锁并减少引用计数
      end_op(); // 结束文件系统操作
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){ // 若inode为设备类型，但major编号不合法，则返回错误
    iunlockput(ip); // 释放inode锁并减少引用计数
    end_op(); // 结束文件系统操作
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){ // 为文件分配file结构并为它分配文件描述符
    if(f) // 如果file分配成功但fd分配失败
      fileclose(f); // 关闭文件并释放资源
    iunlockput(ip); // 释放inode锁并减少引用计数
    end_op(); // 结束文件系统操作
    return -1;
  }

  if(ip->type == T_DEVICE){ // 如果inode类型为设备，设置文件类型为FD_DEVICE，并记录设备主编号
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else { // 否则将文件类型设置为FD_INODE，偏移量初始化为0
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip; // 设置file结构中的inode和读写权限
  f->readable = !(omode & O_WRONLY); // 若omode不是只写，则可读
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR); // 若omode是只写或可读可写，则可写

  if((omode & O_TRUNC) && ip->type == T_FILE){ // 如果模式包含O_TRUNC且inode类型为文件，截断文件内容
    itrunc(ip);
  }

  iunlock(ip); // 释放inode锁
  end_op(); // 结束文件系统操作

  return fd; // 返回分配的文件描述符
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

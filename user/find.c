#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char*
fmtname(char *path)
{
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;
    return p;
}

void
find(char *path, char* str)
{
  char buf[512], *p;
  int fd;
  struct dirent de; // 目录条目结构，用于读取目录内容
  struct stat st; // 文件状态结构，用于获取文件信息

// 尝试以只读方式打开 path，若失败则输出错误信息并返回
  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

// 获取文件状态，若失败则输出错误信息并关闭文件描述符
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_DEVICE://判定为设备文件
  case T_FILE:
    // 如果是普通文件，直接输出文件路径
    if(!strcmp(str, fmtname(path))) //当str和fmtname(path)相同时strcmp()返回0
        printf("%s\n", path);
    break;

  case T_DIR:
    // 如果是目录，检查路径长度是否过长，避免缓冲区溢出
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      break;
    }
    // 将路径拷贝到缓冲区，并在其后加上'/'准备读取目录内容
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){ // 逐个读取目录中的条目
      if(de.inum == 0) // 如果条目无效（inum为0），则跳过
        continue;
      memmove(p, de.name, DIRSIZ); // 将目录项的名称复制到 buf 中。
      p[DIRSIZ] = 0;  // 在 buf 的末尾添加空字符 \0，形成有效的路径。
      if(stat(buf, &st) < 0){
        printf("find: cannot stat %s\n", buf);
        continue;
      }
      char* lstname = fmtname(buf);
      // 跳过当前目录 (".") 和父目录 ("..") 的检查。
      // 这是因为在 UNIX 和 Linux 文件系统中，每个目录都会包含这两个特殊目录项
      if(strcmp(".", lstname) == 0 || strcmp("..", lstname) == 0){ 
        continue;
      }
      else{
        find(buf, str);
      }
    }
    break;
  }
  close(fd);
}

int
main(int argc, char* argv[])
{
  if(argc != 3){
    printf("Parameters are not enough\n");
    exit(1);
  }
  else{
    find(argv[1], argv[2]);
  }
  exit(0);
}



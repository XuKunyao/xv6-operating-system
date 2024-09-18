#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/fs.h"

#define MSGSIZE 16 // 缓冲区的大小是16字节

// echo hello too | echo  bye
int
main(int argc, char *argv[]){
    // sleep(10);
    // Q1:获取前一个命令的标准化输出"hello too"（即此命令的标准化输入）
    char buf[MSGSIZE]; 
    read(0, buf, MSGSIZE); // 从标准输入读取数据，并将其存储在 buf 中

    // Q2:获取到自己的命令行参数"bye""
    char *xargv[MAXARG]; // 存储执行命令的参数，类似于argv[]
    int xargc = 0; // 参数个数，类似于argc
    for (int i = 1; i < argc; i++){ //将命令行参数（除了程序名argv[0]）都复制到 xargv 中
        xargv[xargc] = argv[i];
        xargc ++;
    }
    char *p = buf; //p 指向读取的数据 buf 的起始位置
    for (int i = 0; i < MSGSIZE; i++){
        if (buf[i] == '\n'){ //在buff寻找到一行完整的输入
            int pid = fork();
            if (pid > 0){
                p = &buf[i+1];
                wait(0);
            }
            else{
                // Q3:执行exec()将hello too和bye拼接
                buf[i] = 0; //将\n替换为\0
                xargv[xargc] = p; //将当前行内容的起始地址赋值给xargv数组的下一个空位置
                xargc ++;
                xargv[xargc] = 0;
                xargc ++;

                exec(xargv[0], xargv);
                exit(0);
            }
        }
    }
    wait(0);
    exit(0);
}
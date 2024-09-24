#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int p_left) {
    int prime; //存储质数
    if (read(p_left, &prime, sizeof(prime)) == 0) {
        // read()函数每次读取一个，即读取第一个质数
        exit(0); // 没有更多数据了，退出
    }

    printf("prime %d\n", prime);

    // 创建管道给下一个进程
    int p[2];
    pipe(p);

    if (fork() == 0) {
        close(p[1]); // 子进程只读
        sieve(p[0]); // 递归进入下一个筛选器
    } else {
        close(p[0]); // 父进程只写
        int num;
        while (read(p_left, &num, sizeof(num)) > 0) {
            if (num % prime != 0) {
                write(p[1], &num, sizeof(num)); // 非倍数传递到下一个进程
            }
        }
        close(p[1]); // 关闭写端，通知子进程结束
        wait(0); // 等待子进程结束
        exit(0);
    }
}

int main(int argc, char *argv[]) {
    int p[2];
    pipe(p);

    if (fork() == 0) {
        close(p[1]); // 子进程只读
        sieve(p[0]); // 开始筛选
    } else {
        close(p[0]); // 父进程只写

        // 生成2到35的数字
        for (int i = 2; i <= 35; i++) {
            write(p[1], &i, sizeof(i));
        }

        close(p[1]); // 关闭写端，通知子进程结束
        wait(0); // 等待子进程结束
        exit(0);
    }
    return 0;
}
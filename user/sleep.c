// 为 xv6 实现 UNIX 程序 sleep

#include "kernel/types.h" // 包含内核定义的数据类型
#include "kernel/stat.h"  // 包含文件状态的定义
#include "user/user.h"    // 包含用户空间的库函数和系统调用声明

// 主函数入口，argc 是命令行参数的数量，argv 是命令行参数的数组
int
main(int argc, char *argv[])
{
    // 如果没有传递足够的参数，即只有程序名，没有其他参数
    if (argc == 1)
    {
        // 打印提示信息，要求用户输入参数
        printf("Please enter the parameters!\n");
    }
    else
    {
        // 将命令行传递的第二个参数（argv[1]）转换为整数，即 sleep 的持续时间
        int duration = atoi(argv[1]);

        // 调用 sleep 系统调用，挂起当前进程指定的时间（以 ticks 为单位）
        sleep(duration);
    }

    // 程序正常退出
    exit(0);
}

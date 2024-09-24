#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[]){
    int pipeAtoB[2]; //管道A
    int pipeBtoA[2]; //管道B
    char buffer[20]; //数据存储
    
    pipe(pipeAtoB);
    pipe(pipeBtoA);
    int pid = fork();
    if (pid > 0){ //父进程A
        close(pipeAtoB[0]); //关闭读端，专注于向B发送数据
        close(pipeBtoA[1]); //关闭写端，专注于从B读取数据

        //向B写数据“ping”
        char *message = "ping";
        write(pipeAtoB[1], message, strlen(message) + 1);

        //从B读取数据“pong”
        read(pipeBtoA[0], buffer, sizeof(buffer));
        printf("%d: received %s\n", getpid(), buffer);

        close(pipeAtoB[1]);
        close(pipeBtoA[0]);
        exit(0);
    }
    else if(pid == 0){ //子进程B
        close(pipeAtoB[1]); //关闭写端，专注于向A发送数据
        close(pipeBtoA[0]); //关闭读端，专注于从A读取数据

        //从A读取数据“ping”
        read(pipeAtoB[0], buffer, sizeof(buffer));
        printf("%d: received %s\n", getpid(), buffer);

        //向A写数据“pong”
        char *message = "pong";
        write(pipeBtoA[1], message, strlen(message) + 1);

        close(pipeAtoB[0]);
        close(pipeBtoA[1]);
        exit(0);

    }
    else{
        printf("fork error\n");
        exit(1);
    }

}

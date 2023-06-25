#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int i;
  char *nargv[MAXARG];

  if(argc < 3 || (argv[1][0] < '0' || argv[1][0] > '9')){
    fprintf(2, "Usage: %s mask command\n", argv[0]);
    exit(1);
  }
  // 使用 trace() 系统调用，追踪本进程及其子进程，所有代号与 argv[1] 位标识对应的系统调用
  // 如：trace 2，2的二进制为 0000 .... 0010b，说明仅代号为1的系统调用，即sys_fork，需要被追踪
  // 因为需要追踪本进程及其子进程，因此需要在表示一个进程信息 proc 结构体中添加一个 int 位表示信息
  if (trace(atoi(argv[1])) < 0) {
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }
  
  for(i = 2; i < argc && i < MAXARG; i++){
    nargv[i-2] = argv[i];
  }
  exec(nargv[0], nargv);
  exit(0);
}

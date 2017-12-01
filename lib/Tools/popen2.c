#include "popen2.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int popen2(const char *cmdline, struct popen2 *childinfo) {
  pid_t p;
  int pipe_stdin[2], pipe_stdout[2];

  if(pipe(pipe_stdin)) return -1;
  if(pipe(pipe_stdout)) return -1;

  p = fork();
  if(p < 0) return p; /* Fork failed */
  if(p == 0) { /* child */
    close(pipe_stdin[1]);
    dup2(pipe_stdin[0], 0);
    close(pipe_stdout[0]);
    dup2(pipe_stdout[1], 1);
    execl("/bin/sh", "sh", "-c", cmdline, 0);
    perror("execl"); exit(99);
  }
  childinfo->child_pid = p;
  childinfo->to_child = pipe_stdin[1];
  childinfo->from_child = pipe_stdout[0];
  return 0; 
}

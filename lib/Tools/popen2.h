#ifndef popen2_H
#define popen2_H

#include <sys/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct popen2 {
    pid_t child_pid;
    int   from_child, to_child;
};

int popen2(char const *, char *const[], struct popen2 *);

#if defined(__cplusplus)
}
#endif

#endif

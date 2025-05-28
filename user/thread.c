#include "kernel/types.h"
#include "user/user.h"
#include "thread.h"

int 
thread_create(void (*start_routine)(void *, void *), void *arg1, void *arg2) 
{
    void *stack = malloc(2*THREAD_STACK_SIZE);
    if (stack == 0) {
        return -1;
    }
    
    int tid = clone(start_routine, arg1, arg2, stack);
    
    if (tid < 0) {
        free(stack);
        return -1;
    }
    
    return tid;
}

int 
thread_join(void) 
{
    void *stack = 0;
    int pid = join(&stack);
    if(pid < 0)
        return -1;
    free(stack);
    return pid;
}
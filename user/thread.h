#ifndef THREAD_H
#define THREAD_H

#define THREAD_STACK_SIZE 4096

int thread_create(void (*start_routine)(void *, void *), void *arg1, void *arg2);
int thread_join(void);

#endif
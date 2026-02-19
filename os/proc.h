#ifndef PROC_H
#define PROC_H

#include "types.h"

#define NPROC (16)

#define MAX_SYSCALL_NUM 500  // Safe upper limit covering ID 410 
// since highest syscall is 410 from syscall_ids.h
// redefined here from stddef.h

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	uint64 ustack; // Virtual address of user stack
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	/*
	* LAB1: you may need to add some new fields here
	*/
	// PLaced here to have a place to remember stats for every process
    unsigned int syscall_times[MAX_SYSCALL_NUM]; // To count the calls
	// So when the processes switch, the variables stay with the processes
    uint64 start_time;
	// when the process started
};

/*
* LAB1: you may need to define struct for TaskInfo here
*/
typedef int TaskStatus;

struct TaskInfo {
    //Status for if the task is running, sleeping, or just ready to run
    TaskStatus status;
	// array of set of counters
	// Each index corresponds to a system call ID number
	// Index value is number of times it has been called
    unsigned int syscall_times[MAX_SYSCALL_NUM];
	// total task run time (ms)
    int time;
};

struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
struct proc *allocproc();
// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H
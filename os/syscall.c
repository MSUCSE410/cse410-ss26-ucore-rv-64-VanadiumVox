#include "console.h"
#include "defs.h"
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
// Added a forward declaration for the banker's algorithm function
// Because otherwise the mutex and semaphore functions would throw 
// implicit declaration warnings
int deadlock_detect(const int available[LOCK_POOL_SIZE], const int allocation[NTHREAD][LOCK_POOL_SIZE], const int request[NTHREAD][LOCK_POOL_SIZE]);

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_PIPE:
		return piperead(f->pipe, va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

/*
*	LAB5: (3) In the TA's reference implementation, here defines funtion
*					int deadlock_detect(const int available[LOCK_POOL_SIZE],
*						const int allocation[NTHREAD][LOCK_POOL_SIZE],
*						const int request[NTHREAD][LOCK_POOL_SIZE])
*				for both mutex and semaphore detect, you can also
*				use this idea or just ignore it.
*/

int sys_mutex_create(int blocking)
{
	struct mutex *m = mutex_create(blocking);
	if (m == NULL) {
		errorf("fail to create mutex: out of resource");
		return -1;
	}
	// LAB5: (4-1) You may want to maintain some variables for detect here
	int mutex_id = m - curr_proc()->mutex_pool;
	debugf("create mutex %d", mutex_id);
	////////////////////////////////////////////////////////////////
	// Whenever a lock is created, we have to register its available capacity
	// into the banker's "available" matrix. So the algorithm knows it exists
    curr_proc()->mut_available[mutex_id] = 1;
	return mutex_id;
}

int sys_mutex_lock(int mutex_id)
{
    if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
        errorf("Unexpected mutex id %d", mutex_id);
        return -1;
    }

    struct proc *p = curr_proc();
    int tid = curr_thread() - p->threads; 
    
    p->mut_request[tid][mutex_id]++;
    /////////////////////////////////////////////////////////////////////
    // I replaced this entire function to intercept the deadlocks before the system freezes.
	// We're registering the thread's request in the matrix and run the algorithm. 
	// If a deadlock is detected, we log the event, revert the request, and return a specific error code 
    if (p->deadlock_detect_enabled && deadlock_detect(p->mut_available, p->mut_allocation, p->mut_request)) {
        errorf("detect deadlock on locking mutex %d!", mutex_id);
        p->mut_request[tid][mutex_id]--; // Reverting the request
		return -0xDEAD; // Returns the specific code expected by tests    
		}
    
    mutex_lock(&p->mutex_pool[mutex_id]);
    
    p->mut_request[tid][mutex_id]--;
    p->mut_allocation[tid][mutex_id]++;
    p->mut_available[mutex_id]--;
    return 0;
}

int sys_mutex_unlock(int mutex_id)
{
    if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
        errorf("Unexpected mutex id %d", mutex_id);
        return -1;
    }

    struct proc *p = curr_proc();
    int tid = curr_thread() - p->threads;
    ///////////////////////////////////////////////////////////////////
    // This functions was replaced to properly decrement the thread's allocation matrix
	// and increment available matrix. Also added a bounds check to make sure that the
	// math never goes into negative numbers, which breaks the algorithm.
    if (p->mut_allocation[tid][mutex_id] > 0) {
        p->mut_allocation[tid][mutex_id]--;
    }
    p->mut_available[mutex_id]++;
    
    mutex_unlock(&p->mutex_pool[mutex_id]);
    return 0;
}

int sys_semaphore_create(int res_count)
{
	struct semaphore *s = semaphore_create(res_count);
	if (s == NULL) {
		errorf("fail to create semaphore: out of resource");
		return -1;
	}
	// LAB5: (4-2) You may want to maintain some variables for detect here
	int sem_id = s - curr_proc()->semaphore_pool;
	debugf("create semaphore %d", sem_id);
	//////////////////////////////////////////////////////////////
	// This is the initialization for bankers algorithm. When a new semaphore is 
	// made, we must register its resource count into the avilable matrix. 
	// If we skip this, the algorithm will assume the semaphore has 0 resources, 
	// causing a deadlock panic whenever a thread tries to acquire it.
	curr_proc()->sem_available[sem_id] = res_count;
	return sem_id;
}

int sys_semaphore_down(int semaphore_id)
{
    if (semaphore_id < 0 || semaphore_id >= curr_proc()->next_semaphore_id) {
        errorf("Unexpected semaphore id %d", semaphore_id);
        return -1;
    }

    struct proc *p = curr_proc();
    int tid = curr_thread() - p->threads;
    
	//This registers the intent, which, before checking for a deadlock, we must
	// recorcd that the thread wants this semaphore
    p->sem_request[tid][semaphore_id]++;
    
    // This is the interception. We run banker's algorithm using the updated request matrix
	// If it detects a deadlock, we step in and stop it.
    if (p->deadlock_detect_enabled && deadlock_detect(p->sem_available, p->sem_allocation, p->sem_request)) {
        errorf("detect deadlock on down semaphore %d!", semaphore_id);

		// This is the reversal, since we're denying the lock to prevent deadlock,
		// we have to erase the request from the matrix to keep it accurate
        p->sem_request[tid][semaphore_id]--; 
		return -0xDEAD; // Return the specific easter-egg code expected by the tests    
		}
    
    semaphore_down(&p->semaphore_pool[semaphore_id]);
    
    p->sem_request[tid][semaphore_id]--;
    p->sem_allocation[tid][semaphore_id]++;
    p->sem_available[semaphore_id]--;
    return 0;
}

int sys_semaphore_up(int semaphore_id)
{
    if (semaphore_id < 0 || semaphore_id >= curr_proc()->next_semaphore_id) {
        errorf("Unexpected semaphore id %d", semaphore_id);
        return -1;
    }

    struct proc *p = curr_proc();
    int tid = curr_thread() - p->threads;
    
    // This is the protection check, if a thread is behaving badly, and tries to release
	// a lock it doesn't own, decrementing the allocation matrix would make it negative. 
    if (p->sem_allocation[tid][semaphore_id] > 0) {
        p->sem_allocation[tid][semaphore_id]--;
    }
    p->sem_available[semaphore_id]++;
    // Execute the actual release here
    semaphore_up(&p->semaphore_pool[semaphore_id]);
    return 0;
}

int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}

int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}

int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	cond_wait(&curr_proc()->condvar_pool[cond_id],
		  &curr_proc()->mutex_pool[mutex_id]);
	return 0;
}

// LAB5: (2) you may need to define function enable_deadlock_detect here
//////////////////////////////////////////////////////////////
// This system call is allowing a process to toggle its own deadlock 
// detection flag. It simply grabs the current process's PCB and flips the flag
uint64 sys_enable_deadlock_detect(int is_enable) {
    struct proc *p = curr_proc();
    p->deadlock_detect_enabled = is_enable;
    return 0;
}
// The custom implementation of Banker's algorithm for deadlock detection
// which simulates granting the requested resources. Added an is_empty check below
// where a thread is only finished if it holds 0 allocations and pending requests.
int deadlock_detect(const int available[LOCK_POOL_SIZE],
                    const int allocation[NTHREAD][LOCK_POOL_SIZE],
                    const int request[NTHREAD][LOCK_POOL_SIZE]) {
    int work[LOCK_POOL_SIZE];
    int finish[NTHREAD];
    
    for (int i = 0; i < LOCK_POOL_SIZE; i++) work[i] = available[i];
    
    // fixed: A thread is only "empty" if it holds no allocations AND has 0 requests
    for (int i = 0; i < NTHREAD; i++) {
        int is_empty = 1; // Is empty check
        for (int j = 0; j < LOCK_POOL_SIZE; j++) {
            if (allocation[i][j] > 0 || request[i][j] > 0) { is_empty = 0; break; }
        }
        finish[i] = is_empty;
    }
    
    while (1) {
        int found = 0;
        for (int i = 0; i < NTHREAD; i++) {
            if (!finish[i]) {
                int can_grant = 1;
                for (int j = 0; j < LOCK_POOL_SIZE; j++) {
                    if (request[i][j] > work[j]) { can_grant = 0; break; }
                }
                if (can_grant) {
                    for (int j = 0; j < LOCK_POOL_SIZE; j++) work[j] += allocation[i][j];
                    finish[i] = 1;
                    found = 1;
                }
            }
        }
        if (!found) break;
    }
    
    for (int i = 0; i < NTHREAD; i++) {
        if (!finish[i]) return 1; // Deadlock detected!
    }
    return 0; // System is safe
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	// case SYS_nanosleep:
	// 	ret = sys_nanosleep(args[0]);
	// 	break;
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	// LAB5: (2) you may need to add case SYS_enable_deadlock_detect here
	 /////////////////////////////////////////////////////////////////////
	case SYS_enable_deadlock_detect:
        ret = sys_enable_deadlock_detect(args[0]);
        break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}

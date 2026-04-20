#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"
#include "fs.h"
#include "file.h"

typedef struct {
    uint64 dev;
    uint64 ino;
    uint32 mode;
    uint32 nlink;
    uint64 pad[7];
} Stat;

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

// uint64 sys_spawn(uint64 va)
// {
// 	// TODO: your job is to complete the sys call
// 	return -1;
// }
/////////////////// DOne below ////////////////////
// uint64 sys_set_priority(long long prio)
// {
// 	// TODO: your job is to complete the sys call
// 	return -1;
// }

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

// int sys_fstat(int fd,uint64 stat){
// 	//TODO: your job is to complete the syscall
// 	return -1;
// }
//////////////////// Done Below ////////////////////
// int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
// 	//TODO: your job is to complete the syscall
// 	return -1;
// }

// int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
// 	//TODO: your job is to complete the syscall
// 	return -1;
// }

extern char trap_page[];

extern uint64 useraddr(pagetable_t pagetable, uint64 va);
extern uint64 walkaddr(pagetable_t pagetable, uint64 va);
extern void* kalloc(void);
extern void kfree(void *pa);
extern int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);
extern void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free);
extern int spawn(char *name);

uint64 sys_task_info(uint64 ti_va) {
    struct proc *p = curr_proc(); 
    if (ti_va == 0) return -1;
    struct TaskInfo local_ti;
    local_ti.status = 2; 
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        local_ti.syscall_times[i] = p->syscall_times[i];
    }
    if (p->start_time == 0 || CPU_FREQ == 0) {
        local_ti.time = 0;
    } else {
        local_ti.time = (int)((get_cycle() - p->start_time) / (CPU_FREQ / 1000));
    }
    uint8 *src = (uint8 *)&local_ti;
    for(int i = 0; i < sizeof(struct TaskInfo); i++){
        uint64 pa = useraddr(p->pagetable, ti_va + i);
        if (pa == 0) return -1;
        *(uint8 *)pa = src[i];
    }
    return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
    if (start % 4096 != 0) return -1;
    if (len == 0) return 0; 
    if (len > 1024 * 1024 * 1024) return -1; 
    if ((port & ~0x7) != 0) return -1; 
    if ((port & 0x7) == 0) return -1; 

    uint64 a_len = (len + 4096ULL - 1) & ~(4096ULL - 1);
    struct proc *p = curr_proc();

    for (uint64 va = start; va < start + a_len; va += 4096) {
        if (walkaddr(p->pagetable, va) != 0) return -1; 
    }

    int perm = 1 | 16; // PTE_V | PTE_U
    if (port & 1) perm |= 2; // PTE_R
    if (port & 2) perm |= 4; // PTE_W
    if (port & 4) perm |= 8; // PTE_X

    for (uint64 va = start; va < start + a_len; va += 4096) {
        void *pa = kalloc();
        if (pa == 0) return -1; 
        
        if (mappages(p->pagetable, va, 4096, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
        uint64 current_page = va / 4096;
        if (current_page > p->max_page) {
            p->max_page = current_page;
        }
    }
    return 0; 
}

uint64 sys_munmap(uint64 start, uint64 len) {
    if (start % 4096 != 0) return -1;
    uint64 a_len = (len + 4096ULL - 1) & ~(4096ULL - 1);
    struct proc *p = curr_proc();

    for (uint64 va = start; va < start + a_len; va += 4096) {
        if (walkaddr(p->pagetable, va) == 0) return -1; 
    }
    uvmunmap(p->pagetable, start, a_len / 4096, 1);
    return 0; 
}

uint64 sys_spawn(uint64 va) {
    struct proc *p = curr_proc();
    char name[200];
    copyinstr(p->pagetable, name, va, 200);
    return spawn(name);
}

uint64 sys_setpriority(long long prio) {
    if (prio < 2) return -1;
    struct proc *p = curr_proc();
    p->priority = prio;
    p->pass = BIG_STRIDE / p->priority;
    return prio;
}

uint64 sys_linkat(int olddirfd, uint64 oldpath_va, int newdirfd, uint64 newpath_va, uint64 flags) {
    char oldpath[200], newpath[200];
    struct proc *p = curr_proc();
    
    // Grab both strings from user memory
    copyinstr(p->pagetable, oldpath, oldpath_va, 200);
    copyinstr(p->pagetable, newpath, newpath_va, 200);

    // Find the original file
    struct inode *ip = namei(oldpath);
    if (ip == 0) return -1; 

    // Increment the hard link counter and save to disk
    ivalid(ip);
    ip->nlink++;
    iupdate(ip); 

    // Manually add the new name to the root directory
    struct inode *dp = root_dir();
    if (dirlink(dp, newpath, ip->inum) < 0) {
        // If linking fails, rollback the counter
        ip->nlink--;
        iupdate(ip);
        return -1;
    }
    return 0;
}

uint64 sys_unlinkat(int dirfd, uint64 path_va, uint64 flags) {
    char path[200];
    struct proc *p = curr_proc();
    
    // Bulletproof argument parsing: Try args[1] first. 
    // If invalid, the wrapper might be passing the path in args[0] (dirfd).
    if (copyinstr(p->pagetable, path, path_va, 200) < 0) {
        if (copyinstr(p->pagetable, path, dirfd, 200) < 0) return -1;
    }

    struct inode *dp = root_dir();
    uint off;
    
    // Find the file and record its exact byte-offset in the directory
    struct inode *ip = dirlookup(dp, path, &off);
    if (ip == 0) return -1;

    ivalid(ip);
    ip->nlink--;
    iupdate(ip);

    // Destroy the file's name by overwriting it with an empty directory entry
    struct dirent de;
    memset(&de, 0, sizeof(de));
    writei(dp, 0, (uint64)&de, off, sizeof(de));

    // Drop our reference. If nlink hit 0, this triggers your deletion logic!
    iput(ip); 
    return 0;
}

uint64 sys_fstat(int fd, uint64 stat_va) {
    struct proc *p = curr_proc();
    struct file *f = p->files[fd]; 
    if (f == 0 || f->type != FD_INODE) return -1;

    struct inode *ip = f->ip; 
    ivalid(ip);

    // Package the file status
    Stat st;
    memset(&st, 0, sizeof(Stat)); // Clear all padding bytes so the test reads clean memory
    
    st.dev = ip->dev;
    st.ino = ip->inum;
    st.nlink = ip->nlink;
    
    // Map the internal OS type (2) to the user program's strict hex constant (0x100000)
    if (ip->type == 2) st.mode = 0x100000;
    else if (ip->type == 1) st.mode = 0x040000;
    else st.mode = ip->type;
    
    // Send it back to the user program
    copyout(p->pagetable, stat_va, (char *)&st, sizeof(Stat));
    return 0;
}

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
    curr_proc()->syscall_times[id]++;
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
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_task_info:
    	ret = sys_task_info(args[0]); 
    	break;
	case SYS_mmap:
	    ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
	    break;
	case SYS_munmap:
	    ret = sys_munmap(args[0], args[1]);
	    break;
	case SYS_setpriority:
	    ret = sys_setpriority(args[0]);
	    break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}

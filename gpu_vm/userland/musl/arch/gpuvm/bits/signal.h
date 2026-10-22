/* gpu_vm (arch/gpuvm in the kernel): asm-generic's signals with no SA_RESTORER (a handler returns through the
   kernel's __user_rt_sigreturn, which the kernel puts in ra); the machine context is the kernel's sigcontext, the 64
   registers with the pc in word 0; ucontext is asm-generic's, the mask last. */
#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#if defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192
#endif

typedef struct mcontext_t {
	unsigned long __gregs[64];
} mcontext_t;

#if defined(_GNU_SOURCE)
#define REG_PC 0
#define REG_RA 1
#define REG_SP 2
#define REG_TP 4
#define REG_A0 5
#define REG_S0 32
#endif

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
typedef unsigned long greg_t;
typedef unsigned long gregset_t[64];
struct sigcontext {
	gregset_t gregs;
};
#endif

struct sigaltstack {
	void *ss_sp;
	int ss_flags;
	size_t ss_size;
};

typedef struct __ucontext {
	unsigned long uc_flags;
	struct __ucontext *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	sigset_t uc_sigmask;
} ucontext_t;

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#endif

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     SIGABRT
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED SIGSYS

#define _NSIG     65

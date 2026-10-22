/* No fork without an MMU: the kernel lets clone() without CLONE_VM through, and the child would then run on the
   parent's memory and stack at the same time as it. vfork, posix_spawn (CLONE_VM | CLONE_VFORK) and system and popen
   (posix_spawn) are the ways to start a program. */
#include <errno.h>
#include <unistd.h>
#include "syscall.h"

pid_t _Fork(void)
{
	return __syscall_ret(-ENOSYS);
}

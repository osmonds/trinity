/*
 * SYSCALL_DEFINE3(sched_getattr, pid_t, pid, struct sched_attr __user *, uattr, unsigned int, size)
 */
#include <stdlib.h>
#include "arch.h"
#include "sanitise.h"
#include "shm.h"

#define SCHED_ATTR_SIZE_VER0	48

static void sanitise_sched_getattr(int childno)
{
	unsigned long range = page_size - SCHED_ATTR_SIZE_VER0;

	shm->syscall[childno].a3 = (rand() % range) + SCHED_ATTR_SIZE_VER0;
}

struct syscallentry syscall_sched_getattr = {
	.name = "sched_getattr",
	.num_args = 3,
	.arg1name = "pid",
	.arg1type = ARG_PID,
	.arg2name = "param",
	.arg2type = ARG_ADDRESS,
	.arg3name = "size",
	.sanitise = sanitise_sched_getattr,
};

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ptrace.h>

#include "arch.h" // biarch
#include "child.h"
#include "files.h"
#include "locks.h"
#include "log.h"
#include "params.h"	// quiet_level
#include "pids.h"
#include "shm.h"
#include "syscall.h"
#include "tables.h"
#include "taint.h"
#include "trinity.h" //check_taint

pid_t watchdog_pid;

static unsigned long hiscore = 0;

static int check_shm_sanity(void)
{
	unsigned int i;

	if (shm->running_childs == 0)
		return SHM_OK;

	for_each_pidslot(i) {
		pid_t pid;

		pid = shm->pids[i];
		if (pid == EMPTY_PIDSLOT)
			continue;

		if (pid_is_valid(pid) == FALSE) {
			shm->exit_reason = EXIT_PID_OUT_OF_RANGE;
			return SHM_CORRUPT;
		}
	}

	// FIXME: The '500000' is magic, and should be dynamically calculated.
	// On startup, we should figure out how many getpid()'s per second we can do,
	// and use that.
	if (shm->total_syscalls_done - shm->previous_op_count > 500000) {
		output(0, "Execcount increased dramatically! (old:%ld new:%ld):\n",
			shm->previous_op_count, shm->total_syscalls_done);
		shm->exit_reason = EXIT_SHM_CORRUPTION;
	}
	shm->previous_op_count = shm->total_syscalls_done;

	return SHM_OK;
}

static unsigned int reap_dead_kids(void)
{
	unsigned int i;
	unsigned int alive = 0;
	unsigned int reaped = 0;

	for_each_pidslot(i) {
		pid_t pid;
		int ret;

		pid = shm->pids[i];
		if (pid == EMPTY_PIDSLOT)
			continue;

		ret = kill(pid, 0);
		/* If it disappeared, reap it. */
		if (ret == -1) {
			if (errno == ESRCH) {
				output(0, "pid %d has disappeared (oom-killed maybe?). Reaping.\n", pid);
				reap_child(pid);
				reaped++;
			} else {
				output(0, "problem checking on pid %d (%d:%s)\n", pid, errno, strerror(errno));
			}
		} else {
			alive++;
		}

		if (shm->running_childs == 0)
			return 0;
	}

	if (reaped != 0)
		output(0, "Reaped %d dead children\n", reaped);

	return alive;
}

static void kill_all_kids(void)
{
	unsigned int i;

	shm->spawn_no_more = TRUE;

	/* Wait for all the children to exit. */
	while (shm->running_childs > 0) {
		unsigned int alive;

		/* Make sure there's no dead kids lying around.
		 * We need to do this in case the oom killer has been killing them,
		 * otherwise we end up stuck here with no child processes.
		 */
		alive = reap_dead_kids();
		if (alive == 0)
			return;

		/* Ok, some kids are still alive. 'help' them along with a SIGKILL */
		for_each_pidslot(i) {
			pid_t pid;

			pid = shm->pids[i];
			if (pid == EMPTY_PIDSLOT)
				continue;

			kill(pid, SIGKILL);
		}

		/* wait a second to give kids a chance to exit. */
		sleep(1);

		if (check_shm_sanity()) {
			// FIXME: If we get here, we over-wrote the real exit_reason.
			// We should have saved that, and handled appropriately.
			return;
		}
	}

	/* Just to be sure, clear out the pid slots. */
	for_each_pidslot(i) {
		shm->pids[i] = EMPTY_PIDSLOT;
	}
}

static bool __check_main(void)
{
	int ret;

	if (shm->mainpid == 0)
		return FALSE;

	ret = kill(shm->mainpid, 0);
	if (ret == -1) {
		if (errno == ESRCH) {
			output(0, "main pid %d has disappeared.\n", shm->mainpid);
			shm->exit_reason = EXIT_MAIN_DISAPPEARED;
			shm->mainpid = 0;

			/* if main crashed while regenerating, we'll hang the watchdog,
			 * because nothing will ever set it back to FALSE. So we do it ourselves.
			 */
			shm->regenerating = FALSE;
		} else {
			output(0, "problem checking on pid %d (%d:%s)\n", shm->mainpid, errno, strerror(errno));
		}
		return FALSE;
	}
	return TRUE;
}

static int check_main_alive(void)
{
	int ret;

	/* If we're in the process of exiting, wait, and return without checking. */
	if (shm->exit_reason != STILL_RUNNING) {
		while (shm->mainpid != 0) {
			/* make sure main is still alive, to wait for kids. */
			ret = __check_main();
			if (ret == TRUE) {
				sleep(1);
				kill_all_kids();
			}
		}
		return FALSE;
	}

	ret = __check_main();
	return ret;
}

/* if the first arg was an fd, find out which one it was. */
unsigned int check_if_fd(unsigned int child)
{
	unsigned int fd = shm->syscall[child].a1;
	unsigned int highest;
	unsigned callno;
	bool do32;

	/* shortcut, if it's out of range, it's not going to be valid. */
	if (fd > 1024)
		return FALSE;

	highest = highest_logfile();
	if (fd < highest)
		return FALSE;

	lock(&shm->syscall_lock);
	callno = shm->syscall[child].nr;
	do32 = shm->syscall[child].do32bit;
	unlock(&shm->syscall_lock);

	if (biarch == FALSE) {
		if (syscalls[callno].entry->arg1type == ARG_FD)
			return TRUE;
		return FALSE;
	}

	/* biarch case */
	if (do32 == TRUE) {
		if (syscalls_32bit[callno].entry->arg1type == ARG_FD)
			return TRUE;
	} else {
		if (callno > max_nr_64bit_syscalls) {
			output(0, "Weird, child:%d callno:%d (64bit max:%d)\n", child, callno, max_nr_64bit_syscalls);
			return FALSE;
		}
		if (syscalls_64bit[callno].entry->arg1type == ARG_FD)
			return TRUE;
	}

	return FALSE;
}

static void stuck_syscall_info(int childno)
{
	unsigned int callno = shm->syscall[childno].nr;
	char fdstr[20];
	pid_t pid = shm->pids[childno];

	memset(fdstr, 0, sizeof(fdstr));

	if (check_if_fd(childno) == TRUE)
		sprintf(fdstr, "(fd = %d)", (unsigned int) shm->syscall[childno].a1);

	output(0, "[%d] Stuck in syscall %d:%s%s%s.\n",
		pid, callno,
		print_syscall_name(callno, shm->syscall[childno].do32bit),
		shm->syscall[childno].do32bit ? " (32bit)" : "",
		fdstr);
}

static void check_children(void)
{
	struct timeval tv;
	time_t diff;
	time_t old, now;
	unsigned int i;

	for_each_pidslot(i) {
		pid_t pid;

		pid = shm->pids[i];

		if (pid == EMPTY_PIDSLOT)
			continue;

		old = shm->tv[i].tv_sec;

		if (old == 0)
			continue;

		gettimeofday(&tv, NULL);
		now = tv.tv_sec;

		/* if we wrapped, just reset it, we'll pick it up next time around. */
		if (old > (now + 3)) {
			output(1, "child %u wrapped! old=%lu now=%lu\n", i, old, now);
			shm->tv[i].tv_sec = now;
			continue;
		}

		diff = now - old;

		/* if we're way off, we're comparing garbage. Reset it. */
		if (diff > 1000) {
			output(0, "huge delta! pid slot %d [%d]: old:%ld now:%ld diff:%d.  Setting to now.\n", i, pid, old, now, diff);
			shm->tv[i].tv_sec = now;
			continue;
		}

		/* After 30 seconds of no progress, send a kill signal. */
		if (diff == 30) {
			stuck_syscall_info(i);
			output(0, "pid %d hasn't made progress in 30 seconds! (last:%ld now:%ld diff:%d)\n",
				pid, old, now, diff);
		}

		if (diff >= 30) {
			int ret;

			if (shm->kill_count[i] > 1) {
				output(0, "sending another SIGKILL to pid %d. [kill count:%d] [diff:%d]\n",
					pid, shm->kill_count[i], diff);
			} else {
				output(0, "sending SIGKILL to pid %d. [diff:%d]\n",
					pid, diff);
			}
			shm->kill_count[i]++;
			ret = kill(pid, SIGKILL);
			if (ret != 0) {
				output(0, "couldn't kill pid %d [%s]\n", pid, strerror(errno));
			}
			sleep(1);	// give child time to exit.
		}
	}
}

static void watchdog(void)
{
	static const char watchdogname[17]="trinity-watchdog";
	static unsigned long lastcount = 0;
	bool watchdog_exit = FALSE;
	int ret = 0;

	while (shm->ready == FALSE) {
		sleep(1);
		if (shm->exit_reason != STILL_RUNNING)
			return;
	}

	output(0, "Watchdog is alive. (pid:%d)\n", watchdog_pid);

	prctl(PR_SET_NAME, (unsigned long) &watchdogname);
	(void)signal(SIGSEGV, SIG_DFL);

	while (watchdog_exit == FALSE) {

		if (check_shm_sanity() == SHM_CORRUPT)
			goto corrupt;

		if (check_main_alive() == FALSE)
			goto main_dead;

		if (shm->regenerating == FALSE) {
			unsigned int i;

			reap_dead_kids();

			check_children();

			if (syscalls_todo && (shm->total_syscalls_done >= syscalls_todo)) {
				output(0, "Reached limit %d. Telling children to exit.\n", syscalls_todo);
				shm->exit_reason = EXIT_REACHED_COUNT;
			}

			// Periodic log syncing. FIXME: This is kinda ugly, and mostly unnecessary.
			if (shm->total_syscalls_done % 1000 == 0)
				synclogs();

			for_each_pidslot(i) {
				if (shm->child_op_count[i] > hiscore)
					hiscore = shm->child_op_count[i];
			}

			if (shm->total_syscalls_done > 1) {
				if (shm->total_syscalls_done - lastcount > 10000) {
					output(0, "%ld iterations. [F:%ld S:%ld HI:%ld]\n",
						shm->total_syscalls_done,
						shm->failures, shm->successes,
						hiscore);
					lastcount = shm->total_syscalls_done;
				}
			}

		}

		/* Only check taint if it mask allows it */
		if (kernel_taint_mask != 0) {
			ret = check_tainted();
			if (((ret & kernel_taint_mask) & (~kernel_taint_initial)) != 0) {
				gettimeofday(&shm->taint_tv, NULL);

				output(0, "kernel became tainted! (%d/%d) Last seed was %u\n", ret, kernel_taint_initial, shm->seed);
				shm->exit_reason = EXIT_KERNEL_TAINTED;
			}
		}

main_dead:
		/* Are we done ? */
		if (shm->exit_reason != STILL_RUNNING) {
			/* Give children a chance to exit. */
			sleep(1);

			/* Are there still children running ? */
			if (pidmap_empty() == TRUE)
				watchdog_exit = TRUE;
			else {
				output(0, "exit_reason=%d, but %d children still running.\n",
					shm->exit_reason, shm->running_childs);
				kill_all_kids();
			}
		}

		sleep(1);
	}

corrupt:
	/* We don't want to ever exit before main is waiting for us. */
	while (shm->regenerating == TRUE)
		sleep(1);

	kill_all_kids();
}

void init_watchdog(void)
{
	pid_t pid;

	fflush(stdout);
	pid = fork();

	if (pid == 0) {
		watchdog_pid = getpid();
		watchdog();
		output(0, "[%d] Watchdog exiting\n", watchdog_pid);
		_exit(EXIT_SUCCESS);

	} else {
		watchdog_pid = pid;
		output(0, "Started watchdog process, PID is %d\n", watchdog_pid);
	}
}

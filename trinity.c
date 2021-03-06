#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "arch.h"
#include "trinity.h"
#include "files.h"
#include "log.h"
#include "maps.h"
#include "pids.h"
#include "params.h"
#include "random.h"
#include "signals.h"
#include "shm.h"
#include "tables.h"
#include "ioctls.h"
#include "protocols.h"
#include "uid.h"
#include "config.h"	// for VERSION
#include "taint.h"

char *progname = NULL;

unsigned int page_size;
unsigned int num_online_cpus;
unsigned int max_children;

/*
 * just in case we're not using the test.sh harness, we
 * change to the tmp dir if it exists.
 */
static void change_tmp_dir(void)
{
	struct stat sb;
	const char tmpdir[]="tmp/";
	int ret;

	/* Check if it exists, bail early if it doesn't */
	ret = (lstat(tmpdir, &sb));
	if (ret == -1)
		return;

	/* Just in case a previous run screwed the perms. */
	ret = chmod(tmpdir, 0777);
	if (ret == -1)
		output(0, "Couldn't chmod %s to 0777.\n", tmpdir);

	ret = chdir(tmpdir);
	if (ret == -1)
		output(0, "Couldn't change to %s\n", tmpdir);
}

int main(int argc, char* argv[])
{
	int ret = EXIT_SUCCESS;
	int childstatus;
	pid_t pid;
	const char taskname[13]="trinity-main";

	outputstd("Trinity v" __stringify(VERSION) "  Dave Jones <davej@redhat.com>\n");

	progname = argv[0];

	initpid = getpid();

	page_size = getpagesize();
	num_online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	max_children = num_online_cpus;	/* possibly overridden in params. */

	select_syscall_tables();

	create_shm();

	parse_args(argc, argv);

	create_shm_arrays();

	init_uids();

	if (logging == TRUE)
		open_logfiles();

	init_shm();

	kernel_taint_initial = check_tainted();
	if (kernel_taint_initial != 0)
		output(0, "Kernel was tainted on startup. Will ignore flags that are already set.\n");

	if (munge_tables() == FALSE) {
		ret = EXIT_FAILURE;
		goto out;
	}

	if (show_syscall_list == TRUE) {
		dump_syscall_tables();
		goto out;
	}

	init_syscalls();

	if (show_ioctl_list == TRUE) {
		dump_ioctls();
		goto out;
	}

	do_uid0_check();

	if (do_specific_proto == TRUE)
		find_specific_proto(specific_proto_optarg);

	init_shared_pages();

	parse_devices();

	pids_init();

	setup_main_signals();

	change_tmp_dir();

	/* check if we ctrl'c or something went wrong during init. */
	if (shm->exit_reason != STILL_RUNNING)
		goto cleanup_fds;

	init_watchdog();

	/* do an extra fork so that the watchdog and the children don't share a common parent */
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		shm->mainpid = getpid();

		setup_main_signals();

		output(0, "Main thread is alive.\n");
		prctl(PR_SET_NAME, (unsigned long) &taskname);
		set_seed(0);

		if (setup_fds() == FALSE) {
			shm->exit_reason = EXIT_FD_INIT_FAILURE;	// FIXME: Later, push this down to multiple EXIT's.
			_exit(EXIT_FAILURE);
		}

		if (no_files == FALSE) {
			if (files_in_index == 0) {
				shm->exit_reason = EXIT_NO_FILES;
				_exit(EXIT_FAILURE);
			}
		}

		if (dropprivs == TRUE)	//FIXME: Push down into child processes later.
			drop_privs();

		main_loop();

		_exit(EXIT_SUCCESS);
	}

	/* wait for main loop process to exit. */
	(void)waitpid(pid, &childstatus, 0);

	/* wait for watchdog to exit. */
	waitpid(watchdog_pid, &childstatus, 0);

	output(0, "\nRan %ld syscalls. Successes: %ld  Failures: %ld\n",
		shm->total_syscalls_done - 1, shm->successes, shm->failures);

cleanup_fds:

	close_sockets();

	destroy_shared_mappings();

	if (logging == TRUE)
		close_logfiles();

out:

	exit(ret);
}

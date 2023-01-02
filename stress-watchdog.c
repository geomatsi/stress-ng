/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"

#if defined(HAVE_LINUX_WATCHDOG_H)
#include <linux/watchdog.h>
#endif

static const stress_help_t help[] = {
	{ NULL,	"watchdog N",	  "start N workers that exercise /dev/watchdog" },
	{ NULL,	"watchdog-ops N", "stop after N bogo watchdog operations" },
	{ NULL,	NULL,		  NULL }
};

#if defined(HAVE_LINUX_WATCHDOG_H)

static sigjmp_buf jmp_env;

static const int sigs[] = {
#if defined(SIGILL)
	SIGILL,
#endif
#if defined(SIGTRAP)
	SIGTRAP,
#endif
#if defined(SIGFPE)
	SIGFPE,
#endif
#if defined(SIGBUS)
	SIGBUS,
#endif
#if defined(SIGSEGV)
	SIGSEGV,
#endif
#if defined(SIGIOT)
	SIGIOT,
#endif
#if defined(SIGEMT)
	SIGEMT,
#endif
#if defined(SIGALRM)
	SIGALRM,
#endif
#if defined(SIGINT)
	SIGINT,
#endif
#if defined(SIGHUP)
	SIGHUP
#endif
};

static const char dev_watchdog[] = "/dev/watchdog";
static int fd;

static void stress_watchdog_magic_close(void)
{
	/*
	 *  Some watchdog drivers support the magic close option
	 *  where writing "V" will forcefully disable the watchdog
	 */
	if (fd >= 0) {
		VOID_RET(ssize_t, write(fd, "V", 1));
	}
}

static void NORETURN MLOCKED_TEXT stress_watchdog_handler(int signum)
{
	(void)signum;

	stress_watchdog_magic_close();

	/* trigger early termination */
	keep_stressing_set_flag(false);

	/* jump back */
	siglongjmp(jmp_env, 1);
}

/*
 *  stress_watchdog()
 *	stress /dev/watchdog
 */
static int stress_watchdog(const stress_args_t *args)
{
	int ret;
	size_t i;
	NOCLOBBER int rc = EXIT_SUCCESS;

	fd = -1;
	for (i = 0; i < SIZEOF_ARRAY(sigs); i++) {
		if (stress_sighandler(args->name, sigs[i], stress_watchdog_handler, NULL) < 0)
			return EXIT_FAILURE;
	}

	/*
	 *  Sanity check for existence and r/w permissions
	 *  on /dev/shm, it may not be configure for the
	 *  kernel, so don't make it a failure of it does
	 *  not exist or we can't access it.
	 */
	if (access(dev_watchdog, R_OK | W_OK) < 0) {
		if (errno == ENOENT) {
			if (args->instance == 0)
				pr_inf_skip("%s: %s does not exist, skipping test\n",
					args->name, dev_watchdog);
			return EXIT_SUCCESS;
		} else {
			if (args->instance == 0)
				pr_inf_skip("%s: cannot access %s, errno=%d (%s), skipping test\n",
					args->name, dev_watchdog, errno, strerror(errno));
			return EXIT_SUCCESS;
		}
	}

	fd = 0;
	ret = sigsetjmp(jmp_env, 1);
	if (ret) {
		/* We got interrupted, so abort cleanly */
		if (fd >= 0) {
			stress_watchdog_magic_close();
			(void)close(fd);
		}
		return EXIT_SUCCESS;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	while (keep_stressing(args)) {
		fd = open(dev_watchdog, O_RDWR);

		/* Multiple stressors can lock the device, so retry */
		if (fd < 0) {
			struct timespec tv;

			tv.tv_sec = 0;
			tv.tv_nsec = 10000;
			(void)nanosleep(&tv, NULL);
			continue;
		}

		stress_watchdog_magic_close();

#if defined(WDIOC_KEEPALIVE)
		VOID_RET(int, ioctl(fd, WDIOC_KEEPALIVE, 0));
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETTIMEOUT)
		{
			int timeout;

			VOID_RET(int, ioctl(fd, WDIOC_GETTIMEOUT, &timeout));
		}
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETPRETIMEOUT)
		{
			int timeout;

			VOID_RET(int, ioctl(fd, WDIOC_GETPRETIMEOUT, &timeout));
		}
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETPRETIMEOUT)
		{
			int timeout;

			VOID_RET(int, ioctl(fd, WDIOC_GETTIMELEFT, &timeout));
		}
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETSUPPORT)
		{
			struct watchdog_info ident;

			VOID_RET(int, ioctl(fd, WDIOC_GETSUPPORT, &ident));
		}
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETSTATUS)
		{
			int flags;

			VOID_RET(int, ioctl(fd, WDIOC_GETSTATUS, &flags));
		}
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETBOOTSTATUS)
		{
			int flags;

			VOID_RET(int, ioctl(fd, WDIOC_GETBOOTSTATUS, &flags));
		}
#else
		UNEXPECTED
#endif

#if defined(WDIOC_GETTEMP)
		{
			int temperature;

			VOID_RET(int, ioctl(fd, WDIOC_GETBOOTSTATUS, &temperature));
		}
#else
		UNEXPECTED
#endif
		stress_watchdog_magic_close();
		ret = close(fd);
		fd = -1;
		if (ret < 0) {
			pr_fail("%s: cannot close %s, errno=%d (%s)\n",
				args->name, dev_watchdog, errno, strerror(errno));
			rc = EXIT_FAILURE;
			break;
		}
		(void)shim_sched_yield();
		inc_counter(args);
	}

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	return rc;
}

stressor_info_t stress_watchdog_info = {
	.stressor = stress_watchdog,
	.class = CLASS_VM | CLASS_OS | CLASS_PATHOLOGICAL,
	.help = help
};
#else
stressor_info_t stress_watchdog_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_VM | CLASS_OS | CLASS_PATHOLOGICAL,
	.help = help,
	.unimplemented_reason = "built without linux/watchdog.h"
};
#endif

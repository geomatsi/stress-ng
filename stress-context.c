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

#if defined(HAVE_UCONTEXT_H)
#include <ucontext.h>
#endif

#define STRESS_CONTEXTS		(3)

static stress_help_t help[] = {
	{ NULL,	"context N",	 "start N workers exercising user context" },
	{ NULL,	"context-ops N", "stop context workers after N bogo operations" },
	{ NULL,	NULL,		 NULL }
};

#if defined(HAVE_SWAPCONTEXT) &&	\
    defined(HAVE_UCONTEXT_H)

#define CONTEXT_STACK_SIZE	(16384)

typedef struct {
	uint32_t check0;	/* memory clobbering check canary */
	ucontext_t uctx;	/* swapcontext context */
	uint32_t check1;	/* memory clobbering check canary */
} chk_ucontext_t;

typedef struct {
	uint32_t check0;	/* copy of original check1 canary */
	uint32_t check1;	/* copy of original check1 canary */
} chk_canary_t;

typedef struct {
	chk_ucontext_t	cu;	/* check ucontext */
	uint8_t		ALIGN64 stack[CONTEXT_STACK_SIZE + STACK_ALIGNMENT]; /* stack */
	chk_canary_t	canary;	/* copy of canary */
} context_data_t;

#define CONTEXT_SIZE	(sizeof(context_data_t))
#define ALIGNED_SIZE 	((CONTEXT_SIZE + 63) & ~(size_t)63)

typedef struct {
	context_data_t	d;
	uint8_t padding[ALIGNED_SIZE - CONTEXT_SIZE];
} context_info_t;

static ucontext_t uctx_main;
static context_info_t *context;
static uint64_t context_counter;
static uint64_t stress_max_ops;
double duration, t1, t2, t3;

static void stress_thread1(void)
{
	do {
		duration += stress_time_now() - t3;
		context_counter++;
		t1 = stress_time_now();
		(void)swapcontext(&context[0].d.cu.uctx, &context[1].d.cu.uctx);
	} while (keep_stressing_flag() && (!stress_max_ops || (context_counter < stress_max_ops)));
	(void)swapcontext(&context[0].d.cu.uctx, &uctx_main);
}

static void stress_thread2(void)
{
	do {
		duration += stress_time_now() - t1;
		context_counter++;
		t2 = stress_time_now();
		(void)swapcontext(&context[1].d.cu.uctx, &context[2].d.cu.uctx);
	} while (keep_stressing_flag() && (!stress_max_ops || (context_counter < stress_max_ops)));
	(void)swapcontext(&context[1].d.cu.uctx, &uctx_main);
}

static void stress_thread3(void)
{
	do {
		duration += stress_time_now() - t2;
		context_counter++;
		t3 = stress_time_now();
		(void)swapcontext(&context[2].d.cu.uctx, &context[0].d.cu.uctx);
	} while (keep_stressing_flag() && (!stress_max_ops || (context_counter < stress_max_ops)));
	(void)swapcontext(&context[2].d.cu.uctx, &uctx_main);
}

static void (*stress_threads[STRESS_CONTEXTS])(void) = {
	stress_thread1,
	stress_thread2,
	stress_thread3,
};

static int stress_context_init(
	const stress_args_t *args,
	void (*func)(void),
	ucontext_t *uctx_link,
	context_info_t *context_info)
{
	(void)memset(context_info, 0, sizeof(*context_info));

	if (getcontext(&context_info->d.cu.uctx) < 0) {
		pr_fail("%s: getcontext failed: %d (%s)\n",
			args->name, errno, strerror(errno));
		return -1;
	}

	context_info->d.canary.check0 = stress_mwc32();
	context_info->d.canary.check1 = stress_mwc32();

	context_info->d.cu.check0 = context_info->d.canary.check0;
	context_info->d.cu.check1 = context_info->d.canary.check1;
	context_info->d.cu.uctx.uc_stack.ss_sp =
		(void *)stress_align_address(context_info->d.stack, STACK_ALIGNMENT);
	context_info->d.cu.uctx.uc_stack.ss_size = CONTEXT_STACK_SIZE;
	context_info->d.cu.uctx.uc_link = uctx_link;
	makecontext(&context_info->d.cu.uctx, func, 0);

	return 0;
}

/*
 *  stress_context()
 *	stress that exercises CPU context save/restore
 */
static int stress_context(const stress_args_t *args)
{
	size_t i;
	double rate;
	int rc = EXIT_FAILURE;

	context = (context_info_t *)mmap(NULL, STRESS_CONTEXTS * sizeof(*context),
					PROT_READ | PROT_WRITE,
					MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (context == MAP_FAILED) {
		pr_inf("%s: failed to allocate %d x %zd byte context buffers, skipping stressor\n",
			args->name, STRESS_CONTEXTS, sizeof(context_info_t));
		return EXIT_NO_RESOURCE;
	}

	(void)memset(&uctx_main, 0, sizeof(uctx_main));

	context_counter = 0;
	stress_max_ops = args->max_ops * 1000;

	/* Create 3 micro threads */
	for (i = 0; i < STRESS_CONTEXTS; i++) {
		if (stress_context_init(args, stress_threads[i], &uctx_main, &context[i]) < 0)
			goto fail;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);
	duration = 0.0;
	t1 = 0.0;
	t2 = 0.0;
	t3 = stress_time_now();
	/* And start.. */
	if (swapcontext(&uctx_main, &context[0].d.cu.uctx) < 0) {
		pr_fail("%s: swapcontext failed: %d (%s)\n",
			args->name, errno, strerror(errno));
		goto fail;
	}

	set_counter(args, context_counter / 1000);

	for (i = 0; i < STRESS_CONTEXTS; i++) {
		if (context[i].d.canary.check0 != context[i].d.cu.check0) {
			pr_fail("%s: swapcontext clobbered data before context region\n",
				args->name);
		}
		if (context[i].d.canary.check1 != context[i].d.cu.check1) {
			pr_fail("%s: swapcontext clobbered data after context region\n",
				args->name);
		}
	}
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	rate = (duration > 0.0) ? (double)context_counter / duration : 0.0;
	stress_metrics_set(args, 0, "swapcontext calls per sec", rate);
	rc = EXIT_SUCCESS;

fail:
	(void)munmap((void *)context, STRESS_CONTEXTS * sizeof(*context));

	return rc;
}

stressor_info_t stress_context_info = {
	.stressor = stress_context,
	.class = CLASS_MEMORY | CLASS_CPU,
	.verify = VERIFY_ALWAYS,
	.help = help
};
#else
stressor_info_t stress_context_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_MEMORY | CLASS_CPU,
	.help = help,
	.unimplemented_reason = "built without ucontext.h"
};
#endif

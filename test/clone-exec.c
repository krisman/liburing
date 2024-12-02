#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <sched.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "liburing.h"
#include "helpers.h"
char **_argv;
char **_envp;

int test_fail_sequence(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	int ret;

	ret = t_create_ring(10, &ring, IORING_SETUP_SUBMIT_ALL);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		return T_SETUP_SKIP;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_clone(sqe);
	sqe->flags |= IOSQE_IO_LINK;

	/* Add a command that will fail. */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);
	sqe->nop_flags = IORING_NOP_INJECT_RESULT;

	/*
	 * A random magic number to be retrieved in cqe->res.  Not a
	 * valid errno returned by io_uring.
	 */
	sqe->len = -255;
	sqe->flags |= IOSQE_IO_LINK;

	/* And a NOP that will suceed */
	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_nop(sqe);

	io_uring_submit(&ring);

 	if (io_uring_wait_cqes(&ring, &cqe, 3, NULL, NULL)) {
 		fprintf(stderr, "%s: Failed to wait for cqes\n", __func__);
 		return T_EXIT_FAIL;
 	}

	if (cqe->res) {
 		fprintf(stderr, "%s: failed to clone. Got %d\n",
			__func__, cqe->res);
 		return T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&ring, cqe);

	io_uring_peek_cqe(&ring, &cqe);
	if (cqe->res != -255) {
 		fprintf(stderr, "%s: This nop should have failed with 255. Got %d\n",
			__func__, cqe->res);
 		return T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&ring, cqe);

	io_uring_peek_cqe(&ring, &cqe);
	if (cqe->res != -ECANCELED) {
 		fprintf(stderr, "%s: This should have been -ECANCELED. Got %d\n",
			__func__, cqe->res);
 		return T_EXIT_FAIL;
	}
	io_uring_cqe_seen(&ring, cqe);

	return 0;
}

#define MAX_PATH 1024

int test_spawn_sequence(void)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	unsigned int head;
	int ret, i, reaped = 0;
	char *buf;

	char *const t_argv[] = { "echo","Hello World",  NULL };
	char *const t_envp[] = { NULL };

	char *path[]= { "/usr/local/bin/", "/usr/local/sbin/", "/usr/bin/", "/usr/sbin/", "/sbin", "/bin" };

	ret = t_create_ring(10, &ring, IORING_SETUP_SUBMIT_ALL);
	if (ret < 0) {

		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		return T_SETUP_SKIP;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_clone(sqe);
	/* allocate from heap to simplify freeing. */
	sqe->user_data = (__u64) strdup("clone");
	sqe->flags |= IOSQE_IO_LINK;

	for (i = 0; i < ARRAY_SIZE(path); i++ ) {
		buf = malloc(MAX_PATH);
		if (!buf)
			return -ENOMEM;
		snprintf(buf, MAX_PATH, "%s/%s", path[i], "echo");

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_execveat(sqe, AT_FDCWD, buf, t_argv, t_envp, 0);
		sqe->user_data = (__u64) buf;
		io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK|IOSQE_IO_HARDLINK);
	}

	io_uring_submit_and_wait(&ring, i+ 1);

	io_uring_for_each_cqe(&ring, head, cqe) {
		reaped++;
		free((char*)cqe->user_data);
	}
	io_uring_cq_advance(&ring, reaped);

	return 0;
}

int main(int argc, char *argv[], char *envp[])
{
	if (test_fail_sequence()) {
		fprintf(stderr, "test_failed_sequence failed\n");
		return T_EXIT_FAIL;
	}

	if (test_spawn_sequence()) {
		fprintf(stderr, "test_failed_sequence failed\n");
		return T_EXIT_FAIL;
	}

	while (true);

	return 0;
}

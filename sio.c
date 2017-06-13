/* For O_DIRECT */
#define _GNU_SOURCE

/* A simple io psync test program. */

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BASE_FN "tmpfile"
#define FILE_SZ (2ULL*1024*1024*1024)
#define FILE_CHUNK 8192


char *content;
static int exit_all;

struct thread_arg {
	char fn[64];
	unsigned long long szall;
	int idx;
};

void
create_file(void *arg)
{
	int fd, i, ret, sz;
	struct stat st;
	struct thread_arg *targ = (struct thread_arg *) arg;
	char *fn = targ->fn;

	printf("  Creating file %s if needed\n", fn);
	if (stat(fn, &st) == 0 && st.st_size == FILE_SZ) {
		/* fine. no need to create a new file. */
		return;
	} else {
		unlink(fn);
		
		fd = open(fn, O_CREAT | O_TRUNC | O_WRONLY | O_DIRECT, 0600);
		if (fd < 0) {
			fprintf(stderr, "can not create file %s\n", fn);
			exit(1);
		}

		for (i = 0; i < FILE_SZ/FILE_CHUNK; i++) {
			sz = FILE_CHUNK;

			while (sz != 0) {
				ret = write(fd, content+FILE_CHUNK-sz, sz);

				if (ret > 0) {
					sz -= ret;
				} else if (ret < 0 && errno != EINTR) {
					fprintf(stderr, "creating file failed (%d)on this thread: %d\n", errno, targ->idx);
					exit(1);
				}
			}
		}

		close(fd);
	}
}

void
read_file(void *arg)
{
	int fd, i, ret, sz;
	char buf[FILE_CHUNK];
	struct thread_arg *targ = (struct thread_arg *) arg;
	char *fn = targ->fn;

	printf("  Reading file %s\n", fn);
	fd = open(fn, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "can not read file %s\n", fn);
		exit(1);
	}

	targ->szall = 0;
	for (i = 0; i < FILE_SZ/FILE_CHUNK; i++) {
		sz = FILE_CHUNK;

		while (sz > 0) {
			ret = read(fd, buf+FILE_CHUNK-sz, sz);

			if (ret > 0) {
				sz -= ret;
			} else if (ret < 0 && errno != EINTR) {
				fprintf(stderr, "reading file failed (%d) on this thread: %d\n", errno, targ->idx);
				exit(1);
			}

			/* exit early */
			if (exit_all) {
				targ->szall += FILE_CHUNK - sz;
				return;
			}
		}

		targ->szall += FILE_CHUNK;
	}

	exit_all = 1;

	close(fd);
}

int
main(int argc, char *argv[])
{
	int i, nthread, ret;
	pthread_t *thread_id;
	struct thread_arg *targ;

	/* I'm lazy to free all */

	if (argc != 2) {
		fprintf(stderr, "usage: myio $threads\n");
		exit(1);
	}
	printf("You better clean up the cache (os cache, h/w cache) before running.\n");

	nthread = atoi(argv[1]);

	/* init file content */
    posix_memalign((void**)&content, getpagesize(), FILE_CHUNK);
	for (i = 0; i < FILE_CHUNK; i++) {
		content[i] = i % 256;
	}

	/* create per-thread variable */
	thread_id = malloc(nthread * sizeof(pthread_t));
	if (thread_id == NULL) {
		fprintf(stderr, "fail to allocate %d of thread_t\n", nthread);
		exit(1);
	}

	targ = malloc(nthread * sizeof(struct thread_arg));
	if (targ == NULL) {
		fprintf(stderr, "fail to allocate %d of thread_arg\n", nthread);
		exit(1);
	}

	/* write file */
	printf("Creating files...\n");

	for (i = 0; i < nthread; i++) {
		sprintf(targ[i].fn, "%s_%d", BASE_FN, i);
		targ[i].szall = 0;
		targ[i].idx = i;

		ret = pthread_create(&thread_id[i], NULL,  (void *) create_file, &targ[i]);
	}

	for (i = 0; i < nthread; i++) {
		pthread_join(thread_id[i], NULL);
	}

	/* test file */
	printf("Reading files...\n");

	struct timespec start, end;

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < nthread; i++) {
		sprintf(targ[i].fn, "%s_%d", BASE_FN, i);
		targ[i].szall = 0;
		targ[i].idx = i;

		ret = pthread_create(&thread_id[i], NULL,  (void *) read_file, &targ[i]);
	}

	for (i = 0; i < nthread; i++) {
		pthread_join(thread_id[i], NULL);
	}

	clock_gettime(CLOCK_MONOTONIC, &end);

	unsigned long long all = 0;
	for (i = 0; i < nthread; i++) {
		all += targ[i].szall;
		printf("For thread %d, sz=%lld\n", i, targ[i].szall);
	}
	printf("all size: %lld\n", all);


	double t = (end.tv_sec - start.tv_sec) + ((end.tv_nsec - start.tv_nsec)/1000.0)/1000.0/1000.0;
	printf("performance: %.3f MB/s. time: %.3fs. size: %.3fMB\n", all/1024.0/1024.0/t, t, all/1024.0/1024.0);

	return 0;
}

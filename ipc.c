#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/shm.h>

static int is_verbose;
static int is_server;
enum IPC_TYPE ipc_type;

enum IPC_TYPE {
	IPC_NONE,	/* none */
	IPC_UDS,	/* unix domain socket */
	IPC_SEM,	/* semaphore */
};

static int loop_cnt = 100000;

#define debug_print(...) if (is_verbose) fprintf(stderr, __VA_ARGS__)
#define debug_abort(...)  {fprintf(stderr, __VA_ARGS__); fflush(stderr); exit(1); }

struct timespec
gettimespec(void)
{
	struct timespec ts;
	int status;

	status = clock_gettime(CLOCK_MONOTONIC, &ts);
	if (status != 0)
		debug_abort("clock_gettime() does not work with return values: %d, errno; %d", __func__, status, errno);

	return ts;
}

static void *
shmset(size_t bytes, char *fn, int proj_id, int *id)
{
    key_t key;
    int shmid;
    void *p;

    if ((key = ftok(fn, proj_id)) < 0) {
        debug_abort("ftok fails with errno %d\n", errno);
        return NULL;
    }

    if ((shmid = shmget(key, bytes, 0666|IPC_CREAT|IPC_EXCL)) < 0) {
        if (errno == EEXIST) {
            shmid = shmget(key, bytes, 0666);
            p = shmat(shmid, NULL, 0);
        } else {
            debug_abort("shmget fails (bytes: %d, fn: %s, proj_id: %d), errnor:%d\n", (int) bytes, fn, proj_id, errno);
            p = NULL;
        }
    } else {
        debug_print("shm create done by pid: %d\n", getpid());
        p = shmat(shmid, NULL, 0);
        /* sem is at the beginning of the buffer. */
        if (sem_init(p, 1, 0) != 0)
            debug_abort("sem_init() fails with errno %d", errno);
        debug_print("sem_init OK at addr %p", p);
    }

    *id = shmid;

    debug_print("Use shm id %d, addr %p, pid: %d\n", *id, p, getpid());

    return p;
}

void
sem_server_handler()
{
	sem_t *p_tx, *p_rx;
	int id_tx, id_rx, i;

	unlink("/tmp/tmp.sem.server.tx");
	unlink("/tmp/tmp.sem.client.tx");
	creat("/tmp/tmp.sem.server.tx", 0666);
	creat("/tmp/tmp.sem.client.tx", 0666);
	p_tx = shmset(sizeof(sem_t), "/tmp/tmp.sem.server.tx", 't', &id_tx);
	p_rx = shmset(sizeof(sem_t), "/tmp/tmp.sem.client.tx", 't', &id_rx);

	for (i = 0; i < loop_cnt; i++) {
		sem_post(p_tx);
		sem_wait(p_rx);
	}

	sleep(5);
	shmdt(p_tx);
	shmdt(p_rx);
	shmctl(id_tx, IPC_RMID, NULL);
	shmctl(id_rx, IPC_RMID, NULL);
}

void
sem_client_handler()
{
	sem_t *p_tx, *p_rx;
	int id_tx, id_rx, i;
	struct timespec t1, t2;

	p_rx = shmset(sizeof(sem_t), "/tmp/tmp.sem.server.tx", 't', &id_rx);
	p_tx = shmset(sizeof(sem_t), "/tmp/tmp.sem.client.tx", 't', &id_tx);

	t1 = gettimespec();
	for (i = 0; i < loop_cnt; i++) {
		sem_wait(p_rx);
		sem_post(p_tx);
	}
	t2 = gettimespec();
	fprintf(stderr, "Time spent: %.5fs\n", (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec)/1000.0/1000.0/1000.0);

	sleep(5);
	shmdt(p_tx);
	shmdt(p_rx);
	shmctl(id_tx, IPC_RMID, NULL);
	shmctl(id_rx, IPC_RMID, NULL);
}

void
uds_server_handler()
{
	int fd, cnt;
	int cli_fd;
	struct sockaddr_un un;
	char c;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		debug_abort("socket() fails with errno %d", errno);

	/* accept. */
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strncpy(un.sun_path, "/tmp/tmp.uds", sizeof(un.sun_path) - 1);
	unlink("/tmp/tmp.uds");

	if (bind(fd, (struct sockaddr *) &un, sizeof(un)) < 0)
		debug_abort("bind() fails with errno %d\n", errno);

	if (listen(fd, 100) < 0)
		debug_abort("listen() fails with errno %d\n", errno);

	if ((cli_fd = accept(fd, NULL, NULL)) == -1)
		debug_abort("accept() fails with errno %d\n", errno);

	/* Start testing */
	cnt = 0;
	while (cnt < loop_cnt) {
		while (send(cli_fd, &c, 1, 0) < 0 && errno == EINTR);
		while (recv(cli_fd, &c, 1, 0) < 0 && errno == EINTR);
		cnt++;
	}
}

void
uds_client_handler()
{
	int fd, cnt;
	struct sockaddr_un un;
	char c;
	struct timespec t1, t2;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		debug_abort("socket() fails with errno %d", errno);

	/* Now connect to the server at first. */
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, "/tmp/tmp.uds", sizeof(un.sun_path) - 1);

    if (connect(fd, (struct sockaddr *) &un, sizeof(un)) < 0)
		debug_abort("connect() fails with errno %d\n", errno);

	t1 = gettimespec();
	/* Start testing */
	cnt = 0;
	while (cnt < loop_cnt) {
		while (recv(fd, &c, 1, 0) < 0 && errno == EINTR);
		while (send(fd, &c, 1, 0) < 0 && errno == EINTR);
		cnt++;
	}
	t2 = gettimespec();
	fprintf(stderr, "Time spent: %.5fs\n", (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec)/1000.0/1000.0/1000.0);
}


static void
show_usage()
{
	fprintf(stderr, "Usage: [-v] -s/-c -t type [-n test_cnt]\n");
	fprintf(stderr, "	-v: verbose\n");
	fprintf(stderr, "	-s: server\n");
	fprintf(stderr, "	-c: client\n");
	fprintf(stderr, "	-n: test loop count\n");
	fprintf(stderr, "	-t: type (u: unix domain socket, s: semaphore)\n");
}


int
main(int argc, char *argv[])
{
	int is_client, opt;

    while((opt = getopt( argc, argv, "vscn:t:h")) != -1) {
        switch( opt ) {
            case 'v':
                is_verbose = 1;
                break;

            case 's':
                is_server = 1;
                break;

            case 'c':
                is_client = 1;
                break;

            case 'n':
                loop_cnt = atoi(optarg);
                break;

            case 't':
                if (strlen(optarg) != 1 || (optarg[0] != 'u' && optarg[0] != 's'))
					debug_abort("Unknown type: '%s'\n", optarg);
				if (optarg[0] == 'u')
					ipc_type = IPC_UDS;	
				if (optarg[0] == 's')
					ipc_type = IPC_SEM;	
				break;

            case 'h':
            case '?':
                show_usage();
				exit(1);
                break;

            default:
                /* You won't actually get here. */
                break;
        }
    }

	if (is_server+is_client != 1)
		debug_abort("Must set either server or client.\n");

	if (ipc_type == IPC_NONE)
		debug_abort("Must set an ipc type.\n");

	if (ipc_type == IPC_SEM) {
		printf("Test semaphore\n");
		if (is_server) {
			sem_server_handler();
		} else {
			sem_client_handler();
		}
	} else if (ipc_type == IPC_UDS) {
		printf("Test UDS\n");
		if (is_server) {
			uds_server_handler();
		} else {
			uds_client_handler();
		}
	}

	return 0;
}

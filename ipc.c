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

enum IPC_TYPE {
	IPC_NONE,	/* none */
	IPC_UDS,	/* unix domain socket */
	IPC_SEM,	/* semaphore */
};

#define DEFAULT_LOOP_CNT 100000
static int loop_cnt = DEFAULT_LOOP_CNT;

#define debug_print(...) do { if (is_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
#define debug_abort(...)  do {fprintf(stderr, __VA_ARGS__); fflush(stderr); exit(1); } while (0)

typedef void (*handler)(void);

struct ipc_test {
	enum IPC_TYPE ipc_type;
	char *desc;
	handler server_handler;
	handler client_handler;
};

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

#define SEM_S_TX_FILE "/tmp/tmp.sem.server.tx"
#define SEM_C_TX_FILE "/tmp/tmp.sem.client.tx"
void
sem_server_handler()
{
	sem_t *p_tx, *p_rx;
	int id_tx, id_rx, i;

	unlink(SEM_S_TX_FILE);
	if (creat(SEM_S_TX_FILE, 0666) < 0)
		debug_abort("cannot create file %s with errno %d\n", SEM_S_TX_FILE, errno);
	unlink(SEM_C_TX_FILE);
	if (creat(SEM_C_TX_FILE, 0666) < 0)
		debug_abort("cannot create file %s with errno %d\n", SEM_C_TX_FILE, errno);
	p_tx = shmset(sizeof(sem_t), SEM_S_TX_FILE, 't', &id_tx);
	p_rx = shmset(sizeof(sem_t), SEM_C_TX_FILE, 't', &id_rx);

	for (i = 0; i < loop_cnt; i++) {
		sem_post(p_tx);
		sem_wait(p_rx);
	}

	sleep(3);
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

	p_rx = shmset(sizeof(sem_t), SEM_S_TX_FILE, 't', &id_rx);
	p_tx = shmset(sizeof(sem_t), SEM_C_TX_FILE, 't', &id_tx);

	t1 = gettimespec();
	for (i = 0; i < loop_cnt; i++) {
		sem_wait(p_rx);
		sem_post(p_tx);
	}
	t2 = gettimespec();
	fprintf(stderr, "Time spent: %.5fs\n", (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec)/1000.0/1000.0/1000.0);

	sleep(3);
	shmdt(p_tx);
	shmdt(p_rx);
	shmctl(id_tx, IPC_RMID, NULL);
	shmctl(id_rx, IPC_RMID, NULL);
}

#define UDS_FILE "/tmp/tmp.uds"

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
	strncpy(un.sun_path, UDS_FILE, sizeof(un.sun_path) - 1);
	unlink(UDS_FILE);

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
    strncpy(un.sun_path, UDS_FILE, sizeof(un.sun_path) - 1);

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
	fprintf(stderr, "Usage: [-v] -t type [-p] [-e time] [-n test_loop_cnt] [-h]\n");
	fprintf(stderr, "	-v: verbose printing\n");
	fprintf(stderr, "	-t type: (u: unix domain socket, s: semaphore)\n");
	fprintf(stderr, "	-p: Use process instead (Use pthread by default)\n");
	fprintf(stderr, "	-e seconds: sleep times after test is done. People might want to check process stat data.\n");
	fprintf(stderr, "	-n count: Test loop count. %d by default\n", DEFAULT_LOOP_CNT);
	fprintf(stderr, "	-h: Show usage.\n");
}

/* Compile it with: -lpthread -lrt */
int
main(int argc, char *argv[])
{
	int opt, retval;
	struct ipc_test test;
	int use_process = 0;
	enum IPC_TYPE ipc_type = IPC_NONE;
	int sleep_time = 1;

	use_process = 1;
    while((opt = getopt( argc, argv, "vpe:n:t:h")) != -1) {
        switch( opt ) {
            case 'v':
                is_verbose = 1;
                break;

            case 't':
                if (strlen(optarg) != 1 || (optarg[0] != 'u' && optarg[0] != 's'))
					debug_abort("Unknown type: '%s'\n", optarg);
				if (optarg[0] == 'u')
					ipc_type = IPC_UDS;	
				if (optarg[0] == 's')
					ipc_type = IPC_SEM;	
				break;

            case 'p':
                use_process = 1;
                break;

            case 'e':
				/* For testing so no sanity-checking here. */
                sleep_time = atoi(optarg);
				if (sleep_time < 0)
					sleep_time = 1;
                break;

            case 'n':
                loop_cnt = atoi(optarg);
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

	test.ipc_type = ipc_type;
	switch (ipc_type) {
		case IPC_SEM:
			test.desc = "semaphore";
			test.server_handler = sem_server_handler;
			test.client_handler = sem_client_handler;
			break;
		case IPC_UDS:
			test.desc = "unix domain socket";
			test.server_handler = uds_server_handler;
			test.client_handler = uds_client_handler;
			break;
		default:
			debug_abort("Must set an ipc type.\n");
	}

	if (use_process) {

		fprintf(stderr, "Test '%s' with loop count %d with processes\n", test.desc, loop_cnt);

		retval = fork();

		if (retval < 0) {
			debug_abort("fork() fails with errno %d\n", errno);
		} else if (retval > 0) {
			/* parent */
			test.server_handler();
		} else {
			/* child. Leave some time to server for prepartion. */
			sleep(5);
			test.client_handler();
		}
	} else {
		fprintf(stderr, "Test '%s' with loop count %d with processes\n", test.desc, loop_cnt);

		pthread_t thd;

		/* no shm creation for pthread is needed however I'm lazy to modify. */
		retval = pthread_create(&thd, NULL, test.server_handler);
		if (retval)
			debug_abort("pthread_create() fails with errno %d\n", errno);
		retval = pthread_create(&thd, NULL, test.client_handler);
		if (retval)
			debug_abort("pthread_create() fails with errno %d\n", errno);
	}

	sleep(sleep_time);

	return 0;
}
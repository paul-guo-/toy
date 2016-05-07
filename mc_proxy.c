#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <linux/tcp.h>

#define gettid() syscall(__NR_gettid)
#define DEFAULT_MC_HOST "10.150.110.177"
#define DEFAULT_MC_PORT 11211
#define DEFAULT_NUM_CHILD 4
#define DEFAULT_NUM_DOWN_CONN 4
#define MAX_NUM_DOWN_CONN 16

static char *mc_host = DEFAULT_MC_HOST;
static int up_mc_port = DEFAULT_MC_PORT;
static int down_mc_port = DEFAULT_MC_PORT;
static int num_down_conn = DEFAULT_NUM_DOWN_CONN;

/* toy memcached proxy. Need more work: better data strcture, logging, performance, fault tolerance, state machine cleanup, etc. */
/* TODO: epoll_ctl could add the conn as the callback pointer. */

#if 0
#define log_state(uc) fprintf(stderr, "called on %s(): state %d, fd: %d %d\n", __func__, uc->state, uc->up_fd, uc->down_fd);
#define log_write(...) fprintf(stderr, __VA_ARGS__)
#else
#define log_state(uc)
#define log_write(...)
#endif

enum conn_states {
	conn_listening,
	conn_ready,
	conn_read_upstream_started,
	conn_read_upstream_done,
	conn_write_downstream_started,
	conn_read_downstream_started,
	conn_read_downstream_done,
	conn_write_upstream_started,
	conn_die,
};

struct conn {
	enum conn_states state;
	struct iovec up_buf;
	int up_tot_len;
	int up_left_len;
	struct iovec down_buf;
	int down_tot_len;
	int down_left_len;
	int up_fd;
	int down_fd;
	int down_fd_idx;
	int up_epoll_in_set;
	int down_epoll_in_set;
};

static int epfd;

struct wait_downstream_conns {
	struct conn *conn;
	struct wait_downstream_conns *prev;
	struct wait_downstream_conns *next;
};

static struct wait_downstream_conns wait_down_list_head;
static struct wait_downstream_conns wait_down_list_tail;

/* should have been done via hash. Setting as 1024 for testing so far. */
/* per up_fd */
static struct conn conns_upstream[1024];
/* per down_fd */
static struct conn *conns_for_downstram[1024];

static int down_fds[MAX_NUM_DOWN_CONN];
static char down_used[MAX_NUM_DOWN_CONN];

static int listen_fd;

static struct conn *new_uc_for_downstream_tx;

/* Benifit: Not tested. Probably not necessary to set for all sockfd. */
int
set_sock_others(int sockfd)
{
	int error;
	int flags =1;
	struct linger ling = {0, 0};

	error = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
	if (error != 0) perror("setsockopt SO_REUSEADDR");

	error = setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &flags, sizeof(flags));
	if (error != 0) perror("setsockopt SO_REUSEPORT");

	error = setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
	if (error != 0) perror("setsockopt SO_KEEPALIVE");

	error = setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
	if (error != 0) perror("setsockopt SO_LINGER");

	error = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
	if (error != 0) perror("setsockopt TCP_NODELAY");
}

int
set_sock_nonblocking(int sockfd)
{
	int ret;

	ret = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFD, 0)|O_NONBLOCK);

	return ret;
}

int
conn_downstream()
{
	int sockfd;
	struct sockaddr_in servaddr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "socket() failure: %s\n", strerror(errno));
		exit(1);
	}

	memset(&servaddr, 0, sizeof(struct sockaddr_in));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(down_mc_port);
	if(inet_pton(AF_INET, mc_host, &servaddr.sin_addr) <= 0) {
		fprintf(stderr, "inet_pton error for %s\n", mc_host);
		exit(1);
	}

	if (set_sock_nonblocking(sockfd) == -1) {
		fprintf(stderr, "fcntl socket error: %s(errno: %d)", strerror(errno), errno);
		exit(1);
	}

	if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
		fprintf(stderr, "connect error: %s(errno: %d)\n",strerror(errno),errno);
		exit(1);
	}

	if (set_sock_others(sockfd) == -1) {
		fprintf(stderr, "fcntl socket error: %s(errno: %d)", strerror(errno), errno);
		exit(1);
	}

	return sockfd;
}

int
listen_upstream()
{
	int fd;
	struct sockaddr_in servaddr;

	if((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1 ) {
		fprintf(stderr, "create socket error: %s(errno: %d)\n", strerror(errno), errno);
		exit(1);
	}

	if (set_sock_nonblocking(fd) == -1) {
		fprintf(stderr, "fcntl socket error: %s(errno: %d)", strerror(errno), errno);
		exit(1);
	}

	if (set_sock_others(fd) == -1) {
		fprintf(stderr, "fcntl socket error: %s(errno: %d)", strerror(errno), errno);
		exit(1);
	}

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(up_mc_port);

	if( bind(fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) == -1) {
		fprintf(stderr, "bind socket error: %s(errno: %d)\n", strerror(errno), errno);
		exit(1);
	}

	if( listen(fd, 1024) == -1) {
		fprintf(stderr, "listen socket error: %s(errno: %d)\n", strerror(errno), errno);
		exit(1);
	}

	return fd;
}

int
is_fd_downstream(int fd)
{
	int i;

	for (i = 0; i < num_down_conn; i++) {
		if (fd == down_fds[i])
			return 1;
	}

	return 0;
}

int
is_fd_listen(int fd)
{
	if (fd == listen_fd)
		return 1;

	return 0;
}

void
init_iov(struct iovec *iv)
{
	iv->iov_base = malloc(2048);
	iv->iov_len = 0;
}

void
reset_conn(struct conn *uc)
{
	uc->state = conn_ready;
	uc->up_buf.iov_len = 0;
	uc->up_tot_len = 0;
	uc->up_left_len = 0;
	uc->down_buf.iov_len = 0;
	uc->down_tot_len = 0;
	uc->down_left_len = 0;
	uc->down_fd = -1;
	uc->down_fd_idx = -1;
}

void
init_conn(struct conn *uc)
{
	uc->state = conn_ready;
	init_iov(&uc->up_buf);
	uc->up_tot_len = 0;
	uc->up_left_len = 0;
	init_iov(&uc->down_buf);
	uc->down_tot_len = 0;
	uc->down_left_len = 0;
	uc->up_fd = -1;
	uc->down_fd = -1;
	uc->down_fd_idx = -1;
}

int
drive_machine_listen(int fd)
{
	int connfd;
	struct conn *uc;
	struct epoll_event ev;

	if( (connfd = accept(fd, (struct sockaddr*)NULL, NULL)) == -1){
		/* thundering herd is still possible. todo: refer the nginx code, i.e. lock protection. */
		fprintf(stderr, "accept socket error: %s(errno: %d)\n", strerror(errno),errno);
		return 0;
	}

	if( connfd >= 1024) {
		fprintf(stderr, "For test. connfd (%d) should < 1024.", connfd);
		exit(1);
	}

	if (set_sock_nonblocking(connfd) == -1) {
		fprintf(stderr, "fcntl socket error: %s(errno: %d)", strerror(errno), errno);
		exit(1);
	}

	if (set_sock_others(connfd) == -1) {
		fprintf(stderr, "fcntl socket error: %s(errno: %d)", strerror(errno), errno);
		exit(1);
	}

	uc = &conns_upstream[connfd];
	init_conn(uc);
	uc->up_fd = connfd;

	ev.data.fd = connfd;
	ev.events = EPOLLIN | EPOLLET;
	epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev);

	printf("accepted a new connection, fd: %d on process tid: %d\n", connfd, gettid());
	return 0;
}

int
assign_downstream()
{
	int i;

	for (i = 0; i < num_down_conn; i++) {
		if (down_used[i] == 0) {
			down_used[i] = 1;
			return i;
		}
	}

	return -1;
}

/* better solution: refer kernel list functions. */

void conn_list_add_tail(struct wait_downstream_conns *tail, struct conn *uc)
{
	struct wait_downstream_conns *node = calloc(1, sizeof(struct wait_downstream_conns));

	node->conn = uc;

	tail->prev->next = node;
	node->prev = tail->prev;

	node->next = tail;
	tail->prev = node;

}

struct conn*
conn_list_remove(struct wait_downstream_conns *head, struct wait_downstream_conns *tail)
{
	struct wait_downstream_conns *node;
	struct conn *uc;

	if (head->next == tail)
		return NULL;
	
	node = head->next;

	head->next = node->next;
	node->next->prev = head;

	uc = node->conn;
	free(node);

	return uc;
}

int
downstream_tx_later(struct conn *uc) {
	struct epoll_event ev;
	if (uc->down_epoll_in_set == 0) {
		log_write("enable epoll write on down_fd: %d", uc->down_fd);
		ev.data.fd = uc->down_fd;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		epoll_ctl(epfd, EPOLL_CTL_MOD, uc->down_fd, &ev);
		uc->down_epoll_in_set = 1;
	}
}

/* 0: state machine loop done. 1: call state machine code again. */
int
downstream_tx(struct conn *uc) {
	int res;
	struct epoll_event ev;
//debug
static int down_epoll_set;

	/* no iov yet. Correctly it later. */
	while (1) {
		res = send(uc->down_fd, uc->up_buf.iov_base, uc->up_left_len, 0);
		if (res > 0) {
			uc->up_left_len -= res;
			if (uc->up_left_len == 0) {
				uc->state = conn_read_downstream_started;
				/* Do not epoll the tx event on the fd. */
				log_write("disable epoll write on %s(): fd: %d if necessary. left tx length: %d\n", __func__, uc->down_fd, uc->up_left_len);
				if (uc->down_epoll_in_set) {
					log_write("disable epoll write on fd: %d\n", uc->down_fd);
					ev.data.fd = uc->down_fd;
					ev.events = EPOLLIN | EPOLLET;
					epoll_ctl(epfd, EPOLL_CTL_MOD, uc->down_fd, &ev);
					uc->down_epoll_in_set = 0;
				}
				return 0;
			}
		} else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			fprintf(stderr, "downstream write again/blocked: %s.\n", strerror(errno));
			break;
		} else {
			fprintf(stderr, "write error (res: %d, errno: %d, %s) in %s(). left bytes: %d\n", res, errno, strerror(errno), __func__, uc->up_left_len);
			uc->state = conn_die;
			return 1;
		}
	}

	/* trigger tx event if necessary. */
	log_write("enable epoll write on %s(): down_fd: %d if necessary. left tx length: %d", __func__, uc->down_fd, uc->up_left_len);
	if (uc->down_epoll_in_set == 0) {
down_epoll_set++; if (!(down_epoll_set%100)) printf("down_epoll_set:%d\n", down_epoll_set);
		log_write("enable epoll write on down_fd: %d", uc->down_fd);
		ev.data.fd = uc->down_fd;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		epoll_ctl(epfd, EPOLL_CTL_MOD, uc->down_fd, &ev);
		uc->down_epoll_in_set = 1;
	}

	return 0;
}

/* 0: state machine loop done. 1: call state machine code again. */
int
upstream_tx(struct conn *uc) {
	int res;
	struct epoll_event ev;
static int up_epoll_set;

	log_write("	upstream tx, fd:%d, bytes:%d.\n", uc->up_fd, uc->down_left_len);

	while (1) {
		res = send(uc->up_fd, uc->down_buf.iov_base, uc->down_left_len, 0);
		if (res > 0) {
			uc->down_left_len -= res;
			if (uc->down_left_len == 0) {
				reset_conn(uc);
				uc->state = conn_ready;
				/* Do not epoll the tx event on the fd. */
				log_write("disable epoll write on %s(): up_fd: %d if necessary. left tx length: %d\n", __func__, uc->up_fd, uc->down_left_len);
				if (uc->up_epoll_in_set) {
					log_write("disable epoll write on up_fd: %d", uc->up_fd);
					ev.data.fd = uc->up_fd;
					ev.events = EPOLLIN | EPOLLET;
					epoll_ctl(epfd, EPOLL_CTL_MOD, uc->up_fd, &ev);
					uc->up_epoll_in_set = 0;
				}
				return 0;
			}
		} else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		} else {
			fprintf(stderr, "wrong send (res: %d, errno: %d, %s) in %s.\n", res, errno, strerror(errno), __func__);
			uc->state = conn_die;
			return 1;
		}
	}

	log_write("enable epoll write on %s(): up_fd: %d if necessary. left rx length: %d\n", __func__, uc->up_fd, uc->down_left_len);
	if (uc->up_epoll_in_set == 0) {
up_epoll_set++; if (!(up_epoll_set%100)) printf("up_epoll_set:%d\n", up_epoll_set);
		log_write("enable epoll write on up_fd: %d", uc->up_fd);
		ev.data.fd = uc->up_fd;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		epoll_ctl(epfd, EPOLL_CTL_MOD, uc->up_fd, &ev);
		uc->up_epoll_in_set = 1;
	}

	return 0;
}

/* 0: rx done. */
int
parse_upstream_rx(struct iovec *buf) {
	char *str = buf->iov_base;

	if (buf->iov_len <= 1)
		return 1;

	str[buf->iov_len] = 0;
	//log_write("%s(), buf: %s\n", __func__, str);
	//log_write("0x%x 0x%x\n", str[buf->iov_len-2], str[buf->iov_len-1]);
	if (strncmp(str+buf->iov_len-1, "\n", 1) == 0) {
		log_write("%s(): return 0\n", __func__);
		return 0;
	}

	return 1;
}

/* 0: rx done. */
int
parse_downstream_rx(struct iovec *buf) {
	char *str = buf->iov_base;

	/* only for get. */
	if (buf->iov_len <= 5)
		return 1;

	str[buf->iov_len] = 0;
	log_write("%s, buf: %s\n", __func__, str);
	if (strncmp(str+buf->iov_len-5, "END\r\n", 5) == 0 ||
	    strncmp(str+buf->iov_len-8, "STORED\r\n", 8) == 0)
		return 0;

	return 1;
}

/* 0: state machine loop done. 1: call state machine code again. */
int
drive_machine_upstream_rx(struct conn *uc)
{
	ssize_t res;
	int idx, ret;
//paul
struct epoll_event ev;

	log_state(uc);

	switch (uc->state) {
	case conn_listening:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;

	case conn_ready:
		uc->state = conn_read_upstream_started;
		return 1;

	case conn_read_upstream_started:
		while (1) {
			ret = 0;
			res = recv(uc->up_fd, uc->up_buf.iov_base + uc->up_buf.iov_len, 2048 - uc->up_buf.iov_len, 0);
			if (res > 0){
				uc->up_buf.iov_len += res;
				uc->up_tot_len = uc->up_buf.iov_len; /* not used yet. */
				uc->up_left_len = uc->up_buf.iov_len;
				log_write ("upstream rx. fd:%d, bytes:%d\n", uc->up_fd, uc->up_left_len);
				/* no need to parse it multiple times. */
				if (parse_upstream_rx(&uc->up_buf) == 0) {
					uc->state = conn_read_upstream_done;
					return 1;
				}
			} else if (res == 0) {
				/* connection is closed. */
				log_write("connection is closed.\n");
				//close(uc->up_fd);
				uc->state = conn_die;
				return 1;
			} else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				return 0;
			} else {
				/* wrong! No code to handle it yet. exiting. */
				fprintf(stderr, "wrong recv (res: %d, errno: %d, %s) in %s()\n", res, errno, strerror(errno), __func__);
				uc->state = conn_die;
				return 1;
			}
		}
		return ret;

	case conn_read_upstream_done:
		uc->up_tot_len = uc->up_buf.iov_len; /* not used yet. */
		uc->up_left_len = uc->up_buf.iov_len;
		idx = assign_downstream(uc);
		if (idx >= 0) {
			uc->down_fd_idx = idx;
			uc->down_fd = down_fds[idx];
			log_write("assign_downstream fd: %d\n", idx, uc->down_fd);
			conns_for_downstram[uc->down_fd] = uc;
			uc->state = conn_write_downstream_started;
#if 1
			ret = downstream_tx(uc);
#else
			ret = downstream_tx_later(uc);
#endif
		} else {
			/* put it on wait_down_list_head for later handling. */
			log_write("no downstream idx for fd: %d.\n", uc->up_fd);
			conn_list_add_tail(&wait_down_list_tail, uc);
			uc->state = conn_write_downstream_started;
			ret = 0;
		}
		return ret;

	case conn_write_downstream_started:
	case conn_read_downstream_started:
	case conn_read_downstream_done:
	case conn_write_upstream_started:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;
	case conn_die:
		fprintf(stderr, "add code for conn_die.\n");
		return 0;
	default:
		fprintf(stderr, "undefined state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;
	}
}

int
drive_machine_downstream_tx(struct conn *uc)
{
	ssize_t res;
	struct conn *new_conn;
	int idx, ret;

	log_state(uc);

	switch (uc->state) {
	case conn_listening:
	case conn_ready:
	case conn_read_upstream_started:
	case conn_read_upstream_done:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;

	case conn_write_downstream_started:
		ret = downstream_tx(uc);
		return ret;

	case conn_read_downstream_started:
	case conn_read_downstream_done:
	case conn_write_upstream_started:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;

	case conn_die:
		fprintf(stderr, "add code for conn_die.\n");
		return 0;
	default:
		fprintf(stderr, "undefined state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;
	}
}

int
drive_machine_downstream_rx(struct conn *uc)
{
	ssize_t res;
	struct conn *new_uc;
	struct epoll_event ev;
	int idx;
	
	log_state(uc);

	switch (uc->state) {
	case conn_listening:
	case conn_ready:
	case conn_read_upstream_started:
	case conn_read_upstream_done:
	case conn_write_downstream_started:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;

	case conn_read_downstream_started:
		while (1) {
			res = recv(uc->down_fd, uc->down_buf.iov_base + uc->down_buf.iov_len, 2048 - uc->down_buf.iov_len, 0);
			if (res > 0){
				uc->down_buf.iov_len += res;
				uc->down_left_len = uc->down_buf.iov_len;
				/* fixme: no need to parse it multiple times. */
				if (parse_downstream_rx(&uc->down_buf) == 0) {
					uc->state = conn_read_downstream_done;
					return 1;
				}
			} else if (res == 0) {
				/* connection is closed. */
				uc->state = conn_die;
				return 1;
			} else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				return 0;
			} else {
				/* wrong! No code to handle it yet. exiting. */
				fprintf(stderr, "wrong recv (res: %d, errno: %d, %s) in %s()\n", res, errno, strerror(errno), __func__);
				//close(uc->down_fd);
				uc->state = conn_die;
				return 1;
			}
		}
		return 0;

	case conn_read_downstream_done:
		idx = uc->down_fd_idx;
		down_used[idx] = 0;

		/* assign the down_fd to a new conn */
		new_uc = conn_list_remove(&wait_down_list_head, &wait_down_list_tail);
		if (new_uc != NULL) {
			new_uc->down_fd = down_fds[idx];
			new_uc->down_fd_idx = idx;
			down_used[idx] = 1;
			conns_for_downstram[new_uc->down_fd] = new_uc;
//haha
#if 0
			ev.data.fd = new_uc->down_fd;
			ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
			epoll_ctl(epfd, EPOLL_CTL_MOD, new_uc->down_fd, &ev);
			new_uc->down_epoll_in_set = 1;
#endif
		}
		new_uc_for_downstream_tx = new_uc;

		uc->state = conn_write_upstream_started;
		return upstream_tx(uc);

	case conn_write_upstream_started:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;

	case conn_die:
		fprintf(stderr, "add code for conn_die.\n");
		return 0;
	default:
		fprintf(stderr, "undefined state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;
	}
}

int
drive_machine_upstream_tx(struct conn *uc)
{
	ssize_t res;
	struct conn *new_conn;
	int idx;
	
	log_state(uc);

	switch (uc->state) {
	case conn_listening:
	case conn_ready:
	case conn_read_upstream_started:
	case conn_read_upstream_done:
	case conn_write_downstream_started:
	case conn_read_downstream_started:
	case conn_read_downstream_done:
		fprintf(stderr, "wrong state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;

	case conn_write_upstream_started:
		return upstream_tx(uc);

	case conn_die:
		fprintf(stderr, "add code for conn_die.\n");
		return 0;
	default:
		fprintf(stderr, "undefined state (%d) in %s(fd: %d %d)\n", uc->state, __func__, uc->up_fd, uc->down_fd);
		return 0;
	}
}

int
drive_machine()
{
	struct epoll_event events[1024];
	struct conn *uc;
	int nfds, i, fd, ret;

	memset(events, 0, sizeof(events));

	while (1) {
		nfds = epoll_wait(epfd, events, 1024, -1);

		for (i = 0; i < nfds; i++) {
			fd = events[i].data.fd;

			if (is_fd_listen(fd)) {
				log_write("epoll to handle: listen fd.\n");
				drive_machine_listen(fd);
			} else if (is_fd_downstream(fd)) {
				log_write("epoll to handle: downstream fd: %d.\n", fd);
				if (fd >= 1024) {
					fprintf(stderr, "down fd (%d) >= 1024 in %s.\n", fd, __func__);
					exit(1);
				}
				uc = conns_for_downstram[fd];

				if (events[i].events & EPOLLIN)
					while (drive_machine_downstream_rx(uc));
				if (events[i].events & EPOLLOUT)
					while (drive_machine_downstream_tx(uc));

				/* Do downstream tx for a new connection if necessary. */
				if (new_uc_for_downstream_tx) {
					while (drive_machine_downstream_tx(new_uc_for_downstream_tx));
					new_uc_for_downstream_tx = NULL;
				}
			} else {
				log_write("epoll to handle: upstream fd: %d.\n", fd);
				if (fd >= 1024) {
					fprintf(stderr, "up fd (%d) >= 1024 in %s.\n", fd, __func__);
					exit(1);
				}
				uc = &conns_upstream[fd];

				if (events[i].events & EPOLLIN)
					while (drive_machine_upstream_rx(uc));
				if (events[i].events & EPOLLOUT)
					while (drive_machine_upstream_tx(uc));
			}
		}
	}
}

int
fork_children(int num_child)
{
	int is_child = 0;
	int status;

	if (num_child > 0) {
		while (!is_child) {
			if (num_child > 0){
				switch(fork()){
				case -1:
					return -1;
				case 0:
					is_child = 1;
					break;
				default:
					num_child--;
					break;
				}
			} else {
				if (wait(&status) != -1)
					num_child++;
			}
		}
	}

	return 0;
}

void
main(int argc, char *argv[])
{
	int i, sockfd, opt;
	struct epoll_event ev;
	int num_child = DEFAULT_NUM_CHILD;
	const char *opt_str = "c:n:p:q:s:h";

	opt = getopt(argc, argv, opt_str);
	while(opt != -1) {
		/*No sanity check for all yet! */
		switch(opt) {
		case 'c':
			num_down_conn = atoi(optarg);
			if (num_down_conn > MAX_NUM_DOWN_CONN) {
				fprintf(stderr, "max downstream connections (%d) should be <= %d\n", num_down_conn, MAX_NUM_DOWN_CONN);
				fprintf(stderr, "Forcing to %d\n", MAX_NUM_DOWN_CONN);
				num_down_conn = MAX_NUM_DOWN_CONN;
			}
			break;

		case 'n':
			num_child = atoi(optarg);
			break;

		case 'p':
			up_mc_port = atoi(optarg);
			break;

		case 'q':
			down_mc_port = atoi(optarg);
			break;


		case 's':
			mc_host = strdup(optarg);
			assert(mc_host != NULL);
			break;

		case 'h':
		case '?':
			fprintf(stderr, "\t-c: max downstream connections\n\t-s: server\n\t-p: upstream port\n\t-q: downstream port\n\t-n: worker number\n\t-h: help\n");
			exit(1);

		default:
			/* Never reach here. */
			break;
		}

		opt = getopt(argc, argv, opt_str);
	}
	printf("Configurations:\n");
	printf("  downstream connection per worker: %d\n", num_down_conn);
	printf("  server: %s\n", mc_host);
	printf("  upstream port: %d\n", up_mc_port);
	printf("  downstream port: %d\n", down_mc_port);
	printf("  worker numbers: %d\n", num_child);

	listen_fd = listen_upstream();

	/* fork 4 children. */
	fork_children(num_child);

	/* set >1 epfd? */
	epfd = epoll_create1(0);
	ev.data.fd = listen_fd;
	ev.events = EPOLLIN;
	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	wait_down_list_head.prev = NULL;
	wait_down_list_head.next = &wait_down_list_tail;
	wait_down_list_tail.prev = &wait_down_list_head;
	wait_down_list_tail.next = NULL;

	for (i = 0; i < num_down_conn; i++) {
		down_fds[i] = conn_downstream();
		ev.data.fd = down_fds[i];
		ev.events = EPOLLIN | EPOLLET;
		epoll_ctl(epfd, EPOLL_CTL_ADD, down_fds[i], &ev);
	}

	drive_machine();
}

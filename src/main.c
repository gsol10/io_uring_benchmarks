#include "stdio.h"
//#include <linux/io_uring.h>
#include "liburing.h"
#include <stdlib.h>


#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <string.h>
#include <strings.h>
#include <unistd.h>

#define RECV_BUF_SIZE 4096

//Returns the file descriptor of the socket
int setup() {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	
	struct sockaddr_in laddr;

	bzero(&laddr, sizeof(struct sockaddr_in));

	laddr.sin_family    = AF_INET;
    laddr.sin_addr.s_addr = INADDR_ANY;
    laddr.sin_port = htons(10000);
      
    // Bind the socket with the server address
	int r = 0;
    if (r = bind(fd, (const struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
		char *error = strerror(r);
        printf("Error binding socket : %s\n", error); 
        return -1;
    }

	return fd;
}

static int setup_context(unsigned entries, struct io_uring *ring)
{
	int ret;

	ret = io_uring_queue_init(entries, ring, 0);
	if (ret < 0) {
		fprintf(stderr, "queue_init: %s\n", strerror(-ret));
		return -1;
	}

	return 0;
}

struct io_data {
	int read;
	off_t first_offset, offset;
	size_t first_len;
	struct iovec iov;
};

static int queue_read(int fd, struct io_uring *ring)
{
	int offset = 0;
	int size = RECV_BUF_SIZE;
	struct io_uring_sqe *sqe;
	struct io_data *data;

	data = malloc(RECV_BUF_SIZE + sizeof(*data));
	if (!data)
		return 1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		free(data);
		return 1;
	}

	data->read = 1;
	data->offset = data->first_offset = offset;

	data->iov.iov_base = data + 1;
	data->iov.iov_len = size;
	data->first_len = size;

	io_uring_prep_readv(sqe, fd, &data->iov, 1, offset);
	io_uring_sqe_set_data(sqe, data);
	io_uring_submit(ring);
	return 0;
}

int queue_sendmsg(int fd, struct io_uring *ring, struct msghdr *msg) {
	int offset = 0;
	int size = RECV_BUF_SIZE;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		return 1;
	}

	io_uring_prep_sendmsg(sqe, fd, msg, 0);
	io_uring_sqe_set_data(sqe, msg);
	io_uring_submit(ring);
	return 0;
}

static void queue_recvmsg(int fd, struct io_uring *ring, struct msghdr *msg) {
	struct io_uring_sqe *sqe;
	sqe = io_uring_get_sqe(ring);
	//printf("Sqe is %p\n", sqe);

	io_uring_prep_recvmsg(sqe, fd, msg, 0);
	io_uring_sqe_set_data(sqe, msg);
	//printf("Sent %d sqes\n", io_uring_submit(ring));
}

struct msghdr *alloc_msg() {
	struct msghdr *msg = malloc(sizeof(struct msghdr));
	struct sockaddr *s_addr = malloc(sizeof(struct sockaddr));
	struct iovec *data = malloc(sizeof(struct iovec) + RECV_BUF_SIZE);

	data->iov_base = data + 1;
	data->iov_len = RECV_BUF_SIZE;

	msg->msg_name = s_addr;
	msg->msg_namelen = sizeof(s_addr);
	msg->msg_iov = data;
	msg->msg_iovlen = 1;

	return msg;
}

int tests_io_uring(int fd) {
	struct io_uring ring;
	if (setup_context(64, &ring))
		return 1;

	struct msghdr *msg = alloc_msg();
	queue_recvmsg(fd, &ring, msg);
	//queue_read(fd, &ring);
	while (1) {
		struct io_uring_cqe *cqe;
		int ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret == 0) {
			//printf("It worked !\n");
			struct msghdr *msg = io_uring_cqe_get_data(cqe);
			//printf("Data is at %p, res is %d\n", msg, (cqe->res));

			msg->msg_iov->iov_len = cqe->res;
			
			//data[10] = '\x00';
			//printf("Data = %s\n", (char *) msg->msg_iov->iov_base);

			queue_sendmsg(fd, &ring, msg);
			io_uring_wait_cqe(&ring, &cqe);
			msg->msg_iov->iov_len = RECV_BUF_SIZE;
			queue_recvmsg(fd, &ring, msg);
		}
	}

	io_uring_queue_exit(&ring);
	
}

int main() {
	printf("Socket/io_uring test\n\n");
	// struct io_uring_params params;
	// io_uring_setup(32 , &params);
	printf("Setting up socket\n");
	
	int fd = setup();

	printf("Starting test\n");

	tests_io_uring(fd);

	// char buf[RECV_BUF_SIZE];
	// int r = recv(fd, buf, RECV_BUF_SIZE - 1, 0);
	// printf("Received : %d\n", r);

	// printf("Listening on socket. Receiving with vanilla recvfrom.\n");
	// char buf[RECV_BUF_SIZE];
	// struct sockaddr src_addr; socklen_t addrlen = sizeof(struct sockaddr);
	// int n = 0;
	// while (n = recvfrom(fd, buf, RECV_BUF_SIZE - 1, 0, &src_addr, &addrlen)) {
	// 	buf[n] = '\x00';
	// 	printf("Received message : %s\n", buf);

	// 	addrlen = sizeof(struct sockaddr);
	// }
	close(fd);

	return 0;
}

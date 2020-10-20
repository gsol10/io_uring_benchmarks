#include "stdio.h"
//#include <linux/io_uring.h>
#include "liburing.h"
#include "pci.h"
#include <stdlib.h>


#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>

#include <string.h>
#include <strings.h>
#include <unistd.h>

#define RECV_BUF_SIZE 2048

//https://stackoverflow.com/a/1941331
#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...) printf(fmt, ##args)
#else
#define DEBUG_PRINT(fmt, args...)
#endif

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

int setup_packet(int ifindex) {
	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
	
	struct sockaddr_ll laddr;

	bzero(&laddr, sizeof(struct sockaddr_in));

	laddr.sll_family    = AF_PACKET;
    laddr.sll_protocol = htons(ETH_P_IP);
    laddr.sll_ifindex =  ifindex;
      
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

struct msg_sent {
	int read;
	int ind;
};

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
	DEBUG_PRINT("Sqe is %p\n", sqe);

	io_uring_prep_recvmsg(sqe, fd, msg, 0);
	io_uring_sqe_set_data(sqe, msg);
	int n = io_uring_submit(ring);
	DEBUG_PRINT("Sent %d sqes\n", n);
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
			DEBUG_PRINT("It worked !\n");
			struct msghdr *msg = io_uring_cqe_get_data(cqe);
			DEBUG_PRINT("Data is at %p, res is %d\n", msg, (cqe->res));

			msg->msg_iov->iov_len = cqe->res;
			
			//data[10] = '\x00';
			DEBUG_PRINT("Data = %s\n", (char *) msg->msg_iov->iov_base);

			queue_sendmsg(fd, &ring, msg);
			io_uring_wait_cqe(&ring, &cqe);
			msg->msg_iov->iov_len = RECV_BUF_SIZE;
			queue_recvmsg(fd, &ring, msg);
		}
	}

	io_uring_queue_exit(&ring);
	
}

int echo_io_uring(int fd) {
	struct io_uring ring;
	int req_size = 64;

	if (setup_context(req_size, &ring))
		return 1;

	struct iovec iov[req_size];
	struct msg_sent info[req_size];
	for (int i = 0; i < req_size; i++) {
		iov[i].iov_base = malloc(RECV_BUF_SIZE);
		iov[i].iov_len = RECV_BUF_SIZE;
	}

	io_uring_register_buffers(&ring, iov, req_size);

	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&ring);
	int32_t ind= 0;
	io_uring_prep_read_fixed(sqe, fd, iov[ind].iov_base, RECV_BUF_SIZE, 0, ind);
	info[ind].ind = ind;
	info[ind].read = 1;
	io_uring_sqe_set_data(sqe, &info[ind]);

	io_uring_submit(&ring);
	printf("Reading\n");
	//queue_read(fd, &ring);
	while (1) {
		struct io_uring_cqe *cqe;
		//int ret = io_uring_wait_cqe(&ring, &cqe);
		int ret = io_uring_peek_cqe(&ring, &cqe);
		DEBUG_PRINT("Read ret is %d\n", ret);
		if (ret > 0) {
			DEBUG_PRINT("It worked !\n");
			struct msg_sent *inf = (struct msg_sent *) io_uring_cqe_get_data(cqe);
			DEBUG_PRINT("Buffer index is %d, read is %d, res is %d\n", inf->ind, inf->read, (cqe->res));
			ind = inf->ind;
			if (inf->read == 1) {
				io_uring_cqe_seen(&ring, cqe);
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_write_fixed(sqe, fd, iov[ind].iov_base, ret, 0, ind);
				inf->read = 0;
				io_uring_sqe_set_data(sqe, &info[ind]);
				io_uring_submit(&ring);
			} else {
				io_uring_cqe_seen(&ring, cqe);
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_read_fixed(sqe, fd, iov[ind].iov_base, RECV_BUF_SIZE, 0, ind);
				inf->read = 1;
				io_uring_sqe_set_data(sqe, &info[ind]);
				io_uring_submit(&ring);
			}
		}
	}

	io_uring_queue_exit(&ring);
	
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		printf("Correct use : echo <pci_addr>\n");
		return -1;
	}


	printf("Socket/io_uring test\n\n");
	// struct io_uring_params params;
	// io_uring_setup(32 , &params);

	int n = get_ifindex_of_pic(argv[1]);
	if (n == -1) {
		printf("Network interface corresponding to PCI addr %s not found\n", argv[1]);
		return -1;
	}
	printf("Binding to interface %d correponding to PCI %s\n", n, argv[1]);
	printf("Setting up socket\n");

	int fd = setup_packet(n);

	echo_io_uring(fd);

	// char buf[RECV_BUF_SIZE];
	// int r = read(fd, buf, RECV_BUF_SIZE - 1);
	// printf("Received : %d\n", r);
	// for (int i = 0; i < 6; i++) {
	// 	printf("%hhx", buf[i]);
	// }

	// int fd = setup();

	// printf("Starting test\n");

	// tests_io_uring(fd);

	// char buf[RECV_BUF_SIZE];
	// int r = recv(fd, buf, RECV_BUF_SIZE - 1, 0);
	// DEBUG_PRINT("Received : %d\n", r);

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

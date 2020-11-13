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

 #include <poll.h>

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

int setup_packet(int ifindex) {
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_IP));

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

enum {
	EVENT_POLL,
	EVENT_READ,
	EVENT_WRITE
};

struct msg_sent {
	int op_type;
	int ind;
	int interface; //1 or 2 depending on the fd
};

int echo_io_uring(int fd1, int fd2) {
	struct io_uring ring;
	int req_size = 8;

	int fds[2] = {fd1, fd2};

	if (setup_context(req_size, &ring))
		return 1;

	struct iovec iov[req_size];
	struct msg_sent info[req_size];
	for (int i = 0; i < req_size; i++) {
		iov[i].iov_base = malloc(RECV_BUF_SIZE);
		iov[i].iov_len = RECV_BUF_SIZE;
	}

	struct io_uring_sqe *sqe;

	//We fill the half of the sqes with read requests for both interfaces
	for (int32_t i = 0; i < 4; i+=2) {
		int interface = (i % 4)/2;
		int fd = fds[interface];

		int32_t ind = i;
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_poll_add(sqe, fd, POLLIN);
		sqe->flags |= IOSQE_IO_LINK; //Link so that next submission is executed just after this one
		info[ind].ind = ind;
		info[ind].op_type = EVENT_POLL;
		info[ind].interface = interface;
		io_uring_sqe_set_data(sqe, &info[ind]);


		ind = i + 1;
		sqe = io_uring_get_sqe(&ring);
		sqe->flags = 0;
		io_uring_prep_readv(sqe, fd, &iov[ind], 1, 0);
		info[ind].ind = ind;
		info[ind].op_type = EVENT_READ;
		info[ind].interface = interface;
		io_uring_sqe_set_data(sqe, &info[ind]);
	}

	io_uring_submit(&ring);
	printf("Reading\n");
	//queue_read(fd, &ring);
	struct io_uring_cqe **cqes = malloc(sizeof(struct io_uring_cqe *) * req_size);
	
	while (1) {
		int nb_req = io_uring_peek_batch_cqe(&ring, cqes, req_size);

		DEBUG_PRINT("CQE read is %d\n", ret);
		for (int i = 0; i < nb_req; i++) {
			struct io_uring_cqe *cqe = cqes[i];
			struct msg_sent *inf = (struct msg_sent *) io_uring_cqe_get_data(cqe);
			int32_t interface = inf->interface;
			int ind = inf->ind;
			int type = inf->op_type;
			int fd = fds[1 - interface]; //Opposite fd
			int ret = cqe->res;

			io_uring_cqe_seen(&ring, cqe);

			DEBUG_PRINT("Buffer index is %d, op_type is %d, res is %d\n", ind, type, ret);
			switch (type)
			{
			case EVENT_POLL:
				break;
			case EVENT_READ:
				ind -= 1; //We know the poll was already seen so the previous info is free
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_poll_add(sqe, fd, POLLOUT); //Check when fd is ready to write
				sqe->flags |= IOSQE_IO_LINK; //Link so that next submission is executed just after this one
				info[ind].ind = ind;
				info[ind].op_type = EVENT_POLL;
				info[ind].interface = interface;
				io_uring_sqe_set_data(sqe, &info[ind]);

				ind+=1;
				sqe = io_uring_get_sqe(&ring);
				sqe->flags = 0;
				iov[ind].iov_len = ret;
				io_uring_prep_writev(sqe, fd, &iov[ind], 1, 0);
				info[ind].ind = ind;
				info[ind].op_type = EVENT_WRITE;
				info[ind].interface = interface;
				io_uring_sqe_set_data(sqe, &info[ind]);

				break;			
			case EVENT_WRITE:
				ind -= 1; //We know the poll was already seen so the previous info is free
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_poll_add(sqe, fd, POLLIN); //Check when fd is ready to read
				sqe->flags |= IOSQE_IO_LINK; //Link so that next submission is executed just after this one
				info[ind].ind = ind;
				info[ind].op_type = EVENT_POLL;
				info[ind].interface = interface;
				io_uring_sqe_set_data(sqe, &info[ind]);

				ind+=1;
				sqe = io_uring_get_sqe(&ring);
				sqe->flags = 0;
				iov[ind].iov_len = RECV_BUF_SIZE;
				io_uring_prep_writev(sqe, fd, &iov[ind], 1, 0);
				info[ind].ind = ind;
				info[ind].op_type = EVENT_READ;
				info[ind].interface = interface;
				io_uring_sqe_set_data(sqe, &info[ind]);
				break;
			}
		}
		io_uring_submit(&ring);
	}
	free(cqes);

	io_uring_queue_exit(&ring);
	
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		printf("Correct use : echo <pci_addr1> <pci_addr2>\n");
		return -1;
	}


	printf("Socket/io_uring test\n\n");
	// struct io_uring_params params;
	// io_uring_setup(32 , &params);

	int n1 = get_ifindex_of_pic(argv[1]);
	if (n1 == -1) {
		printf("Network interface corresponding to PCI addr %s not found\n", argv[1]);
		return -1;
	}
	int n2 = get_ifindex_of_pic(argv[2]);
	if (n2 == -1) {
		printf("Network interface corresponding to PCI addr %s not found\n", argv[2]);
		return -1;
	}
	printf("Binding to interfaces %d, %d correponding to PCIs %s and %s\n", n1, n2, argv[1], argv[2]);
	printf("Setting up socket\n");

	int fd1 = setup_packet(n1);
	int fd2 = setup_packet(n2);

	echo_io_uring(fd1, fd2);

	close(fd1);
	close(fd2);

	return 0;
}

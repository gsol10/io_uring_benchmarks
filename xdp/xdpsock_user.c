// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2018 Intel Corporation. 
	This code is taken from the Linux 5.4 kernel samples*/

#include <asm/barrier.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/bpf.h>
#include <linux/compiler.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <locale.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <bpf/bpf.h>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#define NUM_FRAMES (4 * 1024)
#define BATCH_SIZE 64

#define DEBUG_HEXDUMP 0
#define MAX_SOCKS 8

typedef __u64 u64;
typedef __u32 u32;

static unsigned long prev_time;

enum benchmark_type {
	BENCH_RXDROP = 0,
	BENCH_TXONLY = 1,
	BENCH_L2FWD = 2,
};

static char *ifname1;
static char *ifname2;
static int ifindex1;
static int ifindex2;
static int qid1;
static int qid2;

static enum benchmark_type opt_bench = BENCH_L2FWD;
static u32 opt_xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST;
static int opt_poll;
static int opt_interval = 1;
static u32 opt_xdp_bind_flags = XDP_USE_NEED_WAKEUP;
static u32 opt_umem_flags;
static int opt_unaligned_chunks;
static int opt_mmap_flags;
static u32 opt_xdp_bind_flags;
static int opt_xsk_frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
static int opt_timeout = 1000;
static bool opt_need_wakeup = true;
static __u32 prog_id;

struct xsk_umem_info {
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsk_umem *umem;
	void *buffer;
};

struct xsk_socket_info {
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
	struct xsk_umem_info *umem;
	struct xsk_socket *xsk;
	unsigned long rx_npkts;
	unsigned long tx_npkts;
	unsigned long prev_rx_npkts;
	unsigned long prev_tx_npkts;
	u32 outstanding_tx;
};

static int num_socks;
struct xsk_socket_info *xsks[MAX_SOCKS];

static unsigned long get_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static void print_benchmark(bool running)
{
	const char *bench_str = "INVALID";

	if (opt_bench == BENCH_RXDROP)
		bench_str = "rxdrop";
	else if (opt_bench == BENCH_TXONLY)
		bench_str = "txonly";
	else if (opt_bench == BENCH_L2FWD)
		bench_str = "l2fwd";

	printf("%s:%d %s ", ifname1, qid1, bench_str);
	if (opt_xdp_flags & XDP_FLAGS_SKB_MODE)
		printf("xdp-skb ");
	else if (opt_xdp_flags & XDP_FLAGS_DRV_MODE)
		printf("xdp-drv ");
	else
		printf("	");

	if (opt_poll)
		printf("poll() ");

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static void dump_stats(void)
{
	unsigned long now = get_nsecs();
	long dt = now - prev_time;
	int i;

	prev_time = now;

	for (i = 0; i < num_socks && xsks[i]; i++) {
		char *fmt = "%-15s %'-11.0f %'-11lu\n";
		double rx_pps, tx_pps;

		rx_pps = (xsks[i]->rx_npkts - xsks[i]->prev_rx_npkts) *
			 1000000000. / dt;
		tx_pps = (xsks[i]->tx_npkts - xsks[i]->prev_tx_npkts) *
			 1000000000. / dt;

		printf("\n sock%d@", i);
		print_benchmark(false);
		printf("\n");

		printf("%-15s %-11s %-11s %-11.2f\n", "", "pps", "pkts",
		       dt / 1000000000.);
		printf(fmt, "rx", rx_pps, xsks[i]->rx_npkts);
		printf(fmt, "tx", tx_pps, xsks[i]->tx_npkts);

		xsks[i]->prev_rx_npkts = xsks[i]->rx_npkts;
		xsks[i]->prev_tx_npkts = xsks[i]->tx_npkts;
	}
}

static void *poller(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(opt_interval);
		dump_stats();
	}

	return NULL;
}

static void remove_xdp_program(int ifindex)
{
	__u32 curr_prog_id = 0;

	if (bpf_get_link_xdp_id(ifindex, &curr_prog_id, opt_xdp_flags)) {
		printf("bpf_get_link_xdp_id failed\n");
		exit(EXIT_FAILURE);
	}
	if (prog_id == curr_prog_id)
		bpf_set_link_xdp_fd(ifindex, -1, opt_xdp_flags);
	else if (!curr_prog_id)
		printf("couldn't find a prog id on a given interface\n");
	else
		printf("program on interface changed, not removing\n");
}

static void int_exit(int sig)
{
	struct xsk_umem *umem = xsks[0]->umem->umem;

	(void)sig;

	dump_stats();
	xsk_socket__delete(xsks[0]->xsk);
	(void)xsk_umem__delete(umem);
	remove_xdp_program(ifindex1);
	remove_xdp_program(ifindex2);

	exit(EXIT_SUCCESS);
}

static void __exit_with_error(int error, const char *file, const char *func,
			      int line)
{
	fprintf(stderr, "%s:%s:%i: errno: %d/\"%s\"\n", file, func,
		line, error, strerror(error));
	dump_stats();
	remove_xdp_program(ifindex1);
	remove_xdp_program(ifindex2);
	exit(EXIT_FAILURE);
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, \
						 __LINE__)

static void swap_mac_addresses(void *data)
{
	struct ether_header *eth = (struct ether_header *)data;
	struct ether_addr *src_addr = (struct ether_addr *)&eth->ether_shost;
	struct ether_addr *dst_addr = (struct ether_addr *)&eth->ether_dhost;
	struct ether_addr tmp;

	tmp = *src_addr;
	*src_addr = *dst_addr;
	*dst_addr = tmp;
}

static void hex_dump(void *pkt, size_t length, u64 addr)
{
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;
	char buf[32];
	int i = 0;

	if (!DEBUG_HEXDUMP)
		return;

	sprintf(buf, "addr=%llu", addr);
	printf("length = %zu\n", length);
	printf("%s | ", buf);
	while (length-- > 0) {
		printf("%02X ", *address++);
		if (!(++i % line_size) || (length == 0 && i % line_size)) {
			if (length == 0) {
				while (i++ % line_size)
					printf("__ ");
			}
			printf(" | ");	/* right close */
			while (line < address) {
				c = *line++;
				printf("%c", (c < 33 || c == 255) ? 0x2E : c);
			}
			printf("\n");
			if (length > 0)
				printf("%s | ", buf);
		}
	}
	printf("\n");
}

static struct xsk_umem_info *xsk_configure_umem(void *buffer, u64 size)
{
	struct xsk_umem_info *umem;
	struct xsk_umem_config cfg = {
		.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2, //This is needed because the fq can be update after the rx thus causing a deadlock
		.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
		.frame_size = opt_xsk_frame_size,
		.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
		.flags = opt_umem_flags
	};

	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		exit_with_error(errno);

	ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq,
			       &cfg);

	if (ret)
		exit_with_error(-ret);

	umem->buffer = buffer;
	return umem;
}

static struct xsk_socket_info *xsk_configure_socket(struct xsk_umem_info *umem, char *ifname, int ifid, int queue_id, bool first)
{
	struct xsk_socket_config cfg;
	struct xsk_socket_info *xsk;
	int ret;
	u32 idx;
	int i;
	u32 offset = first ? 0 : XSK_RING_PROD__DEFAULT_NUM_DESCS;

	xsk = calloc(1, sizeof(*xsk));
	if (!xsk)
		exit_with_error(errno);

	xsk->umem = umem;
	cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	cfg.libbpf_flags = 0;
	cfg.xdp_flags = opt_xdp_flags;
	cfg.bind_flags = opt_xdp_bind_flags;
	ret = xsk_socket__create(&xsk->xsk, ifname, queue_id, umem->umem,
				 &xsk->rx, &xsk->tx, &cfg);
	if (ret)
		exit_with_error(-ret);

	ret = bpf_get_link_xdp_id(ifid, &prog_id, opt_xdp_flags);
	if (ret)
		exit_with_error(-ret);

	ret = xsk_ring_prod__reserve(&xsk->umem->fq,
				     XSK_RING_PROD__DEFAULT_NUM_DESCS,
				     &idx);
	if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS)
		exit_with_error(-ret);
	for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++)
		*xsk_ring_prod__fill_addr(&xsk->umem->fq, idx++) =
			(i + offset) * opt_xsk_frame_size;
	xsk_ring_prod__submit(&xsk->umem->fq,
			      XSK_RING_PROD__DEFAULT_NUM_DESCS);

	return xsk;
}

static struct option long_options[] = {
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"poll", no_argument, 0, 'p'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"interval", required_argument, 0, 'n'},
	{"zero-copy", no_argument, 0, 'z'},
	{"copy", no_argument, 0, 'c'},
	{"frame-size", required_argument, 0, 'f'},
	{"no-need-wakeup", no_argument, 0, 'm'},
	{"unaligned", no_argument, 0, 'u'},
	{0, 0, 0, 0}
};

static void usage(const char *prog)
{
	const char *str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -r, --rxdrop		Discard all incoming packets (default)\n"
		"  -t, --txonly		Only send packets\n"
		"  -l, --l2fwd		MAC swap L2 forwarding\n"
		"  -i, --interface=n	Run on interface n\n"
		"  -q, --queue=n	Use queue n (default 0)\n"
		"  -p, --poll		Use poll syscall\n"
		"  -S, --xdp-skb=n	Use XDP skb-mod\n"
		"  -N, --xdp-native=n	Enfore XDP native mode\n"
		"  -n, --interval=n	Specify statistics update interval (default 1 sec).\n"
		"  -z, --zero-copy      Force zero-copy mode.\n"
		"  -c, --copy           Force copy mode.\n"
		"  -f, --frame-size=n   Set the frame size (must be a power of two, default is %d).\n"
		"  -m, --no-need-wakeup Turn off use of driver need wakeup flag.\n"
		"  -f, --frame-size=n   Set the frame size (must be a power of two in aligned mode, default is %d).\n"
		"  -u, --unaligned	Enable unaligned chunk placement\n"
		"\n";
	fprintf(stderr, str, prog, XSK_UMEM__DEFAULT_FRAME_SIZE);
	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "Frtli:q:psSNn:czf:mu",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			opt_bench = BENCH_RXDROP;
			break;
		case 't':
			opt_bench = BENCH_TXONLY;
			break;
		case 'l':
			opt_bench = BENCH_L2FWD;
			break;
		// case 'i':
		// 	opt_if = optarg;
		// 	break;
		// case 'q':
		// 	opt_queue = atoi(optarg);
		// 	break;
		case 'p':
			opt_poll = 1;
			break;
		case 'S':
			opt_xdp_flags |= XDP_FLAGS_SKB_MODE;
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'N':
			opt_xdp_flags |= XDP_FLAGS_DRV_MODE;
			break;
		case 'n':
			opt_interval = atoi(optarg);
			break;
		case 'z':
			opt_xdp_bind_flags |= XDP_ZEROCOPY;
			break;
		case 'c':
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'u':
			opt_umem_flags |= XDP_UMEM_UNALIGNED_CHUNK_FLAG;
			opt_unaligned_chunks = 1;
			opt_mmap_flags = MAP_HUGETLB;
			break;
		case 'F':
			opt_xdp_flags &= ~XDP_FLAGS_UPDATE_IF_NOEXIST;
			break;
		case 'f':
			opt_xsk_frame_size = atoi(optarg);
		case 'm':
			opt_need_wakeup = false;
			opt_xdp_bind_flags &= ~XDP_USE_NEED_WAKEUP;
			break;

		default:
			usage(basename(argv[0]));
		}
	}



	if ((opt_xsk_frame_size & (opt_xsk_frame_size - 1)) &&
	    !opt_unaligned_chunks) {
		fprintf(stderr, "--frame-size=%d is not a power of two\n",
			opt_xsk_frame_size);
		usage(basename(argv[0]));
	}
}

static void kick_tx(struct xsk_socket_info *xsk)
{
	int ret;

	ret = sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN || errno == EBUSY)
		return;
	exit_with_error(errno);
}

static inline void complete_tx_l2fwd(struct xsk_socket_info *xsk_r,
					 struct xsk_socket_info *xsk_t,
				     struct pollfd *fds)
{
	struct xsk_umem_info *umem_t = xsk_t->umem;
	struct xsk_umem_info *umem_r = xsk_r->umem;
	u32 idx_cq = 0, idx_fq = 0;
	unsigned int rcvd;
	size_t ndescs;

	if (!xsk_t->outstanding_tx)
		return;

	if (!opt_need_wakeup || xsk_ring_prod__needs_wakeup(&xsk_t->tx))
		kick_tx(xsk_t);

	ndescs = (xsk_t->outstanding_tx > BATCH_SIZE) ? BATCH_SIZE :
		xsk_t->outstanding_tx;

	/* re-add completed Tx buffers */
	rcvd = xsk_ring_cons__peek(&umem_t->cq, ndescs, &idx_cq);
	if (rcvd > 0) {
		unsigned int i;
		int ret;

		ret = xsk_ring_prod__reserve(&umem_r->fq, rcvd, &idx_fq);
		while (ret != rcvd) {
			if (ret < 0)
				exit_with_error(-ret);
			if (xsk_ring_prod__needs_wakeup(&umem_r->fq))
				ret = poll(fds, num_socks, opt_timeout);
			ret = xsk_ring_prod__reserve(&umem_r->fq, rcvd, &idx_fq);
		}

		for (i = 0; i < rcvd; i++)
			*xsk_ring_prod__fill_addr(&umem_r->fq, idx_fq++) =
				*xsk_ring_cons__comp_addr(&umem_t->cq, idx_cq++);

		xsk_ring_prod__submit(&xsk_r->umem->fq, rcvd);
		xsk_ring_cons__release(&xsk_t->umem->cq, rcvd);
		xsk_t->outstanding_tx -= rcvd;
		xsk_t->tx_npkts += rcvd;
	}
}

static void l2fwd(struct xsk_socket_info *xsk_r, struct xsk_socket_info *xsk_t, struct pollfd *fds)
{
	unsigned int rcvd, i;
	u32 idx_rx = 0, idx_tx = 0;
	int ret;

	complete_tx_l2fwd(xsk_r, xsk_t, fds);
	complete_tx_l2fwd(xsk_t, xsk_r, fds);

	rcvd = xsk_ring_cons__peek(&xsk_r->rx, BATCH_SIZE, &idx_rx);
	if (!rcvd) {
		if (xsk_ring_prod__needs_wakeup(&xsk_r->umem->fq))
			ret = poll(fds, num_socks, opt_timeout);
		return;
	}

	ret = xsk_ring_prod__reserve(&xsk_t->tx, rcvd, &idx_tx);
	while (ret != rcvd) {
		if (ret < 0)
			exit_with_error(-ret);
		if (xsk_ring_prod__needs_wakeup(&xsk_t->tx))
			kick_tx(xsk_t);
		ret = xsk_ring_prod__reserve(&xsk_t->tx, rcvd, &idx_tx);
	}

	for (i = 0; i < rcvd; i++) {
		u64 addr = xsk_ring_cons__rx_desc(&xsk_r->rx, idx_rx)->addr;
		u32 len = xsk_ring_cons__rx_desc(&xsk_r->rx, idx_rx++)->len;
		u64 orig = addr;

		addr = xsk_umem__add_offset_to_addr(addr);
		char *pkt = xsk_umem__get_data(xsk_r->umem->buffer, addr);

		swap_mac_addresses(pkt);

		hex_dump(pkt, len, addr);
		xsk_ring_prod__tx_desc(&xsk_t->tx, idx_tx)->addr = orig;
		xsk_ring_prod__tx_desc(&xsk_t->tx, idx_tx++)->len = len;
	}

	xsk_ring_prod__submit(&xsk_t->tx, rcvd);
	xsk_ring_cons__release(&xsk_r->rx, rcvd);

	xsk_r->rx_npkts += rcvd;
	xsk_t->outstanding_tx += rcvd;
}

static void l2fwd_all(void)
{
	struct pollfd fds[MAX_SOCKS];
	int i, ret;

	memset(fds, 0, sizeof(fds));

	for (i = 0; i < num_socks; i++) {
		fds[i].fd = xsk_socket__fd(xsks[i]->xsk);
		fds[i].events = POLLOUT | POLLIN;
	}

	for (;;) {
		if (opt_poll) {
			ret = poll(fds, num_socks, opt_timeout);
			if (ret <= 0)
				continue;
		}

		for (i = 0; i < num_socks; i++)
			l2fwd(xsks[i], xsks[1 - i], fds);
	}
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	struct xsk_umem_info *umem_1, *umem_2;
	pthread_t pt;
	void *bufs;
	int ret;

	parse_command_line(argc, argv);

	ifname1 = "lo";
	ifindex1 = if_nametoindex(ifname1);
	qid1 = 8;
	if (!ifindex1) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			ifname1);
		usage(basename(argv[0]));
	}

	ifname2 = "lo";
	ifindex2 = if_nametoindex(ifname2);
	qid2 = 8;
	if (!ifindex2) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			ifname2);
		usage(basename(argv[0]));
	}

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Reserve memory for the umem. Use hugepages if unaligned chunk mode */
	bufs = mmap(NULL, NUM_FRAMES * opt_xsk_frame_size,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | opt_mmap_flags, -1, 0);
	if (bufs == MAP_FAILED) {
		printf("ERROR: mmap failed\n");
		exit(EXIT_FAILURE);
	}

       /* Create sockets... */
	umem_1 = xsk_configure_umem(bufs, NUM_FRAMES * opt_xsk_frame_size);
	xsks[num_socks++] = xsk_configure_socket(umem_1, ifname1, ifindex1, qid1, 1); //TODO: some work so that first half is for device 1 receive, device 2 receives in 2nd half

	umem_2 = xsk_configure_umem(bufs, NUM_FRAMES * opt_xsk_frame_size); //So this is a hack to use a shared Umem between != devices and qid (cf. https://lore.kernel.org/netdev/1595236694-12749-1-git-send-email-magnus.karlsson@intel.com/, it is fixed in Linux 5.8)
	xsks[num_socks++] = xsk_configure_socket(umem_2, ifname2, ifindex2, qid2, 0);

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

	setlocale(LC_ALL, "");

	ret = pthread_create(&pt, NULL, poller, NULL);
	if (ret)
		exit_with_error(ret);

	prev_time = get_nsecs();

	l2fwd_all();

	return 0;
}

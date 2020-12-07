IO_URING networking benchmarks
===

- Be **EXTRA** caution when running this on machines, io_uring is not always stable. Namely, it can cause some kernel hangs (ie with 5.4.0-53-generic which is LTS Ubuntu kernel to this date)

## Pre results

It appears that the performance is not specially impressive, but this is mostly due to the fact that AF_PACKET sockets are, by default, not built for performance.

- https://blog.packagecloud.io/eng/2016/06/22/monitoring-tuning-linux-networking-stack-receiving-data/ This blog post explains *a lot* about the Linux networking stack, from the IRQ to the moment packets are dispatched to the various sockets, going through NAPI, interrupt coalescing etc.
- https://www.kernel.org/doc/Documentation/networking/packet_mmap.txt explains how PACKET_MMAP, a AF_PACKET
 socket feature, allows to build a ring shared between user space and kernel space, which seems the right way to do high performance with AF_PACKET setups.

## Setup

1. Set up liburing
    1. `git submodule init`
    2. `git submodule update`
    3. `./configure`
    4. `make install`

2. `make`

3. `export LD_LIBRARY_PATH=/path/to/liburing`

4. `run`

## Troubleshooting

- Check kernel version. 5.7 needed for IORING_FEAT_FAST_POLL.
- Check interfaces are promiscuous (`ip link set dev <interface name> promisc on`)

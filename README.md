IO_URING networking benchmarks
===

- Be **EXTRA** caution when running this on machines, io_uring is not always stable. Namely, it can cause some kernel hangs (ie with 5.4.0-53-generic which is LTS Ubuntu kernel to this date)

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

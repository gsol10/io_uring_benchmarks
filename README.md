IO_URING networking benchmarks
===

## Setup

1. Set up liburing
    1. git submodule init
    2. `./configure`
    3. `make install`

2. `make`

3. `export LD_LIBRARY_PATH=/path/to/liburing`

4. `run`

## Troubleshooting

- Check kernel version
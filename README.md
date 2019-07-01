ipc-bench
=========

[![GitHub](https://img.shields.io/github/license/rigtorp/ipc-bench.svg)](https://github.com/rigtorp/ipc-bench/blob/master/LICENSE)

This is a fork of https://github.com/rigtorp/ipc-bench with some improvements.

Some very crude IPC benchmarks.

ping-pong latency benchmarks:
* pipes
* unix domain sockets
* tcp sockets
* udp sockets
* shared memory (spin on wait)
* shared memory (sleep on wait)

throughput benchmarks:
* pipes
* unix domain sockets
* tcp sockets
* udp sockets
* shared memory (spin on wait)
* shared memory (sleep on wait)

This software is distributed under the MIT License.

Credits
-------

* *desbma* for adding cross platform support for clock_gettime

/*
    Measure latency of IPC using unix domain sockets


    Copyright (c) 2016 Erik Rigtorp <erik@rigtorp.se>

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use,
    copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following
    conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
    OTHER DEALINGS IN THE SOFTWARE.
*/

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0) &&                           \
    defined(_POSIX_MONOTONIC_CLOCK)
#define HAS_CLOCK_GETTIME_MONOTONIC
#endif

int read_all(int fd, void *buf, size_t count) {
  size_t sofar;
  for (sofar = 0; sofar < count;) {
    ssize_t rv = read(fd, buf, count - sofar);
    if (rv < 0) {
      return -1;
    }
    sofar += rv;
  }
  return 0;
}

int write_all(int fd, const void *buf, size_t count) {
  size_t sofar;
  for (sofar = 0; sofar < count;) {
    ssize_t rv = write(fd, buf, count - sofar);
    if (rv < 0) {
      return -1;
    }
    sofar += rv;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int ofds[2];
  int ifds[2];

  int pipe_size, size;
  char *buf;
  int64_t count, i, delta;
#ifdef HAS_CLOCK_GETTIME_MONOTONIC
  struct timespec start, stop;
#else
  struct timeval start, stop;
#endif

  if (argc != 3) {
    printf("usage: pipe_lat <message-size> <roundtrip-count>\n");
    return 1;
  }

  size = atoi(argv[1]);
  count = atol(argv[2]);

  buf = malloc(size);
  if (buf == NULL) {
    perror("malloc");
    return 1;
  }

  printf("message size: %i octets\n", size);
  printf("roundtrip count: %li\n", count);

  if (pipe(ofds) == -1) {
    perror("pipe");
    return 1;
  }

  if (pipe(ifds) == -1) {
    perror("pipe");
    return 1;
  }

  pipe_size = fcntl(ofds[0], F_GETPIPE_SZ);

  if (pipe_size < (8 * size)) {
    if (fcntl(ofds[0], F_SETPIPE_SZ, 8 * size) < 0) {
      fprintf(stderr, "cat /proc/sys/fs/pipe-max-size\n");
      perror("fcntl ofds[0]");
      return 1;
    }
    if (fcntl(ofds[1], F_SETPIPE_SZ, 8 * size) < 0) {
      fprintf(stderr, "cat /proc/sys/fs/pipe-max-size\n");
      perror("fcntl ofds[1]");
      return 1;
    }
    if (fcntl(ifds[0], F_SETPIPE_SZ, 8 * size) < 0) {
      fprintf(stderr, "cat /proc/sys/fs/pipe-max-size\n");
      perror("fcntl ifds[0]");
      return 1;
    }
    if (fcntl(ifds[1], F_SETPIPE_SZ, 8 * size) < 0) {
      fprintf(stderr, "cat /proc/sys/fs/pipe-max-size\n");
      perror("fcntl ifds[1]");
      return 1;
    }
  }

  if (!fork()) { /* child */
    for (i = 0; i < count; i++) {

      if (read_all(ifds[0], buf, size) == -1) {
        perror("read");
        return 1;
      }

      if (write_all(ofds[1], buf, size) == -1) {
        perror("write");
        return 1;
      }
    }
  } else { /* parent */

#ifdef HAS_CLOCK_GETTIME_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
      perror("clock_gettime");
      return 1;
    }
#else
    if (gettimeofday(&start, NULL) == -1) {
      perror("gettimeofday");
      return 1;
    }
#endif

    for (i = 0; i < count; i++) {

      if (write_all(ifds[1], buf, size) == -1) {
        perror("write");
        return 1;
      }

      if (read_all(ofds[0], buf, size) == -1) {
        perror("read");
        return 1;
      }
    }

#ifdef HAS_CLOCK_GETTIME_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &stop) == -1) {
      perror("clock_gettime");
      return 1;
    }

    delta = ((stop.tv_sec - start.tv_sec) * 1000000000 +
             (stop.tv_nsec - start.tv_nsec));

#else
    if (gettimeofday(&stop, NULL) == -1) {
      perror("gettimeofday");
      return 1;
    }

    delta =
        (stop.tv_sec - start.tv_sec) * 1000000000 + (stop.tv_usec - start.tv_usec) * 1000;

#endif

    printf("average latency: %li ns\n", delta / (count * 2));

    wait(NULL);
  }

  return 0;
}

/*
    Measure throughput of IPC using shared memory.
    Producer will spin on CPU when queue is full.
    Consumer will spin on CPU when queue is empty.

    Copyright (c) 2019 Michael Spiegel <michael.m.spiegel@gmail.com>

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

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0) &&                           \
    defined(_POSIX_MONOTONIC_CLOCK)
#define HAS_CLOCK_GETTIME_MONOTONIC
#endif

static const int BUFFER_COPIES = 8;

struct shared_region {
  atomic_uint_fast64_t position;
  uint64_t padding[7];
  char buf[];
};

static inline uint32_t get_head(uint64_t value) {
  return (value & 0xffffffff00000000) >> 32;
}

static inline uint32_t get_tail(uint64_t value) {
  return (value & 0x00000000ffffffff);
}

static inline uint64_t create_position(uint32_t head, uint32_t tail) {
  return (((uint64_t) head) << 32) | tail;
}

static inline int is_empty(uint64_t value) {
  uint32_t head = get_head(value);
  uint32_t tail = get_tail(value);
  return (head == tail);
}

static inline int is_full(uint64_t value, size_t size) {
  uint32_t head = get_head(value);
  uint32_t tail = get_tail(value);
  return ((head + 1) % size) == tail;
}

void shmem_read(struct shared_region *shared_buf, void *buf, size_t size) {
  for (;;) {
    uint64_t position = atomic_load(&shared_buf->position);
    uint32_t tail;
    if (is_empty(position)) {
      continue;
    }
    tail = get_tail(position);
    atomic_thread_fence(memory_order_acquire);
    memcpy(buf, shared_buf->buf + (tail * size), size);
    for (;;) {
      uint32_t head = get_head(position);
      uint64_t update = create_position(head, (tail + 1) % BUFFER_COPIES);
      if (atomic_compare_exchange_weak(&shared_buf->position, &position, update)) {
        return;
      }
    }
  }
}

void shmem_write(struct shared_region *shared_buf, void *buf, size_t size) {
  for (;;) {
    uint64_t position = atomic_load(&shared_buf->position);
    uint32_t head;
    if (is_full(position, BUFFER_COPIES)) {
      continue;
    }
    head = get_head(position);
    memcpy(shared_buf->buf + (head * size), buf, size);
    atomic_thread_fence(memory_order_release);
    for (;;) {
      uint32_t tail = get_tail(position);
      uint64_t update = create_position((head + 1) % BUFFER_COPIES, tail);
      if (atomic_compare_exchange_weak(&shared_buf->position, &position, update)) {
        return;
      }
    }
  }
}

int main(int argc, char *argv[]) {
  size_t size, alloc;
  char *local_buf;
  struct shared_region *shared_buf;
  int64_t count, i, delta;
#ifdef HAS_CLOCK_GETTIME_MONOTONIC
  struct timespec start, stop;
#else
  struct timeval start, stop;
#endif

  if (argc != 3) {
    printf("usage: shmem_thr <message-size> <message-count>\n");
    return 1;
  }

  size = atoi(argv[1]);
  count = atol(argv[2]);
  alloc = sizeof(struct shared_region) + BUFFER_COPIES * size;

  local_buf = malloc(size);
  if (local_buf == NULL) {
    perror("malloc");
    return 1;
  }

  shared_buf = mmap(NULL, alloc, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  if (shared_buf == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  shared_buf->position = 0;

  printf("message size: %li octets\n", size);
  printf("message count: %li\n", count);

  if (!fork()) {
    /* child */
    for (i = 0; i < count; i++) {
      shmem_read(shared_buf, local_buf, size);
    }
  } else {
/* parent */

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
      shmem_write(shared_buf, local_buf, size);
    }

    wait(NULL);

#ifdef HAS_CLOCK_GETTIME_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &stop) == -1) {
      perror("clock_gettime");
      return 1;
    }

    delta = ((stop.tv_sec - start.tv_sec) * 1000000 +
             (stop.tv_nsec - start.tv_nsec) / 1000);

#else
    if (gettimeofday(&stop, NULL) == -1) {
      perror("gettimeofday");
      return 1;
    }

    delta =
        (stop.tv_sec - start.tv_sec) * 1000000 + (stop.tv_usec - start.tv_usec);

#endif

    if (delta > 0) {
      printf("average throughput: %li msg/s\n", (count * 1000000) / delta);
      printf("average throughput: %li Mb/s\n",
        (((count * 1000000) / delta) * size * 8) / 1000000);
    }

  }

  if (munmap(shared_buf, alloc) == -1) {
    perror("munmap");
    return 1;
  }

  free(local_buf);

  return 0;
}

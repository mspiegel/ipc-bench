/*
    Measure latency of IPC using shared memory.
    Producer will wait on condition variable when queue is full.
    Consumer will wait on condition variable when queue is empty.

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

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0) &&                           \
    defined(_POSIX_MONOTONIC_CLOCK)
#define HAS_CLOCK_GETTIME_MONOTONIC
#endif

static const int BUFFER_COPIES = 64;

struct shared_region {
  atomic_uint_fast64_t position;
  uint64_t padding[7];
  pthread_mutex_t mutex;
  pthread_cond_t is_not_empty;
  pthread_cond_t is_not_full;
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
  int was_full;
  uint64_t position = atomic_load(&shared_buf->position);
  uint32_t tail;
  if (is_empty(position)) {
    pthread_mutex_lock(&shared_buf->mutex);
    position = atomic_load(&shared_buf->position);
    while (is_empty(position)) {
      pthread_cond_wait(&shared_buf->is_not_empty, &shared_buf->mutex);
      position = atomic_load(&shared_buf->position);
    }
    pthread_mutex_unlock(&shared_buf->mutex);
  }
  tail = get_tail(position);
  atomic_thread_fence(memory_order_acquire);
  memcpy(buf, shared_buf->buf + (tail * size), size);
  for (;;) {
    uint32_t head = get_head(position);
    uint64_t update = create_position(head, (tail + 1) % BUFFER_COPIES);
    if (atomic_compare_exchange_weak(&shared_buf->position, &position, update)) {
      was_full = is_full(position, BUFFER_COPIES);
      break;
    }
  }
  if (was_full) {
    pthread_mutex_lock(&shared_buf->mutex);
    pthread_cond_signal(&shared_buf->is_not_full);
    pthread_mutex_unlock(&shared_buf->mutex);
  }
}

void shmem_write(struct shared_region *shared_buf, void *buf, size_t size) {
  int was_empty;
  uint64_t position = atomic_load(&shared_buf->position);
  uint32_t head;
  if (is_full(position, BUFFER_COPIES)) {
    pthread_mutex_lock(&shared_buf->mutex);
    position = atomic_load(&shared_buf->position);
    while (is_full(position, BUFFER_COPIES)) {
      pthread_cond_wait(&shared_buf->is_not_full, &shared_buf->mutex);
      position = atomic_load(&shared_buf->position);
    }
    pthread_mutex_unlock(&shared_buf->mutex);
  }
  head = get_head(position);
  memcpy(shared_buf->buf + (head * size), buf, size);
  atomic_thread_fence(memory_order_release);
  for (;;) {
    uint32_t tail = get_tail(position);
    uint64_t update = create_position((head + 1) % BUFFER_COPIES, tail);
    if (atomic_compare_exchange_weak(&shared_buf->position, &position, update)) {
      was_empty = is_empty(position);
      break;
    }
  }
  if (was_empty) {
    pthread_mutex_lock(&shared_buf->mutex);
    pthread_cond_signal(&shared_buf->is_not_empty);
    pthread_mutex_unlock(&shared_buf->mutex);
  }
}

int main(int argc, char *argv[]) {
  int rv;
  char *local_buf;
  struct shared_region *shared_buf1, *shared_buf2;
  pthread_mutexattr_t mutex_attr;
  pthread_condattr_t cond_attr;
  size_t size, alloc;
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
  alloc = sizeof(struct shared_region) + BUFFER_COPIES * size;

  local_buf = malloc(size);
  if (local_buf == NULL) {
    perror("malloc");
    return 1;
  }

  shared_buf1 = mmap(NULL, alloc, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  shared_buf2 = mmap(NULL, alloc, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  if (shared_buf1 == MAP_FAILED) {
    perror("mmap shared_buf1");
    return 1;
  }

  if (shared_buf2 == MAP_FAILED) {
    perror("mmap shared_buf2");
    return 1;
  }

  if ((rv = pthread_mutexattr_init(&mutex_attr)) != 0) {
    fprintf(stderr, "pthread_mutexattr_init: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED)) != 0) {
    fprintf(stderr, "pthread_mutexattr_setpshared: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_mutex_init(&shared_buf1->mutex, &mutex_attr)) != 0) {
    fprintf(stderr, "pthread_mutex_init1: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_mutex_init(&shared_buf2->mutex, &mutex_attr)) != 0) {
    fprintf(stderr, "pthread_mutex_init2: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_condattr_init(&cond_attr)) != 0) {
    fprintf(stderr, "pthread_condattr_init: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED)) != 0) {
    fprintf(stderr, "pthread_condattr_setpshared: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_cond_init(&shared_buf1->is_not_empty, &cond_attr)) != 0) {
    fprintf(stderr, "pthread_cond_init1 is_not_empty: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_cond_init(&shared_buf1->is_not_full, &cond_attr)) != 0) {
    fprintf(stderr, "pthread_cond_init1 is_not_full: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_cond_init(&shared_buf2->is_not_empty, &cond_attr)) != 0) {
    fprintf(stderr, "pthread_cond_init1 is_not_empty: %s\n", strerror(rv));
    return 1;
  }

  if ((rv = pthread_cond_init(&shared_buf2->is_not_full, &cond_attr)) != 0) {
    fprintf(stderr, "pthread_cond_init1 is_not_full: %s\n", strerror(rv));
    return 1;
  }

  shared_buf1->position = 0;
  shared_buf2->position = 0;

  printf("message size: %li octets\n", size);
  printf("message count: %li\n", count);

  for (i = 0; i < BUFFER_COPIES / 2; i++) {
    shmem_write(shared_buf1, local_buf, size);
    shmem_write(shared_buf2, local_buf, size);
  }

  if (!fork()) { /* child */
    for (i = 0; i < count; i++) {
      shmem_read(shared_buf1, local_buf, size);
      shmem_write(shared_buf2, local_buf, size);
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
      shmem_write(shared_buf1, local_buf, size);
      shmem_read(shared_buf2, local_buf, size);
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

  pthread_cond_destroy(&shared_buf1->is_not_empty);
  pthread_cond_destroy(&shared_buf1->is_not_full);
  pthread_mutex_destroy(&shared_buf1->mutex);
  pthread_cond_destroy(&shared_buf2->is_not_empty);
  pthread_cond_destroy(&shared_buf2->is_not_full);
  pthread_mutex_destroy(&shared_buf2->mutex);
  pthread_condattr_destroy(&cond_attr);
  pthread_mutexattr_destroy(&mutex_attr);

  if (munmap(shared_buf1, alloc) == -1) {
    perror("munmap");
    return 1;
  }

  if (munmap(shared_buf2, alloc) == -1) {
    perror("munmap");
    return 1;
  }

  free(local_buf);

  return 0;
}

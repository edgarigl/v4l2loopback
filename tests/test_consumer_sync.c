/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Run on an unused loopback: test_consumer_sync /dev/video10 [dma-heap]. */
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../v4l2loopback.h"

#define COUNT 4
#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL line %d: %s (errno=%d: %s)\n", \
		__LINE__, #c, errno, strerror(errno)); exit(1); } } while (0)
#define ERROR(expr, code) do { errno = 0; CHECK((expr) == -1); \
	CHECK(errno == (code)); } while (0)

static struct v4l2_buffer buffer(unsigned type, unsigned index)
{
	struct v4l2_buffer b = { 0 };
	b.type = type;
	b.memory = type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
		V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
	b.index = index;
	return b;
}

static int queue(int fd, unsigned type, unsigned index, int dmafd)
{
	struct v4l2_buffer b = buffer(type, index);
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		b.m.fd = dmafd;
		b.bytesused = 320 * 240 * 2;
	}
	return ioctl(fd, VIDIOC_QBUF, &b);
}

static int dequeue(int fd, unsigned type, struct v4l2_buffer *b)
{
	*b = buffer(type, 0);
	return ioctl(fd, VIDIOC_DQBUF, b);
}

static int readiness(int fd, short events, int timeout)
{
	struct pollfd p = { .fd = fd, .events = events };
	CHECK(poll(&p, 1, timeout) >= 0);
	return p.revents;
}

static void request(int fd, unsigned type, unsigned count)
{
	struct v4l2_requestbuffers r = { .type = type, .count = count,
		.memory = type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
			V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP };
	CHECK(ioctl(fd, VIDIOC_REQBUFS, &r) == 0);
	CHECK(r.count == count);
}

static int export(int fd, unsigned type, unsigned index)
{
	struct v4l2_exportbuffer e = { .type = type, .index = index,
		.flags = O_CLOEXEC | O_RDWR };
	CHECK(ioctl(fd, VIDIOC_EXPBUF, &e) == 0);
	return e.fd;
}

static void same_object(int a, int b)
{
	struct stat sa, sb;
	CHECK(fstat(a, &sa) == 0 && fstat(b, &sb) == 0);
	CHECK(sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev);
}

static void ignored_signal(int sig) { (void)sig; }

/* A pipe makes blocking/wakeup observable without relying on scheduler timing. */
static pid_t blocked_dequeue(int fd, unsigned type, int pipefd[2])
{
	pid_t pid;
	CHECK(pipe(pipefd) == 0);
	CHECK(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) == 0);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		struct sigaction sa = { .sa_handler = ignored_signal };
		struct v4l2_buffer b;
		int result;
		close(pipefd[0]);
		sigemptyset(&sa.sa_mask);
		CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
		result = 1;
		CHECK(write(pipefd[1], &result, sizeof(result)) == sizeof(result));
		result = dequeue(fd, type, &b) < 0 ? errno : 0;
		CHECK(write(pipefd[1], &result, sizeof(result)) == sizeof(result));
		_exit(0);
	}
	close(pipefd[1]);
	{
		int ready;
		CHECK(readiness(pipefd[0], POLLIN, 1000) & POLLIN);
		CHECK(read(pipefd[0], &ready, sizeof(ready)) == sizeof(ready));
		CHECK(ready == 1);
	}
	CHECK(readiness(pipefd[0], POLLIN, 50) == 0);
	return pid;
}

static void joined(pid_t pid, int readfd, int fd, int expected)
{
	int result, status;
	CHECK(readiness(readfd, POLLIN, 2000) & POLLIN);
	CHECK(read(readfd, &result, sizeof(result)) == sizeof(result));
	CHECK(result == expected);
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 0);
	close(readfd);
	CHECK(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
}

static void legacy_file_io(const char *path)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	unsigned char *pixels;
	size_t size;
	int w = open(path, O_RDWR), r;

	CHECK(w >= 0);
	fmt.fmt.pix.width = 320;
	fmt.fmt.pix.height = 240;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	CHECK(ioctl(w, VIDIOC_S_FMT, &fmt) == 0);
	size = fmt.fmt.pix.sizeimage;
	pixels = malloc(size);
	CHECK(pixels != NULL);
	memset(pixels, 0x5a, size);
	for (unsigned i = 0; i < 64; i++)
		CHECK(write(w, pixels, size) == (ssize_t)size);
	r = open(path, O_RDWR);
	CHECK(r >= 0);
	memset(pixels, 0, size);
	CHECK(read(r, pixels, size) == (ssize_t)size);
	for (size_t i = 0; i < size; i++)
		CHECK(pixels[i] == 0x5a);
	free(pixels);
	close(r);
	close(w);
	puts("ok - legacy write/read works after the sync pool closes");
}

int main(int argc, char **argv)
{
	unsigned out = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	unsigned cap = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2loopback_bind_dmabuf bind = { 0 };
	struct v4l2_buffer b;
	struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	uint32_t enable = 1;
	int w, r, heap, fds[COUNT], exports[COUNT], p[2];
	size_t length;
	pid_t child;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s DEVICE [DMA_HEAP]\n", argv[0]);
		return 2;
	}
	alarm(30);
	w = open(argv[1], O_RDWR | O_NONBLOCK);
	CHECK(w >= 0);
	fmt.fmt.pix.width = 320;
	fmt.fmt.pix.height = 240;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	CHECK(ioctl(w, VIDIOC_S_FMT, &fmt) == 0);
	CHECK(ioctl(w, V4L2LOOPBACK_SET_CONSUMER_SYNC, &enable) == 0);
	{
		struct v4l2_requestbuffers req = { .type = out,
			.memory = V4L2_MEMORY_MMAP, .count = COUNT };
		char byte = 0;
		ERROR(ioctl(w, VIDIOC_REQBUFS, &req), EINVAL);
		ERROR(write(w, &byte, 1), EOPNOTSUPP);
	}
	request(w, out, COUNT);
	ERROR(ioctl(w, V4L2LOOPBACK_SET_CONSUMER_SYNC, &enable), EBUSY);
	ERROR(ioctl(w, VIDIOC_EXPBUF, &eb), ENODATA);
	ERROR(ioctl(w, VIDIOC_STREAMON, &out), ENODATA);
	r = open(argv[1], O_RDWR | O_NONBLOCK);
	CHECK(r >= 0);
	{
		struct v4l2_requestbuffers req = { .type = cap,
			.memory = V4L2_MEMORY_MMAP, .count = COUNT };
		ERROR(ioctl(r, VIDIOC_REQBUFS, &req), EAGAIN);
	}
	close(r);
	b = buffer(out, 0);
	CHECK(ioctl(w, VIDIOC_QUERYBUF, &b) == 0);
	length = b.length;
	heap = open(argc == 3 ? argv[2] : "/dev/dma_heap/system", O_RDONLY);
	CHECK(heap >= 0);
	for (unsigned i = 0; i < COUNT; i++) {
		struct dma_heap_allocation_data a = { .len = length,
			.fd_flags = O_RDWR | O_CLOEXEC };
		CHECK(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) == 0);
		fds[i] = a.fd;
		bind.index = i;
		bind.fd = fds[i];
		CHECK(ioctl(w, V4L2LOOPBACK_BIND_DMABUF, &bind) == 0);
		ERROR(ioctl(w, V4L2LOOPBACK_BIND_DMABUF, &bind), EBUSY);
	}
	close(heap);
	CHECK(ioctl(w, VIDIOC_STREAMON, &out) == 0);
	r = open(argv[1], O_RDWR | O_NONBLOCK);
	CHECK(r >= 0);
	request(r, cap, COUNT);
	CHECK(queue(r, cap, 0, -1) == 0);
	CHECK(ioctl(r, VIDIOC_STREAMON, &cap) == 0);
	ERROR(dequeue(r, cap, &b), EAGAIN);
	CHECK(readiness(r, POLLIN, 0) == 0);
	/* A second CAPTURE owner cannot acquire this queue. */
	{
		int second = open(argv[1], O_RDWR | O_NONBLOCK);
		struct v4l2_requestbuffers req = { .type = cap,
			.memory = V4L2_MEMORY_MMAP, .count = COUNT };
		CHECK(second >= 0);
		ERROR(ioctl(second, VIDIOC_REQBUFS, &req), EBUSY);
		close(second);
	}
	close(r);
	puts("ok - binding publishes no frame and CAPTURE ownership is exclusive");
	ERROR(queue(w, out, 0, fds[1]), EINVAL);
	CHECK(queue(w, out, 0, fds[0]) == 0);
	ERROR(dequeue(w, out, &b), EAGAIN);
	CHECK(readiness(w, POLLOUT, 0) == 0);
	puts("ok - no consumer cannot complete OUTPUT");

	r = open(argv[1], O_RDWR | O_NONBLOCK);
	CHECK(r >= 0);
	request(r, cap, COUNT);
	for (unsigned i = 0; i < COUNT; i++) {
		exports[i] = export(r, cap, i);
		same_object(fds[i], exports[i]);
	}
	CHECK(mmap(NULL, length, PROT_READ, MAP_SHARED, r, 0) == MAP_FAILED);
	CHECK(errno == EOPNOTSUPP);
	ERROR(ioctl(w, V4L2LOOPBACK_BIND_DMABUF, &bind), EBUSY);
	CHECK(queue(r, cap, 0, -1) == 0);
	CHECK(ioctl(r, VIDIOC_STREAMON, &cap) == 0);
	ERROR(dequeue(w, out, &b), EAGAIN);
	CHECK(readiness(r, POLLIN, 0) & POLLIN);
	CHECK(dequeue(r, cap, &b) == 0 && b.index == 0 && b.sequence == 0);
	ERROR(dequeue(w, out, &b), EAGAIN);
	ERROR(queue(w, out, 0, fds[0]), EBUSY);
	CHECK(queue(r, cap, 0, -1) == 0);
	ERROR(queue(r, cap, 0, -1), EINVAL);
	CHECK(readiness(w, POLLOUT, 0) & POLLOUT);
	CHECK(dequeue(w, out, &b) == 0 && b.index == 0 && b.sequence == 0 &&
	      !(b.flags & V4L2_BUF_FLAG_ERROR) && b.memory == V4L2_MEMORY_DMABUF);
	CHECK(queue(w, out, 0, fds[0]) == 0);
	ERROR(queue(r, cap, 0, -1), EINVAL);
	ERROR(dequeue(w, out, &b), EAGAIN);
	CHECK(dequeue(r, cap, &b) == 0 && b.sequence == 1);
	CHECK(queue(r, cap, 0, -1) == 0);
	CHECK(dequeue(w, out, &b) == 0 && b.sequence == 1);
	ERROR(dequeue(w, out, &b), EAGAIN);
	puts("ok - stable prebinding, initial credit, delivery and one release");

	CHECK(queue(w, out, 1, fds[1]) == 0);
	ERROR(dequeue(r, cap, &b), EAGAIN);
	CHECK(queue(r, cap, 1, -1) == 0);
	ERROR(dequeue(w, out, &b), EAGAIN);
	CHECK(dequeue(r, cap, &b) == 0 && b.index == 1);
	CHECK(queue(r, cap, 3, -1) == 0);
	CHECK(queue(w, out, 3, fds[3]) == 0);
	CHECK(dequeue(r, cap, &b) == 0 && b.index == 3);
	CHECK(queue(r, cap, 3, -1) == 0);
	CHECK(queue(r, cap, 2, -1) == 0);
	CHECK(queue(w, out, 2, fds[2]) == 0);
	CHECK(ioctl(r, VIDIOC_STREAMOFF, &cap) == 0);
	{
		unsigned seen = 0;
		for (unsigned i = 0; i < 3; i++) {
			CHECK(dequeue(w, out, &b) == 0);
			CHECK(b.index >= 1 && b.index <= 3 && !(seen & (1u << b.index)));
			seen |= 1u << b.index;
			CHECK(!!(b.flags & V4L2_BUF_FLAG_ERROR) == (b.index != 3));
		}
		CHECK(seen == 14);
	}
	ERROR(queue(w, out, 1, fds[1]), EBUSY);
	ERROR(queue(w, out, 2, fds[2]), EBUSY);
	close(r);
	r = open(argv[1], O_RDWR | O_NONBLOCK);
	CHECK(r >= 0);
	request(r, cap, COUNT);
	for (unsigned i = 0; i < COUNT; i++)
		CHECK(queue(r, cap, i, -1) == 0);
	CHECK(ioctl(r, VIDIOC_STREAMON, &cap) == 0);
	ERROR(queue(w, out, 1, fds[1]), EBUSY);
	CHECK(queue(w, out, 0, fds[0]) == 0);
	CHECK(dequeue(r, cap, &b) == 0 && b.index == 0);
	child = blocked_dequeue(w, out, p);
	CHECK(queue(r, cap, 0, -1) == 0);
	joined(child, p[0], w, 0);
	puts("ok - cancellation retires slots; a new reader uses remaining slots");

	child = blocked_dequeue(w, out, p);
	CHECK(kill(child, SIGUSR1) == 0);
	joined(child, p[0], w, EINTR);
	child = blocked_dequeue(r, cap, p);
	CHECK(ioctl(r, VIDIOC_STREAMOFF, &cap) == 0);
	joined(child, p[0], r, EPIPE);
	CHECK(queue(r, cap, 0, -1) == 0);
	CHECK(queue(r, cap, 3, -1) == 0);
	CHECK(ioctl(r, VIDIOC_STREAMON, &cap) == 0);
	for (unsigned i = 0; i < 1000; i++) {
		CHECK(queue(w, out, 0, fds[0]) == 0);
		CHECK(dequeue(r, cap, &b) == 0 && b.index == 0);
		CHECK(queue(r, cap, 0, -1) == 0);
		CHECK(dequeue(w, out, &b) == 0 && !(b.flags & V4L2_BUF_FLAG_ERROR));
	}
	{
		int fd = export(r, cap, 0);
		same_object(exports[0], fd);
		close(fd);
	}
	/* Closing without STREAMOFF must also cancel a delivered frame. */
	CHECK(queue(w, out, 3, fds[3]) == 0);
	CHECK(dequeue(r, cap, &b) == 0 && b.index == 3);
	close(r);
	CHECK(dequeue(w, out, &b) == 0 && b.index == 3 &&
	      (b.flags & V4L2_BUF_FLAG_ERROR));
	ERROR(queue(w, out, 3, fds[3]), EBUSY);
	r = open(argv[1], O_RDWR | O_NONBLOCK);
	CHECK(r >= 0);
	request(r, cap, COUNT);
	CHECK(queue(r, cap, 0, -1) == 0);
	CHECK(ioctl(r, VIDIOC_STREAMON, &cap) == 0);
	child = blocked_dequeue(r, cap, p);
	CHECK(ioctl(w, VIDIOC_STREAMOFF, &out) == 0);
	joined(child, p[0], r, EPIPE);
	CHECK(readiness(r, POLLIN, 0) & POLLERR);
	ERROR(ioctl(w, VIDIOC_STREAMON, &out), EPIPE);
	ERROR(queue(w, out, 0, fds[0]), EPIPE);
	close(w);
	close(r);
	for (unsigned i = 0; i < COUNT; i++) {
		void *map = mmap(NULL, length, PROT_READ, MAP_SHARED, exports[i], 0);
		CHECK(map != MAP_FAILED);
		same_object(fds[i], exports[i]);
		CHECK(munmap(map, length) == 0);
		close(exports[i]);
		close(fds[i]);
	}
	puts("ok - blocking wakeup, EINTR, 1000 reuses, close and producer stop");
	legacy_file_io(argv[1]);
	puts("PASS consumer-coupled queues");
	return 0;
}

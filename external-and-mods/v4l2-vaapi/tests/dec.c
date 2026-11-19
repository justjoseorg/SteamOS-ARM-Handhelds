#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int test_open(const char *path, int flags, ...);
static int test_close(int fd);
static int test_ioctl(int fd, unsigned long request, ...);
static int test_poll(struct pollfd *fds, nfds_t count, int timeout);
#define open test_open
#define close test_close
#define ioctl test_ioctl
#define poll test_poll
#include "../src/dec.c"
#undef open
#undef close
#undef ioctl
#undef poll

static int output_set, reject_output, substitute_output, closes;
static int events, event_reads, capture_starts, capture_allocs, capture_releases;
static int fail_start, dequeue_mode, event_polls;
static uint64_t capture_ts;

void dbg(struct drv *d, const char *fmt, ...)
{
	(void)d;
	(void)fmt;
}

int gpu_wait(struct drv *d, struct surface *s, int timeout_ms)
{
	(void)d;
	(void)s;
	(void)timeout_ms;
	return 0;
}

static int test_open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	return 42;
}

static int test_close(int fd)
{
	assert(fd == 42);
	closes++;
	return 0;
}

static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
	(void)timeout;
	assert(count == 1);
	if (fds[0].events == POLLPRI)
		event_polls++;
	return 0;
}

static int test_ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	switch (request) {
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
			assert(f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_AV1);
			if (reject_output)
				return -1;
			output_set = 1;
			if (substitute_output)
				f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
		}
		return 0;
	}
	case VIDIOC_ENUM_FMT: {
		struct v4l2_fmtdesc *f = arg;
		if (f->index)
			return -1;
		f->pixelformat = f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
				V4L2_PIX_FMT_AV1 : output_set ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
		return 0;
	}
	case VIDIOC_G_FMT: {
		struct v4l2_format *f = arg;
		f->fmt.pix_mp.width = f->fmt.pix_mp.height = 64;
		f->fmt.pix_mp.plane_fmt[0].bytesperline = 128;
		f->fmt.pix_mp.plane_fmt[0].sizeimage = 12288;
		return 0;
	}
	case VIDIOC_G_CTRL:
		((struct v4l2_control *)arg)->value = 1;
		return 0;
	case VIDIOC_REQBUFS:
		if (((struct v4l2_requestbuffers *)arg)->count)
			capture_allocs++;
		else
			capture_releases++;
		return 0;
	case VIDIOC_STREAMON:
		assert(*(int *)arg == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		capture_starts++;
		return fail_start && capture_starts == 1 ? -1 : 0;
	case VIDIOC_STREAMOFF:
	case VIDIOC_QBUF:
		return 0;
	case VIDIOC_DQEVENT: {
		event_reads++;
		if (!events)
			break;
		events--;
		((struct v4l2_event *)arg)->type = V4L2_EVENT_SOURCE_CHANGE;
		return 0;
	}
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE || dequeue_mode == 4)
			break;
		if (dequeue_mode == 3) {
			errno = EIO;
			return -1;
		}
		b->index = 0;
		b->timestamp.tv_sec = capture_ts / 1000000;
		b->timestamp.tv_usec = capture_ts % 1000000;
		b->flags = dequeue_mode == 1 ? V4L2_BUF_FLAG_ERROR : 0;
		b->m.planes[0].bytesused = dequeue_mode == 2 ? 0 : 1;
		return 0;
	}
	default:
		errno = ENOTTY;
		return -1;
	}
	errno = EAGAIN;
	return -1;
}

static void reset(void)
{
	output_set = reject_output = substitute_output = closes = 0;
	events = event_reads = capture_starts = capture_allocs = capture_releases = 0;
	fail_start = dequeue_mode = event_polls = 0;
	capture_ts = 1;
}

static void test_probe(void)
{
	reset();
	assert(dec_has_av1("/dev/test"));
	assert(!output_set && closes == 1);
	assert(dec_has_p010("/dev/test"));
	assert(output_set && closes == 2);
	reset();
	reject_output = 1;
	assert(!dec_has_p010("/dev/test") && closes == 1);
	reset();
	substitute_output = 1;
	assert(!dec_has_p010("/dev/test") && closes == 1);
}

static void test_capture_retry(struct context *c, VAProfile profile)
{
	reset();
	memset(c, 0, sizeof(*c));
	struct drv d = {0};
	c->drv = &d;
	c->cfg.profile = profile;
	c->fd = 42;
	uint8_t data[2] = {0}, output[2][16];
	c->out_count = 2;
	for (int i = 0; i < 2; i++) {
		c->out[i].map = output[i];
		c->out[i].len = sizeof(output[i]);
	}
	struct surface first = {.ctx = c, .fourcc = VIDEO_NV12, .pitch = 128,
		.uv_offset = 8192, .size = 12288, .slot = -1};
	struct surface keyframe = first;
	events = fail_start = 1;
	assert(dec_submit(c, data, sizeof(data), &first) == -1);
	assert(c->failed && !c->cap_ready && !first.pending && first.error);
	assert(c->cap_format_known && capture_releases == 1 && !c->cap_count);
	c->failed = 0;
	assert(!dec_submit(c, data, sizeof(data), &keyframe));
	assert(!c->failed && c->cap_ready && keyframe.pending && !keyframe.error);
	assert(capture_starts == 2 && capture_allocs == 2);
	assert(event_reads == 1 && !event_polls);
	capture_ts = keyframe.ts;
	assert(!dec_wait(c, &keyframe, 1));
	cap_release(c);
}

static void test_decode_errors(struct context *c)
{
	for (int copy = 0; copy < 2; copy++)
		for (int mode = 1; mode <= 4; mode++) {
			reset();
			memset(c, 0, sizeof(*c));
			struct drv d = {0};
			c->drv = &d;
			c->cfg.profile = VAProfileAV1Profile0;
			c->copy_mode = copy;
			struct surface s = {.ctx = c, .pending = 1, .ts = 1};
			c->slot_owner[0] = &s;
			c->inflight[1] = &s;
			dequeue_mode = mode;
			assert(dec_wait(c, &s, 1) == -1);
			assert(s.error && !s.pending && c->failed);
			c->failed = 0;
			assert(dec_wait(c, &s, 1) == -1 && !c->failed);
		}
}

int main(void)
{
	struct context *c = malloc(sizeof(*c));
	assert(c);
	test_probe();
	test_capture_retry(c, VAProfileAV1Profile0);
	test_capture_retry(c, VAProfileH264High);
	test_decode_errors(c);
	free(c);
	puts("V4L2 format probing, capture retry and decode error tests passed");
	return 0;
}

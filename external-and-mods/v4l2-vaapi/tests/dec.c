#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int test_open(const char *path, int flags, ...);
static int test_close(int fd);
static int test_ioctl(int fd, unsigned long request, ...);
static int test_poll(struct pollfd *fds, nfds_t count, int timeout);
static void *test_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset);
static int test_munmap(void *address, size_t length);
#define open test_open
#define close test_close
#define ioctl test_ioctl
#define poll test_poll
#define mmap test_mmap
#define munmap test_munmap
#include "../src/dec.c"
#undef open
#undef close
#undef ioctl
#undef poll
#undef mmap
#undef munmap

static int output_set, reject_output, substitute_output, opens, closes;
static int events, event_reads, capture_starts, capture_allocs, capture_releases;
static int fail_start, pending_output, mappings, dequeue_mode;
static uint32_t codec;
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
	opens++;
	events = 1;
	return 42;
}

static int test_close(int fd)
{
	assert(fd == 42 && !pending_output);
	closes++;
	return 0;
}

static void *test_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
	(void)address;
	(void)prot;
	(void)flags;
	(void)offset;
	assert(fd == 42);
	void *map = calloc(1, length);
	assert(map);
	mappings++;
	return map;
}

static int test_munmap(void *address, size_t length)
{
	(void)length;
	free(address);
	mappings--;
	return 0;
}

static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
	(void)fds;
	(void)timeout;
	assert(count == 1);
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
			assert(f->fmt.pix_mp.pixelformat == codec);
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
				codec : output_set ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
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
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *r = arg;
		if (r->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			if (r->count)
				capture_allocs++;
			else
				capture_releases++;
		}
		return 0;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;
		b->m.planes[0].length = 12288;
		return 0;
	}
	case VIDIOC_STREAMON:
		if (*(int *)arg == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			capture_starts++;
			if (fail_start && capture_starts == 1)
				return -1;
		}
		return 0;
	case VIDIOC_STREAMOFF:
		if (*(int *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			pending_output = 0;
		return 0;
	case VIDIOC_QBUF:
		if (((struct v4l2_buffer *)arg)->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			pending_output++;
		return 0;
	case VIDIOC_DQEVENT:
		event_reads++;
		if (!events)
			break;
		events--;
		((struct v4l2_event *)arg)->type = V4L2_EVENT_SOURCE_CHANGE;
		return 0;
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
	assert(!mappings && !pending_output);
	output_set = reject_output = substitute_output = opens = closes = 0;
	events = event_reads = capture_starts = capture_allocs = capture_releases = 0;
	fail_start = dequeue_mode = 0;
	codec = V4L2_PIX_FMT_HEVC;
	capture_ts = 1;
}

static void test_probe(void)
{
	const VAProfile profiles[] = {VAProfileHEVCMain10, VAProfileAV1Profile0};
	for (unsigned i = 0; i < 2; i++) {
		reset();
		VAProfile profile = profiles[i];
		codec = i ? V4L2_PIX_FMT_AV1 : V4L2_PIX_FMT_HEVC;
		assert((i ? dec_has_av1("/dev/test") : dec_has_hevc("/dev/test")));
		assert(!output_set && closes == 1);
		assert(dec_has_p010("/dev/test", profile) && output_set && closes == 2);
		reject_output = 1;
		assert(!dec_has_p010("/dev/test", profile) && closes == 3);
		reject_output = 0;
		substitute_output = 1;
		assert(!dec_has_p010("/dev/test", profile) && closes == 4);
	}
}

static void test_capture_reset(struct context *c, VAProfile profile, int copy)
{
	reset();
	memset(c, 0, sizeof(*c));
	struct drv d = {0};
	c->drv = &d;
	c->cfg.profile = profile;
	codec = profile == VAProfileAV1Profile0 ? V4L2_PIX_FMT_AV1 :
		hevc_profile(profile) ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
	assert(!dec_open(c));
	uint8_t data[] = {0, 0};
	struct surface first = {.ctx = c, .fourcc = VIDEO_NV12, .pitch = copy ? 64 : 128,
		.uv_offset = 8192, .size = 12288, .slot = -1};
	struct surface keyframe = first;
	fail_start = 1;
	assert(dec_submit(c, data, sizeof(data), &first) == -1);
	assert(c->failed && !c->cap_ready && !first.pending && first.error);
	assert(capture_releases == 1 && !c->cap_count && pending_output == 1);
	for (unsigned i = 0; i < MAX_SLOTS; i++)
		assert(!c->cap_map[i]);
	struct surface stale = {.pending = 1, .ctx = c};
	c->inflight[7] = &stale;
	c->newest = &stale;
	c->av1_deferred.data = malloc(1);
	assert(c->av1_deferred.data);
	c->av1_slot[0] = c->av1_refs[0].id = 1;
	c->av1_seq_len = 2;
	assert(!dec_reset(c));
	assert(!stale.pending && stale.error && !c->newest && !pending_output);
	assert(!c->cap_format_known && !c->av1_deferred.data && !c->av1_seq_len);
	assert(!c->av1_slot[0] && !c->av1_refs[0].id);
	assert(opens == 2 && closes == 1 && !c->failed && !c->cap_ready);
	assert(!dec_submit(c, data, sizeof(data), &keyframe));
	assert(c->cap_ready && keyframe.pending && !keyframe.error && pending_output == 1);
	assert(capture_starts == 2 && capture_allocs == 2 && event_reads == 2);
	capture_ts = keyframe.ts;
	if (copy) {
		keyframe.height = 64;
		keyframe.map = calloc(1, keyframe.size);
		assert(keyframe.map);
	}
	assert(!dec_wait(c, &keyframe, 1));
	free(keyframe.map);
	dec_close(c);
	assert(!mappings && opens == closes);
}

static void test_reopen_failure(struct context *c)
{
	reset();
	memset(c, 0, sizeof(*c));
	struct drv d = {0};
	c->drv = &d;
	c->cfg.profile = VAProfileHEVCMain;
	assert(!dec_open(c));
	reject_output = 1;
	assert(dec_reset(c) == -1);
	assert(c->failed && c->fd == -1 && !c->out_count && !mappings && opens == closes);
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
	for (int copy = 0; copy < 2; copy++) {
		test_capture_reset(c, VAProfileHEVCMain, copy);
		test_capture_reset(c, VAProfileH264High, copy);
		test_capture_reset(c, VAProfileAV1Profile0, copy);
	}
	test_reopen_failure(c);
	test_decode_errors(c);
	free(c);
	puts("AV1/HEVC format probing, decode errors and complete decoder reset tests passed");
	return 0;
}

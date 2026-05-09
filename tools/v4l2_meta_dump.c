#include <errno.h>
#include <fcntl.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct mapped_buf {
	void *ptr;
	size_t len;
};

static int xioctl(int fd, unsigned long req, void *arg) {
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static int setup_video(int fd, struct mapped_buf *bufs, unsigned int *count) {
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 256;
	fmt.fmt.pix.height = 392;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT video");
		return -1;
	}

	struct v4l2_requestbuffers req = {0};
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS video");
		return -1;
	}
	*count = req.count;
	for (unsigned int i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf = {0};
		buf.type = req.type;
		buf.memory = req.memory;
		buf.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF video");
			return -1;
		}
		bufs[i].len = buf.length;
		bufs[i].ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
		if (bufs[i].ptr == MAP_FAILED) {
			perror("mmap video");
			return -1;
		}
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF video");
			return -1;
		}
	}
	return 0;
}

static int setup_meta(int fd, struct mapped_buf *bufs, unsigned int *count) {
	struct v4l2_requestbuffers req = {0};
	req.count = 4;
	req.type = V4L2_BUF_TYPE_META_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS meta");
		return -1;
	}
	*count = req.count;
	for (unsigned int i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf = {0};
		buf.type = req.type;
		buf.memory = req.memory;
		buf.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF meta");
			return -1;
		}
		bufs[i].len = buf.length;
		bufs[i].ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
		if (bufs[i].ptr == MAP_FAILED) {
			perror("mmap meta");
			return -1;
		}
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF meta");
			return -1;
		}
	}
	return 0;
}

static void hexdump(const uint8_t *buf, size_t len) {
	size_t n = len < 128 ? len : 128;
	for (size_t i = 0; i < n; ++i) {
		printf("%02x", buf[i]);
		if ((i % 16) == 15 || i + 1 == n) printf("\n");
		else printf(" ");
	}
}

int main(void) {
	int vfd = open("/dev/video2", O_RDWR);
	int mfd = open("/dev/video3", O_RDWR);
	if (vfd < 0 || mfd < 0) {
		perror("open");
		return 1;
	}

	struct mapped_buf vbufs[4] = {0};
	struct mapped_buf mbufs[4] = {0};
	unsigned int vcount = 0, mcount = 0;
	if (setup_meta(mfd, mbufs, &mcount) < 0) return 1;
	if (setup_video(vfd, vbufs, &vcount) < 0) return 1;

	enum v4l2_buf_type vtype = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	enum v4l2_buf_type mtype = V4L2_BUF_TYPE_META_CAPTURE;
	if (xioctl(vfd, VIDIOC_STREAMON, &vtype) < 0) {
		perror("VIDIOC_STREAMON video");
		return 1;
	}
	if (xioctl(mfd, VIDIOC_STREAMON, &mtype) < 0) {
		perror("VIDIOC_STREAMON meta");
		return 1;
	}

	struct pollfd pfds[2] = {
		{ .fd = vfd, .events = POLLIN },
		{ .fd = mfd, .events = POLLIN },
	};
	for (int iter = 0; iter < 10; ++iter) {
		if (poll(pfds, 2, 1000) <= 0) {
			perror("poll");
			return 1;
		}

		struct v4l2_buffer vbuf = {0};
		vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vbuf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(vfd, VIDIOC_DQBUF, &vbuf) < 0) {
			perror("VIDIOC_DQBUF video");
			return 1;
		}

		struct v4l2_buffer mbuf = {0};
		mbuf.type = V4L2_BUF_TYPE_META_CAPTURE;
		mbuf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(mfd, VIDIOC_DQBUF, &mbuf) < 0) {
			perror("VIDIOC_DQBUF meta");
			return 1;
		}

		printf("iter=%d video bytesused=%u index=%u meta bytesused=%u index=%u\n",
		       iter, vbuf.bytesused, vbuf.index, mbuf.bytesused, mbuf.index);

		if (mbuf.bytesused > 0) {
			uint8_t *meta = (uint8_t *)mbufs[mbuf.index].ptr;
			hexdump(meta, mbuf.bytesused);

			size_t off = 0;
			while (off + sizeof(struct uvc_meta_buf) <= mbuf.bytesused) {
				struct uvc_meta_buf *m = (struct uvc_meta_buf *)(meta + off);
				size_t obj_len = sizeof(*m) + m->length;
				if (obj_len == 0 || off + obj_len > mbuf.bytesused) break;
				printf("meta_obj ns=%llu sof=%u len=%u flags=0x%02x\n",
				       (unsigned long long)m->ns, m->sof, m->length, m->flags);
				if (m->length > 0) {
					hexdump(m->buf, m->length);
				}
				off += obj_len;
			}
			return 0;
		}

		if (xioctl(vfd, VIDIOC_QBUF, &vbuf) < 0) {
			perror("VIDIOC_QBUF video loop");
			return 1;
		}
		if (xioctl(mfd, VIDIOC_QBUF, &mbuf) < 0) {
			perror("VIDIOC_QBUF meta loop");
			return 1;
		}
	}

	return 0;
}

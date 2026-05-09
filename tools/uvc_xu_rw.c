#include <errno.h>
#include <fcntl.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int query_ctrl(int fd, uint8_t unit, uint8_t selector, uint8_t query, uint16_t size, uint8_t *data) {
	struct uvc_xu_control_query q = {
		.unit = unit,
		.selector = selector,
		.query = query,
		.size = size,
		.data = data,
	};
	return ioctl(fd, UVCIOC_CTRL_QUERY, &q);
}

static uint8_t parse_query(const char *name) {
	if (!strcmp(name, "cur")) return UVC_GET_CUR;
	if (!strcmp(name, "min")) return UVC_GET_MIN;
	if (!strcmp(name, "max")) return UVC_GET_MAX;
	if (!strcmp(name, "res")) return UVC_GET_RES;
	if (!strcmp(name, "def")) return UVC_GET_DEF;
	if (!strcmp(name, "info")) return UVC_GET_INFO;
	if (!strcmp(name, "len")) return UVC_GET_LEN;
	if (!strcmp(name, "set")) return UVC_SET_CUR;
	fprintf(stderr, "unknown query %s\n", name);
	exit(2);
}

static int parse_hex_bytes(const char *text, uint8_t *out, size_t max_len) {
	size_t n = 0;
	while (*text) {
		while (*text == ' ' || *text == ':' || *text == ',') ++text;
		if (!*text) break;
		char *end = NULL;
		long v = strtol(text, &end, 16);
		if (end == text || v < 0 || v > 255 || n >= max_len) return -1;
		out[n++] = (uint8_t)v;
		text = end;
	}
	return (int)n;
}

int main(int argc, char **argv) {
	if (argc < 6) {
		fprintf(stderr, "usage: %s <dev> <unit> <selector> <query> <size> [hex-bytes-for-set]\n", argv[0]);
		return 2;
	}
	const char *dev = argv[1];
	uint8_t unit = (uint8_t)atoi(argv[2]);
	uint8_t selector = (uint8_t)atoi(argv[3]);
	uint8_t query = parse_query(argv[4]);
	int size = atoi(argv[5]);
	if (size <= 0 || size > 4096) {
		fprintf(stderr, "bad size\n");
		return 2;
	}
	uint8_t *buf = calloc((size_t)size, 1);
	if (!buf) {
		perror("calloc");
		return 1;
	}
	if (query == UVC_SET_CUR) {
		if (argc < 7) {
			fprintf(stderr, "set needs data bytes\n");
			return 2;
		}
		int n = parse_hex_bytes(argv[6], buf, (size_t)size);
		if (n < 0) {
			fprintf(stderr, "bad hex bytes\n");
			return 2;
		}
	}
	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	int rc = query_ctrl(fd, unit, selector, query, (uint16_t)size, buf);
	if (rc < 0) {
		fprintf(stderr, "ioctl failed: %d %s\n", errno, strerror(errno));
		close(fd);
		return 1;
	}
	for (int i = 0; i < size; ++i) {
		printf("%02x", buf[i]);
		if (i + 1 != size) printf(" ");
	}
	printf("\n");
	close(fd);
	free(buf);
	return 0;
}

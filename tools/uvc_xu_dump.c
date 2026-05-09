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

static void print_hex(const uint8_t *buf, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		printf("%02x", buf[i]);
		if (i + 1 != n) printf(" ");
	}
}

static const char *query_name(uint8_t query) {
	switch (query) {
	case UVC_GET_CUR: return "CUR";
	case UVC_GET_MIN: return "MIN";
	case UVC_GET_MAX: return "MAX";
	case UVC_GET_RES: return "RES";
	case UVC_GET_DEF: return "DEF";
	case UVC_GET_INFO: return "INFO";
	case UVC_GET_LEN: return "LEN";
	default: return "?";
	}
}

int main(int argc, char **argv) {
	const char *dev = "/dev/video2";
	uint8_t unit = 10;
	int max_selector = 23;
	int specific_selector = 0;
	if (argc > 1) dev = argv[1];
	if (argc > 2) unit = (uint8_t)atoi(argv[2]);
	if (argc > 3) max_selector = atoi(argv[3]);
	if (argc > 4) specific_selector = atoi(argv[4]);

	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	for (int sel = 1; sel <= max_selector; ++sel) {
		if (specific_selector && sel != specific_selector) continue;
		uint8_t info = 0;
		uint8_t lenbuf[2] = {0};
		int rinfo = query_ctrl(fd, unit, (uint8_t)sel, UVC_GET_INFO, sizeof(info), &info);
		int rlen = query_ctrl(fd, unit, (uint8_t)sel, UVC_GET_LEN, sizeof(lenbuf), lenbuf);
		printf("selector %d:", sel);
		if (rinfo == 0) {
			printf(" info=0x%02x", info);
		} else {
			printf(" info=ERR(%d:%s)", errno, strerror(errno));
		}
		if (rlen == 0) {
			uint16_t len = (uint16_t)lenbuf[0] | ((uint16_t)lenbuf[1] << 8);
			printf(" len=%u", len);
			if (len > 0 && len <= 4096) {
				uint8_t *buf = calloc(len, 1);
				if (!buf) {
					printf(" alloc_fail");
				} else {
					uint8_t queries[] = { UVC_GET_CUR, UVC_GET_MIN, UVC_GET_MAX, UVC_GET_RES, UVC_GET_DEF };
					for (size_t qi = 0; qi < sizeof(queries) / sizeof(queries[0]); ++qi) {
						memset(buf, 0, len);
						int rcur = query_ctrl(fd, unit, (uint8_t)sel, queries[qi], len, buf);
						printf(" %s=", query_name(queries[qi]));
						if (rcur == 0) {
							print_hex(buf, len);
						} else {
							printf("ERR(%d:%s)", errno, strerror(errno));
						}
					}
					free(buf);
				}
			}
		} else {
			printf(" len=ERR(%d:%s)", errno, strerror(errno));
		}
		printf("\n");
	}

	close(fd);
	return 0;
}

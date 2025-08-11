#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#define NIP5F_MAX_FRAME (1024 * 1024)

static int read_n(int fd, void *buf, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, p + got, n - got, 0);
    if (r == 0) return -1; // peer closed
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    got += (size_t)r;
  }
  return 0;
}

static int write_n(int fd, const void *buf, size_t n) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t put = 0;
  while (put < n) {
    ssize_t w = send(fd, p + put, n - put, 0);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return -1;
    }
    put += (size_t)w;
  }
  return 0;
}

int nip5f_read_frame(int fd, char **out_json, size_t *out_len) {
  uint8_t hdr[4];
  if (read_n(fd, hdr, 4) < 0) return -1;
  uint32_t L = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  if (L == 0 || L > NIP5F_MAX_FRAME) return -2; // invalid length
  char *buf = (char *)malloc(L + 1);
  if (!buf) return -1;
  if (read_n(fd, buf, L) < 0) { free(buf); return -1; }
  buf[L] = '\0';
  if (out_json) *out_json = buf; else free(buf);
  if (out_len) *out_len = L;
  return 0;
}

int nip5f_write_frame(int fd, const char *json, size_t len) {
  if (!json) return -1;
  uint32_t L = (uint32_t)len;
  if (L == 0 || L > NIP5F_MAX_FRAME) return -2;
  uint8_t hdr[4] = { (uint8_t)(L >> 24), (uint8_t)(L >> 16), (uint8_t)(L >> 8), (uint8_t)L };
  if (write_n(fd, hdr, 4) < 0) return -1;
  if (write_n(fd, json, L) < 0) return -1;
  return 0;
}

/* Expose prototypes for internal use by other translation units */
int nip5f_read_frame(int fd, char **out_json, size_t *out_len);
int nip5f_write_frame(int fd, const char *json, size_t len);

// M1 spike S2 (throwaway). See net_probe.h.
#include "net_probe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

int mv_net_has_dns64(void) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = NULL;
  if (getaddrinfo("ipv4only.arpa", NULL, &hints, &res) != 0) return 0;
  // A success alone proves nothing (Darwin answered on a plain IPv4 network): the address must
  // really be synthesised, with 192.0.0.170 or .171 in its last 32 bits and not v4-mapped.
  int found = 0;
  for (const struct addrinfo* r = res; r; r = r->ai_next) {
    if (r->ai_family != AF_INET6) continue;
    const struct in6_addr* a = &((const struct sockaddr_in6*)r->ai_addr)->sin6_addr;
    const unsigned char* b = a->s6_addr;
    if (!IN6_IS_ADDR_V4MAPPED(a) && b[12] == 192 && b[13] == 0 && b[14] == 0 && (b[15] == 170 || b[15] == 171)) found = 1;
  }
  freeaddrinfo(res);
  return found;
}

int mv_net_is_ipv4_literal(const char* host) {
  struct in_addr a;
  return inet_pton(AF_INET, host, &a) == 1;
}

static int is_well_known_nat64(const struct sockaddr* sa) {
  if (sa->sa_family != AF_INET6) return 0;
  static const unsigned char prefix[12] = {0, 0x64, 0xff, 0x9b};
  return memcmp(((const struct sockaddr_in6*)sa)->sin6_addr.s6_addr, prefix, sizeof prefix) == 0;
}

// Connects with a 6 s limit, then sends one range GET and reads to the end.
static void attempt(const struct sockaddr* sa, socklen_t len, const char* host, const char* port,
                    const char* path, long range_bytes, mv_net_attempt* out) {
  memset(out, 0, sizeof *out);
  out->family = sa->sa_family == AF_INET6 ? 6 : 4;
  out->well_known_nat64 = is_well_known_nat64(sa);

  const int fd = socket(sa->sa_family, SOCK_STREAM, 0);
  if (fd < 0) {
    out->error = errno;
    return;
  }
  const int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  const double t0 = now_ms();
  if (connect(fd, sa, len) != 0) {
    if (errno != EINPROGRESS) {
      out->error = errno;
      close(fd);
      return;
    }
    struct pollfd p = {fd, POLLOUT, 0};
    const int ready = poll(&p, 1, 6000);
    int soerr = 0;
    socklen_t soerr_len = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len);
    if (ready <= 0 || soerr != 0) {
      out->error = ready <= 0 ? ETIMEDOUT : soerr;
      out->connect_ms = now_ms() - t0;
      close(fd);
      return;
    }
  }
  out->connect_ms = now_ms() - t0;
  out->connected = 1;
  fcntl(fd, F_SETFL, flags);
  const struct timeval timeout = {30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

  char request[2048];
  const int n = snprintf(request, sizeof request,
                         "GET %s HTTP/1.1\r\nHost: %s:%s\r\nRange: bytes=0-%ld\r\nConnection: close\r\n\r\n",
                         path, host, port, range_bytes - 1);
  const double sent = now_ms();
  if (n <= 0 || (size_t)n >= sizeof request || send(fd, request, (size_t)n, 0) != n) {
    out->error = errno;
    close(fd);
    return;
  }

  char buffer[65536];
  char head[4096];
  size_t head_used = 0;
  int in_body = 0;
  for (;;) {
    const ssize_t got = recv(fd, buffer, sizeof buffer, 0);
    if (got < 0) {
      out->error = errno;
      break;
    }
    if (got == 0) break;
    if (out->first_byte_ms == 0) out->first_byte_ms = now_ms() - sent;
    if (in_body) {
      out->body_bytes += got;
      continue;
    }
    const size_t take = (size_t)got < sizeof head - 1 - head_used ? (size_t)got : sizeof head - 1 - head_used;
    memcpy(head + head_used, buffer, take);
    head_used += take;
    head[head_used] = 0;
    sscanf(head, "HTTP/%*d.%*d %d", &out->http_status);
    const char* end = strstr(head, "\r\n\r\n");
    if (end) {
      in_body = 1;
      out->body_bytes += (long)head_used - (long)(end + 4 - head);
      out->body_bytes += got - (ssize_t)take;
    }
  }
  out->total_ms = now_ms() - sent;
  close(fd);
}

size_t mv_net_probe_name(const char* host, const char* port, const char* path, long range_bytes,
                         mv_net_attempt* out, size_t cap, double* resolve_ms, int* gai_error) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = NULL;
  const double t0 = now_ms();
  *gai_error = getaddrinfo(host, port, &hints, &res);
  *resolve_ms = now_ms() - t0;
  if (*gai_error != 0) return 0;

  size_t used = 0;
  for (const struct addrinfo* r = res; r && used < cap; r = r->ai_next) {
    attempt(r->ai_addr, r->ai_addrlen, host, port, path, range_bytes, &out[used]);
    ++used;
    if (out[used - 1].connected && out[used - 1].error == 0) break;
  }
  freeaddrinfo(res);
  return used;
}

int mv_net_probe_raw_ipv4(const char* host, const char* port, const char* path, long range_bytes,
                          mv_net_attempt* out) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_len = sizeof a;
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)atoi(port));
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return 0;
  attempt((const struct sockaddr*)&a, sizeof a, host, port, path, range_bytes, out);
  return 1;
}

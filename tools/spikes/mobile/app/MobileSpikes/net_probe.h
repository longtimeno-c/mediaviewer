// M1 spike S2 (throwaway). One range GET over plain BSD sockets, the way a C++ core connects:
// getaddrinfo with AF_UNSPEC (src/addons/driftwire/net_posix.cpp, resolve), then connect() to each
// address in turn. URLSession hides what this shows on a NAT64 network, where it papers over IPv4.
#pragma once

#include <stddef.h>

typedef struct {
  int family;            // 4 or 6
  int well_known_nat64;  // the address is in 64:ff9b::/96
  int connected;
  int error;             // errno of the failed connect or read; 0 for none
  double connect_ms;
  double first_byte_ms;  // from sending the request
  double total_ms;
  int http_status;
  long body_bytes;
} mv_net_attempt;

// 1 when this network's DNS synthesises AAAA for ipv4only.arpa (RFC 7050): DNS64 is here.
int mv_net_has_dns64(void);

// 1 when `host` is a dotted IPv4 address.
int mv_net_is_ipv4_literal(const char* host);

// Resolves `host` and tries its addresses in order, stopping at the first that answers. Returns
// the number of attempts written. `gai_error` is getaddrinfo's result, 0 for success.
size_t mv_net_probe_name(const char* host, const char* port, const char* path, long range_bytes,
                         mv_net_attempt* out, size_t cap, double* resolve_ms, int* gai_error);

// No getaddrinfo: an AF_INET socket straight to the literal, as a stored IPv4 address would be used.
int mv_net_probe_raw_ipv4(const char* host, const char* port, const char* path, long range_bytes,
                          mv_net_attempt* out);

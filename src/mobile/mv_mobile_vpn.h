/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The phone's VPN through the person's own server (docs/plans/phone-vpn.md §1.1): the C ABI
 * between Swift and Driftwire's shared VPN (vpn::runner, netstack, tunnel::dialer, the sealed
 * hop and, with the HTTPS look, BoringSSL). Two callers:
 *
 *   the app        mv_vpn_parse_code (the pasted VPN code) and mv_vpn_derive_knock_key (Argon2id,
 *                  64 MiB: the app's budget, never the extension's, §2.1);
 *   the extension  mv_vpn_start .. mv_vpn_stop, one tunnel per process: packets from
 *                  NEPacketTunnelFlow in through mv_vpn_packet_in, out through the config's
 *                  packet_out callback; mv_vpn_settings_json for the interface's routes.
 *
 * Opaque handles, POD structs, a status from every call; nothing throws across it. Calls marked
 * BLOCKS wait on the network or the CPU and are for worker queues only. Nothing is logged with a
 * host, an address or a key (rule 6).
 */
#ifndef MV_MOBILE_VPN_H
#define MV_MOBILE_VPN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mv_vpn_status {
  MV_VPN_OK = 0,
  MV_VPN_INVALID = 1,         /* not a VPN code, or a bad argument (the HTTPS look without a knock key) */
  MV_VPN_KEY_REJECTED = 2,    /* the server answered and did not take the key (an old code) */
  MV_VPN_UNREACHABLE = 3,     /* nothing answered at the server's address and port */
  MV_VPN_NOT_FORWARDING = 4,  /* the server answered and does not forward (its VPN is off) */
  MV_VPN_FULL = 5,            /* mv_vpn_packet_in: the queue is full and the packet was dropped */
  MV_VPN_INTERNAL = 6,
} mv_vpn_status;

/* ---- the app ---------------------------------------------------------------------------------- */

/* The server's VPN code, "mediaviewer://relay#host:port#KEY", "mediaviewer://relay-tls#host:port#KEY"
 * (the server looks like a web site on that port: the HTTPS look) or a bare "host:port#KEY", spaces
 * and a newline around it allowed (serve::parse_relay_connect_code, the laptop's own parser). On OK,
 * *relay is "host:port" and *key the key (free each with mv_vpn_free), *tls nonzero for the HTTPS
 * look and *tls_port its port (the code's; 0 without the look), as mv_vpn_config takes them. */
mv_vpn_status mv_vpn_parse_code(const char* text, char** relay, char** key, int32_t* tls, uint16_t* tls_port);

/* Zeroes and frees a string this header returned. NULL is fine. */
void mv_vpn_free(char* s);

/* The 32-byte knock key for the server's HTTPS look (tls::derive_knock_key). BLOCKS: Argon2id at
 * 64 MiB, more than a packet tunnel extension may hold, so the app derives it once and hands the
 * extension the result. */
mv_vpn_status mv_vpn_derive_knock_key(const char* key, uint8_t out[32]);

/* ---- the extension ---------------------------------------------------------------------------- */

typedef struct mv_vpn mv_vpn;

/* The extension's caps (docs/plans/phone-vpn.md §2.1), far under the desktop's: app TCP
 * connections and datagram flows at once (more are refused: a reset, or dropped), and the bytes
 * held per connection for the app's side. */
#define MV_VPN_MAX_TCP 96
#define MV_VPN_MAX_UDP 64
#define MV_VPN_SEND_BUFFER (64 * 1024)

/* One whole IP packet for the interface (NEPacketTunnelFlow.writePackets). Any of the VPN's
 * threads; the bytes are valid only during the call. Never call into this header from it. */
typedef void (*mv_vpn_packet_fn)(void* ctx, const uint8_t* packet, size_t size);
/* The server's state changed: OK once a forward opened again; UNREACHABLE when one failed at the
 * server while running (call mv_vpn_check, off this thread, to learn whether it is the key or a
 * server that does not forward); or what mv_vpn_check found. Any of the VPN's threads, only on a
 * change. Never call mv_vpn_stop or mv_vpn_check from it. */
typedef void (*mv_vpn_event_fn)(void* ctx, mv_vpn_status problem);

typedef struct mv_vpn_config {
  const char* relay;          /* "host:port" (a name, an IPv4 address or "[IPv6]:port") */
  const char* key;            /* the server's key */
  int32_t tls;                /* nonzero: the server's HTTPS look (its TLS port, then the knock) */
  uint16_t tls_port;          /* 0: 443 */
  const uint8_t* knock_key;   /* with tls: the 32 bytes from mv_vpn_derive_knock_key (required) */
  mv_vpn_packet_fn packet_out;
  mv_vpn_event_fn event;      /* may be NULL */
  void* ctx;                  /* passed to both callbacks */
} mv_vpn_config;

/* Starts the VPN: first one forward to the server's own resolver, to learn that it answers and
 * takes the key (nothing leaves it), then the stack. BLOCKS for up to the forward's wait (about
 * 13 s with no answer). *out only on OK; stop it with mv_vpn_stop. */
mv_vpn_status mv_vpn_start(const mv_vpn_config* config, mv_vpn** out);

/* A packet the interface read. Never blocks: it is queued for the VPN's own thread (FULL: dropped,
 * as a full interface queue would; TCP sends it again). */
mv_vpn_status mv_vpn_packet_in(mv_vpn* vpn, const uint8_t* packet, size_t size);

/* The server again, as mv_vpn_start checks it (after the network changed, or an UNREACHABLE
 * event); the event then tells what it found. BLOCKS. */
mv_vpn_status mv_vpn_check(mv_vpn* vpn);

typedef struct mv_vpn_state {
  uint32_t tcp_open;       /* app connections through the tunnel now */
  uint64_t tcp_made;       /* in all */
  uint64_t udp_flows;      /* datagram flows (DNS) opened in all */
  uint64_t failed;         /* forwards the server refused or could not make */
  uint64_t dropped;        /* packets mv_vpn_packet_in dropped */
  mv_vpn_status problem;   /* OK, or the server's last problem */
} mv_vpn_state;
void mv_vpn_get_state(mv_vpn* vpn, mv_vpn_state* out);

/* Every stream reset, every thread joined, the handle freed. BLOCKS (briefly). NULL is fine. */
void mv_vpn_stop(mv_vpn* vpn);

/* The interface's settings as JSON, for NEPacketTunnelNetworkSettings:
 *   {"remote" (the server's first resolved address),"address4","prefix4","dns4","address6","prefix6","mtu",
 *    "include4":[{"base","prefix"}],"exclude4":[..],"include6":[..],"exclude6":[..]}
 * every address through the tunnel, both families, with the server's own addresses (`relay`'s host
 * resolved now, on the current network) and this device's local networks left out (vpn::plan_routes).
 * BLOCKS (a name lookup). Free with mv_vpn_free; NULL for a relay that is not "host:port". */
char* mv_vpn_settings_json(const char* relay);

#ifdef __cplusplus
}
#endif

#endif

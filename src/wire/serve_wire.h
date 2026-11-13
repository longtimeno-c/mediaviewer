// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What `driftwire serve` and its clients say (docs/design/28 "The NAS
// service"), apart from sockets so it is tested and fuzzed on its own. The
// message ids are in wire.h (25 and up, only on a library channel).
//
// After ident the client sends serve_hello: the protocol it speaks and the
// operations it can use. The service answers serve_caps: the same, cut to what
// both have. Neither ever sends an operation the other did not list, because a
// peer ends the link on a message id it does not know; an unknown operation
// name inside a request is answered end_code::unsupported instead.
//
// A stream carries one operation. The client picks its id, opens it with a
// credit (bytes the service may send), and tops that up as it consumes; the
// service sends at most what it has been given, in chunks of at most
// kInteractiveChunk, so a seek is never behind more than one small chunk per
// stream. stream_cancel ends one between chunks.
//
// Paths: a root id from the service's own list, plus a relative path that
// passes safe_relative(). Nothing absolute ever crosses, and no message says
// where a named folder is on the NAS. Portable (D9): no socket, no OS header.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wire/wire.h"
#include "core/result.h"

namespace mv::driftwire::serve {

// Bumped only for a change an older peer cannot ignore; an older peer's
// behaviour is kept for as long as `kMinVersion` says.
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMinVersion = 1;

// One message's file bytes. Interactive streams (a viewer, a seek) stay small
// so that one chunk of something else is never long in front of them; bulk
// (a whole-file pull) may use the channel's full chunk.
inline constexpr std::size_t kInteractiveChunk = 64 * 1024;
inline constexpr std::size_t kBulkChunk = kChunkBytes;
inline constexpr std::size_t kDataHeaderBytes = 12;  // stream id, offset
// A request's JSON, at most (the longest legal path, in escapes, fits).
inline constexpr std::size_t kMaxRequestBytes = 8192;
// A stream's outstanding credit, at most: a client cannot have the service
// committed to more than this per stream.
inline constexpr std::uint32_t kMaxStreamCredit = 8u * 1024 * 1024;
inline constexpr std::size_t kMaxStreams = 32;   // open at once, per session
inline constexpr std::size_t kMaxRoots = 32;
inline constexpr std::size_t kMaxConfigBytes = 64 * 1024;
inline constexpr std::size_t kMaxLabelBytes = 64;
// A listing, at most (a folder bigger than this says "truncated").
inline constexpr std::size_t kMaxListBytes = 16u * 1024 * 1024;

enum class op : std::uint8_t { roots = 0, list = 1, stat = 2, read_range = 3, admin = 4, upgrade = 5, write = 6 };
// What a phone can use to read. `admin` is apart: granted only to the owner's session.
inline constexpr std::uint32_t kAllOps = (1u << 4) - 1;
inline constexpr std::uint32_t kAdminOp = 1u << 4;
// A direct path after the relay (docs/plans/nas-relay.md §9): granted only to a
// session that came in through the relay. Asked on serve_admin's frame as
// {"do":"upgrade","candidates":[{"a","p"},...]}; answered with the service's.
inline constexpr std::uint32_t kUpgradeOp = 1u << 5;
// `write` (phone backup, docs/plans/mobile.md §10b) is apart too: granted to a
// device that may write (serve.h, may_write: its grant's backup flag), and then
// only into a backup folder, under its storage limit, creating files and never
// replacing or deleting one. A service from before 1.1 does not know the name
// and leaves it out of serve_caps, which is how a phone learns it is too old.
// Ops cross the wire by name, so this bit is this build's own.
inline constexpr std::uint32_t kWriteOp = 1u << 6;
inline constexpr std::size_t kMaxCandidates = 8;
inline constexpr int kPunchMs = 10000;
struct candidate {
  std::string address;  // numeric
  std::uint16_t port = 0;
};
[[nodiscard]] std::string candidates_json(const std::vector<candidate>& c, std::string_view what);
// At most kMaxCandidates numeric addresses with a port; anything else left out.
[[nodiscard]] std::vector<candidate> parse_candidates(std::string_view json);
// A candidate the service will send a SYN to: unicast and somewhere else, never
// unspecified, loopback (but for `loopback_only`, the tests: 127/8 alone),
// multicast, broadcast or link-local.
[[nodiscard]] bool usable_candidate(const candidate& c, bool loopback_only);
[[nodiscard]] const char* op_name(op o) noexcept;
[[nodiscard]] std::optional<op> parse_op(std::string_view name) noexcept;
[[nodiscard]] constexpr std::uint32_t op_bit(op o) noexcept { return 1u << static_cast<unsigned>(o); }

// A stream's last word.
enum class end_code : std::uint8_t {
  ok = 0,
  refused = 1,      // outside the named folders, or not granted
  not_found = 2,    // inside them, and not there
  unsupported = 3,  // an operation not agreed
  error = 4,        // it could not be read
  bad_request = 5,
  too_many = 6,     // kMaxStreams already open
};

// ---- writes (phone backup) -----------------------------------------------------------------
// A write stream: the client opens {"op":"write","root","rel","at","size","hash"} and the
// service answers either at once with stream_end, or with stream_credit (service to client:
// bytes it may send), then takes stream_data (client to service) at the offsets it expects,
// and tops the credit up as it writes. With every byte there it flushes, hashes what is on
// disk, and renames the file into place without replacing anything: stream_end ok
// {"name":"<rel it got>"} (" (2)" and so on for a name already taken by another file).
// A file that is already there with that name, size and hash: ok {"name","existing":true},
// at once. Refusals carry {"why":...} (kWhy* below), and "offset" carries {"have":N}: the
// bytes of this device's own unfinished copy, to go on from (`at` must equal it).
//
// `meta`: `rel` is under the folder's `.mediaviewer-phone-backup/` instead (a hidden name, so
// never reachable otherwise): any device that may write there may read and list it, and may
// write only `<its own device id>/ledger.json`, the one file the service replaces.
inline constexpr const char* kBackupMetaFolder = ".mediaviewer-phone-backup";
inline constexpr const char* kLedgerFile = "ledger.json";
inline constexpr std::uint64_t kMaxLedgerBytes = 64ull * 1024 * 1024;
inline constexpr std::uint32_t kWriteWindow = 4u * 1024 * 1024;  // credit a write stream is given at once
inline constexpr const char* kWhyNotAllowed = "not-allowed";  // not a backup folder, or this device may not write
inline constexpr const char* kWhyLimit = "limit";              // past the folder's storage limit
inline constexpr const char* kWhyFull = "full";                // the volume would keep less than the reserve
inline constexpr const char* kWhyOffset = "offset";            // `at` is not where its copy ends
inline constexpr const char* kWhyHash = "hash";                // what reached the disk is not what was sent
inline constexpr const char* kWhyBusy = "busy";                // this device is already writing that name
inline constexpr const char* kWhyName = "name";                // a name too long to keep beside its unfinished copy
inline constexpr std::size_t kMaxWriteName = 200;              // bytes of the last part of `rel`
// The 16 hex digits a device's files are known by in a backup folder: its unfinished
// copies and its ledger. The first 8 bytes of BLAKE2b-256 over its public key.
[[nodiscard]] std::string device_folder_id(const std::array<std::uint8_t, 32>& device_public);

// ---- serve_hello and serve_caps ------------------------------------------------------------
struct hello_msg {
  std::uint32_t version = 0;
  std::uint32_t min_version = 0;
  std::uint32_t ops = 0;  // op_bit()s; names this build does not know are ignored
};
[[nodiscard]] std::string hello_json(std::uint32_t ops);
[[nodiscard]] std::optional<hello_msg> parse_hello(std::string_view json);

struct caps_msg {
  std::uint32_t version = 0;
  std::uint32_t ops = 0;
  std::uint32_t chunk = 0;
  std::uint32_t bulk_chunk = 0;
  // To the owner's session alone (docs/plans/nas-owner-and-guests.md §4.1):
  // what this service can do beyond the operations, and its passphrase
  // ("set", "none", "broken"). Empty from a service older than that.
  std::vector<std::string> features;
  std::string passphrase;
  // To a device with a limit: when it ends (unix seconds on the service's
  // clock, 0: never) and whether it sees only some folders.
  bool has_grant = false;
  std::int64_t grant_expires = 0;
  bool grant_scoped = false;
};
// `owner_fields`: members appended to the object as they are (the owner's
// "features" and "passphrase"), or empty.
[[nodiscard]] std::string caps_json(std::uint32_t version, std::uint32_t ops, std::string_view owner_fields = {});
[[nodiscard]] std::optional<caps_msg> parse_caps(std::string_view json);

// ---- serve_pair ----------------------------------------------------------------------------
// A single-use code, redeemed once by a device the service has not seen (the message
// itself is below, with the claim and the owner's requests).
inline constexpr std::size_t kMaxPairCodeBytes = 128;

// ---- stream_open ---------------------------------------------------------------------------
struct open_request {
  std::uint32_t id = 0;
  op kind = op::roots;
  std::string root;
  std::string rel;           // may be empty: the root itself
  std::uint64_t at = 0;      // read_range
  std::uint64_t length = 0;  // read_range: UINT64_MAX means to the end
  std::uint32_t credit = 0;  // the first credit
  bool bulk = false;
  // write
  std::uint64_t size = 0;    // the whole file
  std::string hash;          // its BLAKE3, 64 lowercase hex digits
  bool meta = false;         // rel is under kBackupMetaFolder
};
[[nodiscard]] std::vector<std::uint8_t> encode_open(const open_request& r);
// nullopt: not a well-formed request at all. A name this build does not know
// as an operation is reported through `unknown_op`, so it can be answered
// end_code::unsupported on that stream rather than ending the link.
[[nodiscard]] std::optional<open_request> parse_open(std::span<const std::uint8_t> body, bool& unknown_op);

// ---- the fixed frames ----------------------------------------------------------------------
[[nodiscard]] std::vector<std::uint8_t> encode_credit(std::uint32_t id, std::uint32_t bytes);
struct credit_msg {
  std::uint32_t id = 0;
  std::uint32_t bytes = 0;
};
[[nodiscard]] std::optional<credit_msg> parse_credit(std::span<const std::uint8_t> body) noexcept;
[[nodiscard]] std::vector<std::uint8_t> encode_cancel(std::uint32_t id);
[[nodiscard]] std::optional<std::uint32_t> parse_cancel(std::span<const std::uint8_t> body) noexcept;

// `out` is grown to hold the header, the bytes follow it there.
void put_data_header(std::span<std::uint8_t, kDataHeaderBytes> out, std::uint32_t id, std::uint64_t offset) noexcept;
struct data_msg {
  std::uint32_t id = 0;
  std::uint64_t offset = 0;
  std::span<const std::uint8_t> bytes;
};
[[nodiscard]] std::optional<data_msg> parse_data(std::span<const std::uint8_t> body) noexcept;

[[nodiscard]] std::vector<std::uint8_t> encode_end(std::uint32_t id, end_code code, std::string_view json = {});
struct end_msg {
  std::uint32_t id = 0;
  end_code code = end_code::error;
  std::string_view json;
};
[[nodiscard]] std::optional<end_msg> parse_end(std::span<const std::uint8_t> body) noexcept;

// ---- the results, in stream_end's JSON or (list) the stream's bytes ------------------------
struct root_info {
  std::string id;
  std::string label;
  bool backup = false;    // a backup folder (1.1)
  bool writable = false;  // and this device may write there
};
[[nodiscard]] std::string roots_json(std::span<const root_info> roots);
[[nodiscard]] std::optional<std::vector<root_info>> parse_roots(std::string_view json);

struct stat_info {
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  bool dir = false;
  // A backup folder's own stat (rel empty): its storage limit, what is in it, and
  // what may still be written before either the limit or the volume's reserve.
  bool has_space = false;
  std::uint64_t limit = 0, used = 0, room = 0;
};
[[nodiscard]] std::string stat_json(const stat_info& s);
[[nodiscard]] std::optional<stat_info> parse_stat(std::string_view json);

struct list_entry {
  std::string name;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  bool dir = false;
};
// One line of a listing, its newline included.
[[nodiscard]] std::string list_line(const list_entry& e);
// A whole listing's bytes (a line that is not one fails the whole).
[[nodiscard]] std::optional<std::vector<list_entry>> parse_listing(std::string_view lines);

// ---- pairing, claiming and the owner's requests -------------------------------------------
// A single-use pairing code: 32 lowercase hex digits.
inline constexpr std::size_t kPairCodeChars = 32;
inline constexpr std::size_t kMaxPairCodes = 8;     // waiting at once
inline constexpr std::int64_t kMaxPairTtlMs = 600000;

// serve_pair: the code the QR carried and what the phone calls itself.
[[nodiscard]] std::string pair_json(std::string_view code, std::string_view name);
struct pair_msg {
  std::string code;
  std::string name;
};
[[nodiscard]] std::optional<pair_msg> parse_pair(std::string_view json);

// serve_admin's frame: u32 stream, then the JSON of the request.
[[nodiscard]] std::vector<std::uint8_t> encode_admin(std::uint32_t id, std::string_view json);
struct admin_msg {
  std::uint32_t id = 0;
  std::string_view json;
};
[[nodiscard]] std::optional<admin_msg> parse_admin(std::span<const std::uint8_t> body) noexcept;

// ---- the service's configuration (a file the desktop writes at setup) ----------------------
struct root_config {
  std::string id;     // opaque: 16 lowercase hex digits (valid_id)
  std::string label;  // shown by the client; never the path
  std::string path;   // absolute, on the NAS; never sent
  // A backup folder (1.1): phones may create files here, never past `limit`
  // bytes in it, and never leaving the volume less than `reserve` free.
  bool backup = false;
  std::uint64_t limit = 0;
  std::uint64_t reserve = 0;
};
struct config {
  std::uint16_t port = 0;
  std::string name;  // the service's name, as the client's ident shows it
  std::vector<root_config> roots;
  // The person's own relay server, "host:port" ("" for none): the service
  // waits there for its paired phones (docs/plans/nas-relay.md). Its key is in
  // the secret store (kRelayKeyName), never here.
  std::string relay;
};

// What a paired device's waiter at the relay is called: BLAKE2b-128 keyed by
// the library access key over a label and the device's public key. The relay
// sees an opaque token per phone; only a holder of the access key can name one.
[[nodiscard]] std::array<std::uint8_t, 16> relay_token(const std::array<std::uint8_t, 32>& access_key,
                                                       const std::array<std::uint8_t, 32>& device_public);
// The relay's "host:port" (a name, an IPv4 address or "[IPv6]:port"), split;
// false for anything else (no port, port 0, spaces or control characters).
[[nodiscard]] bool parse_relay_address(std::string_view address, std::string& host, std::uint16_t& port);
// {"port":N,"name":"...","roots":[{"id","label","path"}],"relay":"host:port"?}. `any_port`: port 0
// is accepted (the OS picks; the test build only): a service needs a fixed one.
[[nodiscard]] result<config> parse_config(std::string_view json, bool any_port = false);
// The same, written ("backup":{"limit","reserve"} only on a backup folder).
[[nodiscard]] std::string config_json(const config& c);
// The free space a backup folder keeps when the owner sets none.
inline constexpr std::uint64_t kDefaultReserve = 1ull << 30;

}  // namespace mv::driftwire::serve

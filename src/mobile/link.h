// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's end of the NAS service (docs/plans/mobile.md §7): `remote_file`,
// the one port every byte from the NAS comes through, and its implementation
// over a Driftwire library channel. Worker threads only: every call blocks.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "wire/net.h"
#include "wire/serve_client.h"
#include "core/result.h"

namespace mv::mobile {

using key32 = std::array<std::uint8_t, 32>;

// Set from any thread; every blocking call here checks it.
struct cancel_flag {
  std::atomic<bool> stop{false};
  [[nodiscard]] bool stopped() const noexcept { return stop.load(std::memory_order_relaxed); }
};

// What a whole-file or range read hands over, in order. Returning false stops
// the read (status::cancelled).
using byte_sink = std::function<bool(std::uint64_t offset, std::span<const std::uint8_t> bytes)>;

// The port. Everything from the NAS goes through this, and tests can fake it.
class remote_file {
 public:
  virtual ~remote_file() = default;
  [[nodiscard]] virtual result<std::vector<driftwire::serve::root_info>> roots() = 0;
  [[nodiscard]] virtual result<std::vector<driftwire::serve::list_entry>> list(const std::string& root,
                                                                               const std::string& rel) = 0;
  [[nodiscard]] virtual result<driftwire::serve::stat_info> stat(const std::string& root, const std::string& rel) = 0;
  // Reads `length` bytes from `at` (or to the end of the file). status::not_found,
  // ::permission_denied (outside the named folders), ::io (the link went: the
  // caller asks again from where it got to), ::cancelled.
  [[nodiscard]] virtual expected read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                            std::uint64_t length, const byte_sink& sink,
                                            const cancel_flag* cancel = nullptr, bool bulk = false) = 0;
};

// "host:port"; a bracketed IPv6 literal is allowed. False when it is not one.
[[nodiscard]] bool split_host_port(std::string_view text, std::string& host, std::uint16_t& port);

// Who the phone is, and where the service is said to be.
struct service_config {
  std::string local;        // "host:port", may be empty
  std::string public_name;  // "name:port", may be empty
  std::string relay;        // the person's own relay, "host:port", may be empty: tried last
  key32 identity{};         // the service's pinned public key
  key32 access_key{};
  key32 device_key{};       // this phone's X25519 private key
  std::string device_id = "0000000000000000";
  std::string device_name = "iPhone";
  // "desktop" (serve::kDeviceDesktop) when a desktop pairs to open the folders: it starts
  // as "can view files" (docs/plans/self-host-users-smb.md §9). "": a phone.
  std::string device_kind;
  // Ask for a direct path once on the relay (docs/plans/nas-relay.md §9).
  bool upgrade = false;
  bool loopback_only = false;  // the tests: punch on 127.0.0.1 only
};

enum class route_problem : std::uint8_t {
  unreachable,    // no connection, or no answer within the time
  wrong_service,  // completed a handshake with a key that is not the pinned one
  not_paired,
  too_old,
  busy,
  bad_code,       // the pairing code was wrong, spent or expired
  expired,        // this phone's limited access has ended (docs/plans/nas-owner-and-guests.md §4.3)
};

struct route_failure {
  std::string address;
  route_problem problem = route_problem::unreachable;
};

struct route_report {
  std::string address;     // the one that answered
  bool over_internet = false;
  bool relayed = false;   // through the person's own relay (the mobile-data rules apply as over the internet)
  bool upgraded = false;  // moved from the relay to a direct path it punched
  std::vector<route_failure> failures;  // the ones that did not
};

// Redeems the QR's single-use code on whichever route answers first, then
// proves it with a real connection. status::permission_denied: wrong code, or
// the wrong service; status::io: nothing answered.
[[nodiscard]] expected pair_device(const service_config& c, std::string_view code, route_report& report,
                                   const cancel_flag* cancel = nullptr, int timeout_ms = 5000);


class link_session final : public remote_file {
 public:
  explicit link_session(service_config c);
  ~link_session() override;

  // Keeps a live link (and says how it was reached); races the routes only
  // when there is none. Single-flight: concurrent callers share one race.
  [[nodiscard]] expected connect(route_report& report, const cancel_flag* cancel = nullptr, int timeout_ms = 5000);
  // The network changed: races again and swaps the winner in. Reads running on
  // the old client finish on it. On failure the old client stays.
  [[nodiscard]] expected reconnect(route_report& report, const cancel_flag* cancel = nullptr, int timeout_ms = 5000);
  // Cuts everything now (reads end with status::io).
  void disconnect() noexcept;
  // "Forget this library": disconnects and refuses every later connect.
  void shut_down() noexcept;
  // The tests: what an io error on the live client does (dropped()), without the error.
  void drop_for_test() noexcept {
    std::shared_ptr<driftwire::serve::client> c;
    {
      std::lock_guard lock(m_);
      c = live_;
    }
    if (c) dropped(c);
  }
  [[nodiscard]] bool connected() const;
  // Whether a direct path may be tried now (nas-relay.md §9.2): on Wi-Fi
  // always; on mobile data only with the person's setting on (default on).
  // The host says which network it is on and what the setting is.
  void set_upgrade_policy(bool on_cellular, bool allowed_on_cellular) noexcept;
  // The tests: try a direct path now, and wait for it (true when it moved).
  [[nodiscard]] bool upgrade_now_for_test();
  // The live connection's limit, as the service said it in serve_caps: 1
  // limited, 0 unlimited, -1 no live connection (nothing known).
  [[nodiscard]] int grant(std::int64_t& expires_unix, bool& scoped) const;
  // The caps' "storage" word (serve_wire.h): "" when healthy or none; -1 (as grant) with no live link.
  [[nodiscard]] int storage(std::string& word) const;
  // The caps' "role" for this device (serve_wire.h kCapsRole): "view", "change" or "admin"; "" from
  // a service older than roles. -1 (as grant) with no live link.
  [[nodiscard]] int role(std::string& word) const;
  // Where the service is (serve_wire.h kCapsAddresses): the addresses it was set up with, each
  // replaced by the one the latest connect's caps gave; one the caps left out stays. The next
  // race uses them, and a host keeps them so the next run does. Never blocks on a connect.
  void addresses(std::string& local, std::string& public_name, std::string& relay) const;

  [[nodiscard]] result<std::vector<driftwire::serve::root_info>> roots() override;
  [[nodiscard]] result<std::vector<driftwire::serve::list_entry>> list(const std::string& root,
                                                                       const std::string& rel) override;
  [[nodiscard]] result<driftwire::serve::stat_info> stat(const std::string& root, const std::string& rel) override;
  [[nodiscard]] expected read_range(const std::string& root, const std::string& rel, std::uint64_t at,
                                    std::uint64_t length, const byte_sink& sink, const cancel_flag* cancel,
                                    bool bulk) override;

  // Phone backup (1.1, src/mobile/backup.h): the service's write op and a backup
  // folder's meta folder. status::unsupported_format: the service is from before 1.1.
  [[nodiscard]] result<bool> can_write();
  // Clips this phone can't play (docs/plans/phone-clip-convert.md): whether the service offers
  // `convert`, a clip's plan (status::unsupported_format: not offered, or not a clip), and one
  // segment's MPEG-TS. A segment's first bytes wait for the server's encoder (kConvertStallMs).
  [[nodiscard]] result<bool> can_convert();
  [[nodiscard]] result<driftwire::serve::convert_plan> convert_plan(const std::string& root, const std::string& rel,
                                                                    const cancel_flag* cancel);
  [[nodiscard]] expected convert_read(const std::string& root, const std::string& rel, std::uint32_t segment,
                                      const byte_sink& sink, const cancel_flag* cancel);
  [[nodiscard]] result<driftwire::serve::client::write_done> write(
      const std::string& root, const std::string& rel, std::uint64_t size, const std::string& hash_hex,
      std::uint64_t at, const driftwire::serve::client::read_fn& read, const std::function<void(std::uint64_t)>& progress,
      const cancel_flag* cancel, driftwire::serve::client::write_refused* why, bool meta = false);
  // Changing the shared folders (serve_client.h "changing the shared folders"): plain
  // pass-throughs to the live client, each blocking (a worker). changes_files(): the
  // service has mkdir, move and delete; show New folder / Move / Rename / Delete only then,
  // and on a folder only when its root_info.writable. A dropped link is dropped here, as
  // write() does; `add` resumes after an offset refusal with `at` = `why->have`.
  [[nodiscard]] result<bool> changes_files();
  [[nodiscard]] result<driftwire::serve::client::change_done> add(
      const std::string& root, const std::string& rel, std::uint64_t size, const std::string& hash_hex,
      std::uint64_t at, const driftwire::serve::client::read_fn& read, const std::function<void(std::uint64_t)>& progress,
      const cancel_flag* cancel, driftwire::serve::client::change_refused* why = nullptr);
  // The phone's Add: the file at `local_path`, hashed here, sent with add() and carried on after an
  // offset refusal (a cancel or a drop left this device's unfinished copy), once more from the start
  // after a hash refusal, and once more on a new link after a drop (waiting out "busy" while the
  // service lets go of the old stream). `progress` gets the bytes the service has, of the size.
  [[nodiscard]] result<driftwire::serve::client::change_done> add_file(
      const std::string& root, const std::string& rel, const std::string& local_path,
      const std::function<void(std::uint64_t done, std::uint64_t total)>& progress, const cancel_flag* cancel,
      driftwire::serve::client::change_refused* why);
  [[nodiscard]] result<driftwire::serve::client::change_done> make_folder(
      const std::string& root, const std::string& rel, driftwire::serve::client::change_refused* why = nullptr);
  [[nodiscard]] result<driftwire::serve::client::change_done> move(
      const std::string& root, const std::string& rel, const std::string& to_root, const std::string& to_rel,
      driftwire::serve::client::change_refused* why = nullptr);
  [[nodiscard]] result<driftwire::serve::client::change_done> remove(
      const std::string& root, const std::string& rel, driftwire::serve::client::change_refused* why = nullptr);
  [[nodiscard]] result<std::vector<driftwire::serve::list_entry>> list_meta(const std::string& root, const std::string& rel);
  // The whole listing or an error: status::io when the service cut it short, so a caller that
  // drops what is not listed never drops what was merely past the cut.
  [[nodiscard]] result<std::vector<driftwire::serve::list_entry>> list_whole(const std::string& root, const std::string& rel);
  [[nodiscard]] expected read_meta(const std::string& root, const std::string& rel, const byte_sink& sink,
                                   const cancel_flag* cancel);
  [[nodiscard]] const std::string& device_folder_id() const noexcept { return device_folder_id_; }

 private:
  // The live client, connecting first if there is none.
  [[nodiscard]] result<std::shared_ptr<driftwire::serve::client>> client(const cancel_flag* cancel);
  void dropped(const std::shared_ptr<driftwire::serve::client>& c) noexcept;
  [[nodiscard]] bool is_shut() const;
  [[nodiscard]] expected race_locked(route_report& report, const cancel_flag* cancel, int timeout_ms);  // connect_m_ held
  // The direct path after the relay: one thread while the live link is relayed,
  // trying at once, then after 1, 5 and 15 minutes, then hourly.
  void start_upgrades();
  // Ends a try in flight at once (its port shut); `join` waits for the thread.
  void stop_upgrades(bool join) noexcept;
  void upgrade_loop();
  [[nodiscard]] bool try_upgrade();
  [[nodiscard]] std::uint32_t live_caps_ops() const;
  [[nodiscard]] bool upgrade_allowed() const noexcept;

  service_config config_;  // its addresses change under both connect_m_ and m_ (race_locked)
  std::string device_folder_id_;  // serve::device_folder_id of this phone's public key
  mutable std::mutex connect_m_;  // single-flight connect; held across the race, on a worker
  mutable std::mutex m_;          // guards live_
  std::shared_ptr<driftwire::serve::client> live_;
  route_report last_report_;  // under m_
  std::vector<std::weak_ptr<driftwire::serve::client>> retired_;  // under m_: swapped out, maybe still read

  // The service ends any stream past kMaxStreams with "too many", and frees a slot a moment after
  // its end reaches us: one below that, and the rest wait here (or until cancelled or shut down).
  class stream_slot;
  std::mutex streams_m_;
  std::condition_variable streams_cv_;
  std::size_t streams_ = 0;  // under streams_m_
  bool shut_ = false;         // under m_

  std::atomic<bool> on_cellular_{false}, cellular_ok_{true};
  std::mutex up_m_;
  std::condition_variable up_cv_;
  std::thread upgrader_;     // under up_m_
  bool up_stop_ = false;     // under up_m_
  bool up_now_ = false;      // under up_m_: try at once (a fresh relayed link, or the tests)
  std::shared_ptr<driftwire::net::tcp_rendezvous> up_rz_;  // under up_m_: the try in flight's port
  std::shared_ptr<driftwire::link> up_link_;               // under up_m_: a punched link mid-handshake
  bool up_running_ = false;  // under up_m_: the thread has not returned yet
};

}  // namespace mv::mobile

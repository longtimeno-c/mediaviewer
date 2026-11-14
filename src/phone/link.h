// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone access in the app (docs/plans/phone-in-the-app.md; mobile.md §5 rule
// 5, §7.2–§7.5): the NAS folder, the publisher's identity on it, the service
// on the NAS (claim, addresses, named folders), the pairing code and its QR,
// the paired phones, the nearby copy, and handing the library to another
// desktop. Settings → Phone access asks (request) and watches (state_json).
//
// Until 2026-11-12 this was Local search's (engine_phone_link.cpp): it now
// works with no add-on installed. Local search, when it is loaded, is a
// catalogue source: it builds and delivers generations signed with the same
// key (catalogue/publisher.h), and this follows them for the nearby copy. With
// no source the library is still claimed, paired and served: the phone lists
// and searches the named folders by name and tag.
//
// One request at a time on its own thread, made at launch (resume) or at the
// first request; it waits on the NAS and the OS's secret store, never on the
// UI or render thread. With a folder chosen, or Local search loaded, it also
// wakes every `tick_s` (a small JSON read) for the revoke queue, a new
// generation, and a library Local search published before the move;
// otherwise it sleeps until asked. No
// address, path or file name is logged (rule 6). Portable (D9).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/result.h"
#include "wire/secrets.h"

namespace mv::phone {

// What Local search provides while it is loaded (mv_ai_api phone_publish,
// phone_cancel, phone_json). Each is [no-block] there.
struct catalogue_source {
  std::function<expected(const std::string& destination)> publish;
  std::function<void()> cancel;
  std::function<std::string()> json;
  [[nodiscard]] explicit operator bool() const { return publish && cancel && json; }
};

class link {
 public:
  struct options {
    std::string data_dir;  // this feature's own folder: folder.json, link.json, nearby/
    // The publisher key's store, and where Local search kept it before
    // (catalogue/publisher.h). Asked on the link thread.
    std::function<std::unique_ptr<driftwire::secrets::store>()> key_store;
    std::function<std::unique_ptr<driftwire::secrets::store>()> legacy_key_store;
    // The desk (phone/desk.h, route_request), one method and its JSON.
    // status::not_found from "status" means no desk at all.
    std::function<result<std::string>(const char* method, const std::string& json)> route;
    // The name another desktop sees when this one asks for the library.
    std::string machine_name = "Another computer";
    double retry_s = 30;      // a revoke the NAS has not heard: again after this, doubling to an hour
    double code_ttl_s = 300;  // a pairing code's life on the NAS
    double tick_s = 30;       // once a folder is chosen
    // The tests: right after a hand-over is written, before anything reads it back.
    std::function<void()> after_hand_over;
    // The app's link lives to the end of the process: stop() lets a request
    // still waiting on the NAS finish on its own instead of joining it.
    bool detach_on_stop = false;
  };

  explicit link(options o);
  ~link();
  link(const link&) = delete;
  link& operator=(const link&) = delete;

  // One request {"do": ...}, queued for the link thread. [no-block]
  //   folder {path}        the NAS folder: library.json there, Local search told
  //   publish / cancel     Local search's Publish now and its Cancel
  //   refresh, claim {local, public}, pair, pair_stop, revoke {key},
  //   add_folder {label, path}, remove_folder {id}, set_addresses {local, public},
  //   takeover_ask, takeover_restart, takeover_allow {key}: as before the move.
  // The owner's passphrase and timed phones (docs/plans/nas-owner-and-guests.md §8.1):
  //   claim also takes {passphrase, new_passphrase}; pair {for_s} makes a guest code;
  //   unlock {passphrase}, lock, set_passphrase {passphrase}, grant {key, for_s (0: no limit)}.
  // Phone backup (docs/plans/mobile.md §10b.1): add_folder also takes {backup:"1", limit, reserve}
  //   (decimal bytes); set_backup {id, limit, reserve}; phone_backup {key, on} (a phone's grant).
  // A passphrase is handed to the desk and never kept here or shown in state_json.
  [[nodiscard]] expected request(const std::string& request_json);
  // The card's state, one JSON object (docs/plans/phone-in-the-app.md §3). [no-block]
  [[nodiscard]] std::string state_json() const;
  // At launch: the thread starts and, for a library set up before, the revoke
  // queue and the nearby copy carry on with no card open. [no-block]
  void resume();
  // Local search loaded (a source) or gone (an empty one). Any thread.
  void set_source(catalogue_source s);
  // Ends the thread; a code showing is written down and withdrawn next run.
  void stop() noexcept;

 private:
  struct row {
    std::string name, key;
    // A timed or scoped phone, as the NAS last said (docs/plans/nas-owner-and-guests.md §8.1).
    bool limited = false, expired = false, guest = false, scoped = false;
    std::int64_t expires = 0;
    bool backup = true;  // may back up to the backup folders (docs/plans/mobile.md §10b.1, B2)
  };
  struct folder_row {
    std::string id, label, path;
    // A backup folder (phone backup, 1.1): its sizes, and whether Local search on
    // this computer covers it, so what phones send reaches search (§10b.1, B7).
    bool backup = false;
    std::uint64_t limit = 0, reserve = 0, used = 0, room = 0;
    bool indexed = false;
  };
  // Who this library is (folder.json) and what Local search last said.
  struct publisher {
    std::string destination, library_id;
    std::uint64_t epoch = 1;
    std::uint64_t generation = 0, waiting = 0;
    std::string gen_dir;  // Local search's newest generation, here
    std::string source_library_id;  // the id and epoch Local search builds under
    std::uint64_t source_epoch = 0;
    bool source = false;  // Local search is loaded
    std::string source_json;
  };
  struct view {
    bool driftwire = true;  // false: no desk (no secret store on this machine)
    std::string busy, error;
    bool claimed = false, reachable = true;
    std::string local, public_name, warning;
    std::vector<row> devices;
    std::vector<folder_row> folders;
    std::string qr_url, qr_library, qr_fingerprint;
    std::int64_t qr_expires = 0;
    std::vector<std::string> added;
    bool nearby = false;
    std::uint64_t prepared = 0, tried = 0;
    bool other_publisher = false, mismatch = false, handed_over = false;
    bool code_left = false;
    std::int64_t code_left_until = 0;
    bool flags_unsaved = false, flags_dirty = false, pairing = false, flags_read = false;
    bool code_waiting = false;
    std::int64_t pending = 0;
    double retry_s = 0;
    std::chrono::steady_clock::time_point retry_at{};
    std::string other_fingerprint, asking_name, asking_key, asking_fingerprint;
    // The NAS owner's side (§3): "set", "none", "broken", "" (a NAS too old to say).
    std::string nas_passphrase;
    std::vector<std::string> nas_features;
    std::string relay;  // the person's own relay the service waits at ("" for none)
    std::int64_t unlocked_s = 0, locked_until = 0;
    // When the unlock read as unlocked_s runs out: the card's countdown is the
    // time left from now, not the snapshot (state_json).
    std::chrono::steady_clock::time_point unlocked_until{};
    std::int64_t qr_for_s = 0;  // the code showing is a guest code for this long
  };

  void loop();
  void quit_tidy() noexcept;
  void tick();
  void run(const std::string& request);
  void refresh(bool phones_only = false);
  void prepare_nearby();
  void load_flags();
  void save_flags();
  void load_state();
  void save_state();
  void read_source();
  // Whether a backup folder's NAS path is under one of Local search's roots here (the
  // "search_roots" Local search reports): the card's "indexed" line (§10b.1, B7).
  [[nodiscard]] bool indexed_here(const std::string& nas_path) const;
  void choose_folder(const std::string& path);
  void adopt_from_source();
  [[nodiscard]] result<std::string> route(const char* method, const std::string& json);
  [[nodiscard]] result<std::vector<std::uint8_t>> signing_key(bool make);
  void start_thread_locked();

  options o_;
  mutable std::mutex m_;
  view v_;                          // under m_
  publisher pub_;                   // under m_
  catalogue_source source_;         // under source_m_
  mutable std::mutex source_m_;     // taken alone, around a call into Local search
  std::deque<std::string> requests_;  // under m_
  std::condition_variable cv_;
  std::thread thread_;              // under m_
  std::atomic<bool> stopping_{false};
  bool state_read_ = false;         // the link thread's
  bool tick_seen_ = false;          // the link thread's
  std::vector<std::string> before_; // the link thread's: keys when the code went up
};

}  // namespace mv::phone

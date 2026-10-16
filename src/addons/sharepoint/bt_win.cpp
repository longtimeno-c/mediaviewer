// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// make_bluetooth on Windows: Windows.Devices.Bluetooth through C++/WinRT, both
// roles (docs/design/28 "Bluetooth"; the pipe is bt_pipe.h).
//
//   Peripheral (receiving side): a GattServiceProvider with the service,
//   advertised connectable; a client that subscribes to P2C is an incoming
//   link, its writes to C2P are its bytes.
//   Central (sending side): a BluetoothLEAdvertisementWatcher filtered to the
//   service; each new device is opened once to read INFO, and a link is the
//   device's service with C2P written without response and P2C notifying.
//
// Threads: WinRT calls run on the engine's threads and on the system thread
// pool, all in the process's multithreaded apartment (CoIncrementMTAUsage
// keeps one alive), where blocking on .get() is allowed. Event handlers only
// move bytes into pipes, update tables and signal; handing an incoming link
// to the engine goes to a thread of its own, through the gate. Every WinRT
// call is inside a try: the projection throws hresult_error, which never
// leaves this file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "addons/sharepoint/bt_pipe.h"
#include "addons/sharepoint/presence.h"
#include "addons/sharepoint/wire.h"

namespace mv::sharepoint {

namespace {

namespace wdb = winrt::Windows::Devices::Bluetooth;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace streams = winrt::Windows::Storage::Streams;

winrt::guid guid_of(const char* text) {
  bt::guid_parts g;
  (void)bt::parse_guid(text, g);  // our own constants: always the shape
  return winrt::guid{g.d1, g.d2, g.d3, {g.d4[0], g.d4[1], g.d4[2], g.d4[3], g.d4[4], g.d4[5], g.d4[6], g.d4[7]}};
}

streams::IBuffer buffer_of(std::span<const std::uint8_t> bytes) {
  streams::DataWriter w;
  w.WriteBytes(winrt::array_view<const std::uint8_t>(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
  return w.DetachBuffer();
}

std::vector<std::uint8_t> bytes_of(const streams::IBuffer& b) {
  std::vector<std::uint8_t> out(b ? b.Length() : 0);
  if (!out.empty()) {
    auto r = streams::DataReader::FromBuffer(b);
    r.ReadBytes(winrt::array_view<std::uint8_t>(out.data(), static_cast<std::uint32_t>(out.size())));
  }
  return out;
}

std::string address_text(std::uint64_t a) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%012llx", static_cast<unsigned long long>(a));
  return buf;
}

std::uint64_t address_of(const std::string& handle) {
  try {
    return std::stoull(handle, nullptr, 16);
  } catch (...) {
    return 0;
  }
}

// An open GATT client connection to a peer's service (central side).
struct remote {
  wdb::BluetoothLEDevice device{nullptr};
  gatt::GattDeviceService service{nullptr};
  gatt::GattCharacteristic c2p{nullptr}, p2c{nullptr};
  winrt::event_token value_token{};
  std::uint32_t packet = 20;

  void close() noexcept {
    try {
      if (p2c && value_token.value) p2c.ValueChanged(value_token);
    } catch (...) {
    }
    try {
      if (service) service.Close();
    } catch (...) {
    }
    try {
      if (device) device.Close();
    } catch (...) {
    }
    p2c = nullptr;
    c2p = nullptr;
    service = nullptr;
    device = nullptr;
  }
};

result<std::shared_ptr<remote>> open_remote(std::uint64_t address, bool with_pipe) {
  try {
    auto r = std::make_shared<remote>();
    r->device = wdb::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
    if (!r->device) return err(status::not_found);
    auto services = r->device.GetGattServicesForUuidAsync(guid_of(bt::kServiceUuid), wdb::BluetoothCacheMode::Uncached).get();
    if (services.Status() != gatt::GattCommunicationStatus::Success || services.Services().Size() == 0) {
      r->close();
      return err(status::not_found);
    }
    r->service = services.Services().GetAt(0);
    if (!with_pipe) return r;
    auto c2p = r->service.GetCharacteristicsForUuidAsync(guid_of(bt::kC2PUuid), wdb::BluetoothCacheMode::Uncached).get();
    auto p2c = r->service.GetCharacteristicsForUuidAsync(guid_of(bt::kP2CUuid), wdb::BluetoothCacheMode::Uncached).get();
    if (c2p.Status() != gatt::GattCommunicationStatus::Success || c2p.Characteristics().Size() == 0 ||
        p2c.Status() != gatt::GattCommunicationStatus::Success || p2c.Characteristics().Size() == 0) {
      r->close();
      return err(status::not_found);
    }
    r->c2p = c2p.Characteristics().GetAt(0);
    r->p2c = p2c.Characteristics().GetAt(0);
    try {
      auto session = gatt::GattSession::FromDeviceIdAsync(r->device.BluetoothDeviceId()).get();
      if (session && session.MaxPduSize() > 3) r->packet = session.MaxPduSize() - 3u;
    } catch (...) {
    }
    return r;
  } catch (...) {
    return err(status::io);
  }
}

// What the radio's handlers touch. They hold it by shared_ptr, so a handler
// still running while the bearer stops never reaches freed memory.
struct core {
  std::atomic<bool> running{false};
  std::shared_ptr<bt::callback_gate> gate;
  bluetooth::peer_fn on_peer;
  bluetooth::link_fn on_link;
  gatt::GattLocalCharacteristic p2c{nullptr};

  std::mutex m;
  struct client_state {
    gatt::GattSubscribedClient client{nullptr};
    std::weak_ptr<bt::pipe> pipe;
  };
  std::map<std::wstring, client_state> clients;                         // under m
  std::map<std::string, std::pair<std::string, std::string>> info;      // under m
  std::map<std::string, std::chrono::steady_clock::time_point> reported;  // under m
  std::set<std::string> probing;                                        // under m
  struct worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<worker> threads;  // under m; finished ones are joined as new ones start

  // Under m: a thread of its own for `fn` (a probe, a hand-off).
  template <typename Fn>
  void spawn_locked(Fn&& fn) {
    for (auto it = threads.begin(); it != threads.end();) {
      if (it->done->load()) {
        it->thread.join();
        it = threads.erase(it);
      } else {
        ++it;
      }
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    threads.push_back({std::thread([fn = std::forward<Fn>(fn), done]() mutable {
                         try {
                           fn();
                         } catch (...) {
                         }
                         done->store(true);
                       }),
                       done});
  }

  void report(const bt_peer& peer) {
    gate->call([&] {
      if (on_peer) on_peer(peer);
    });
  }
};

void on_advertisement(const std::shared_ptr<core>& c, std::uint64_t address) {
  if (!c->running.load()) return;
  const std::string key = address_text(address);
  std::unique_lock lock(c->m);
  const auto known = c->info.find(key);
  if (known != c->info.end()) {
    const auto now = std::chrono::steady_clock::now();
    auto& last = c->reported[key];
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    const bt_peer peer{key, known->second.first, known->second.second};
    lock.unlock();
    c->report(peer);
    return;
  }
  if (c->probing.count(key) || c->probing.size() >= 8) return;  // a few at a time
  c->probing.insert(key);
  // Who it is: open it once and read INFO, off the watcher's thread.
  c->spawn_locked([c, address, key] {
    std::string id, name;
    if (auto r = open_remote(address, false)) {
      try {
        auto chars =
            (*r)->service.GetCharacteristicsForUuidAsync(guid_of(bt::kInfoUuid), wdb::BluetoothCacheMode::Uncached).get();
        if (chars.Status() == gatt::GattCommunicationStatus::Success && chars.Characteristics().Size() > 0) {
          auto read = chars.Characteristics().GetAt(0).ReadValueAsync(wdb::BluetoothCacheMode::Uncached).get();
          if (read.Status() == gatt::GattCommunicationStatus::Success) {
            const auto value = bytes_of(read.Value());
            if (!bt::parse_info(value, id, name)) id.clear();
          }
        }
      } catch (...) {
        id.clear();
      }
      (*r)->close();
    }
    {
      std::lock_guard l(c->m);
      c->probing.erase(key);
      if (!id.empty()) {
        c->info[key] = {id, name};
        c->reported[key] = std::chrono::steady_clock::now();
      }
    }
    if (!id.empty()) c->report(bt_peer{key, id, name});
  });
}

void on_write(const std::shared_ptr<core>& c, const gatt::GattWriteRequestedEventArgs& args) {
  auto deferral = args.GetDeferral();
  try {
    auto request = args.GetRequestAsync().get();
    const std::wstring who(args.Session().DeviceId().Id());
    std::shared_ptr<bt::pipe> p;
    {
      std::lock_guard lock(c->m);
      const auto it = c->clients.find(who);
      if (it != c->clients.end()) p = it->second.pipe.lock();
    }
    if (request && p) p->deliver(bytes_of(request.Value()));
    if (request && request.Option() == gatt::GattWriteOption::WriteWithResponse) request.Respond();
  } catch (...) {
  }
  deferral.Complete();
}

void on_subscribers(const std::shared_ptr<core>& c, const gatt::GattLocalCharacteristic& characteristic) {
  if (!c->running.load()) return;
  std::set<std::wstring> now;
  std::vector<gatt::GattSubscribedClient> fresh;
  try {
    for (const auto& client : characteristic.SubscribedClients()) {
      const std::wstring who(client.Session().DeviceId().Id());
      now.insert(who);
      std::lock_guard lock(c->m);
      if (!c->clients.count(who)) fresh.push_back(client);
    }
  } catch (...) {
    return;
  }
  {
    // Gone: their links end.
    std::lock_guard lock(c->m);
    for (auto it = c->clients.begin(); it != c->clients.end();) {
      if (now.count(it->first)) {
        ++it;
        continue;
      }
      if (auto p = it->second.pipe.lock()) p->closed_by_peer();
      it = c->clients.erase(it);
    }
  }
  for (const auto& client : fresh) {
    std::uint32_t packet = 20;
    try {
      packet = client.MaxNotificationSize();
    } catch (...) {
    }
    gatt::GattLocalCharacteristic p2c = c->p2c;
    auto send = [p2c, client](std::span<const std::uint8_t> bytes) -> bool {
      try {
        auto r = p2c.NotifyValueAsync(buffer_of(bytes), client).get();
        return r && r.Status() == gatt::GattCommunicationStatus::Success;
      } catch (...) {
        return false;
      }
    };
    auto closed = [p2c, client]() {
      try {
        const std::uint8_t close[1] = {bt::kPacketClose};
        (void)p2c.NotifyValueAsync(buffer_of(close), client).get();
      } catch (...) {
      }
    };
    auto p = std::make_shared<bt::pipe>(packet, send, closed);
    std::wstring who;
    try {
      who = std::wstring(client.Session().DeviceId().Id());
    } catch (...) {
      continue;
    }
    std::lock_guard lock(c->m);
    c->clients[who] = core::client_state{client, p};
    // The engine may wait: hand the link over on a thread of its own.
    c->spawn_locked([c, p] {
      c->gate->call([&] {
        if (c->on_link) c->on_link(std::unique_ptr<link>(new bt::pipe_ref(p)));
      });
    });
  }
}

class win_bluetooth final : public bluetooth {
 public:
  win_bluetooth() { (void)::CoIncrementMTAUsage(&mta_); }
  ~win_bluetooth() override {
    stop();
    if (mta_) (void)::CoDecrementMTAUsage(mta_);
  }

  bool start(const std::string& id, const std::string& name, peer_fn on_peer, link_fn on_link,
             std::string& why) override {
    stop();
    {
      std::lock_guard lock(m_);
      c_ = std::make_shared<core>();
    }
    c_->on_peer = std::move(on_peer);
    c_->on_link = std::move(on_link);
    c_->gate = std::make_shared<bt::callback_gate>();
    c_->gate->open();
    try {
      auto adapter = wdb::BluetoothAdapter::GetDefaultAsync().get();
      if (!adapter || !adapter.IsLowEnergySupported()) {
        why = "This PC has no Bluetooth LE.";
        stop();
        return false;
      }
      auto radio = adapter.GetRadioAsync().get();
      if (radio && radio.State() != winrt::Windows::Devices::Radios::RadioState::On) {
        why = "Bluetooth is off.";
        stop();
        return false;
      }
      c_->running.store(true);
      start_watcher();
      if (adapter.IsPeripheralRoleSupported()) start_provider(bt::info_json(id, name));
      return true;
    } catch (const winrt::hresult_error& e) {
      why = e.code() == E_ACCESSDENIED ? "MediaViewer may not use Bluetooth. Allow it in Settings, Privacy & security."
                                       : "Bluetooth could not start.";
      stop();
      return false;
    } catch (...) {
      why = "Bluetooth could not start.";
      stop();
      return false;
    }
  }

  void stop() noexcept override {
    std::shared_ptr<core> c;
    {
      std::lock_guard lock(m_);
      c = c_;
    }
    if (!c) return;
    c_->running.store(false);
    c_->gate->close();  // no call into the engine after this
    try {
      if (watcher_) {
        watcher_.Received(received_token_);
        watcher_.Stop();
      }
    } catch (...) {
    }
    watcher_ = nullptr;
    try {
      if (provider_) provider_.StopAdvertising();
    } catch (...) {
    }
    try {
      if (c2p_local_) c2p_local_.WriteRequested(write_token_);
      if (p2c_local_) p2c_local_.SubscribedClientsChanged(subscribe_token_);
    } catch (...) {
    }
    provider_ = nullptr;
    c2p_local_ = nullptr;
    p2c_local_ = nullptr;
    info_local_ = nullptr;
    std::vector<core::worker> threads;
    {
      std::lock_guard lock(c_->m);
      for (auto& [k, cs] : c_->clients) {
        if (auto p = cs.pipe.lock()) p->closed_by_peer();
      }
      c_->clients.clear();
      threads.swap(c_->threads);
    }
    for (auto& t : threads) {
      if (t.thread.joinable()) t.thread.join();
    }
    c_->p2c = nullptr;
    std::lock_guard lock(m_);
    c_.reset();
  }

  result<std::unique_ptr<link>> connect(const std::string& handle, int timeout_ms) override {
    (void)timeout_ms;  // WinRT's own connection timeout applies
    const std::uint64_t address = address_of(handle);
    std::shared_ptr<core> c;
    {
      std::lock_guard lock(m_);
      c = c_;
    }
    if (!address || !c || !c->running.load()) return err(status::not_found);
    auto opened = open_remote(address, true);
    if (!opened) return err(opened.error());
    std::shared_ptr<remote> r = *opened;
    auto send = [r](std::span<const std::uint8_t> packet) -> bool {
      try {
        if (!r->c2p) return false;
        auto res =
            r->c2p.WriteValueWithResultAsync(buffer_of(packet), gatt::GattWriteOption::WriteWithoutResponse).get();
        return res.Status() == gatt::GattCommunicationStatus::Success;
      } catch (...) {
        return false;
      }
    };
    auto closed = [r]() {
      // On the thread that shut the link: a close packet, then the device goes.
      try {
        if (r->c2p) {
          const std::uint8_t close[1] = {bt::kPacketClose};
          (void)r->c2p.WriteValueWithResultAsync(buffer_of(close), gatt::GattWriteOption::WriteWithoutResponse).get();
        }
      } catch (...) {
      }
      r->close();
    };
    auto p = std::make_shared<bt::pipe>(r->packet, send, closed);
    std::weak_ptr<bt::pipe> weak = p;
    try {
      r->value_token = r->p2c.ValueChanged(
          [weak](const gatt::GattCharacteristic&, const gatt::GattValueChangedEventArgs& args) {
            if (auto pp = weak.lock()) pp->deliver(bytes_of(args.CharacteristicValue()));
          });
      r->device.ConnectionStatusChanged(
          [weak](const wdb::BluetoothLEDevice& d, const winrt::Windows::Foundation::IInspectable&) {
            if (d.ConnectionStatus() == wdb::BluetoothConnectionStatus::Disconnected) {
              if (auto pp = weak.lock()) pp->closed_by_peer();
            }
          });
      const auto st = r->p2c
                          .WriteClientCharacteristicConfigurationDescriptorAsync(
                              gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify)
                          .get();
      if (st != gatt::GattCommunicationStatus::Success) {
        r->close();
        return err(status::io);
      }
    } catch (...) {
      r->close();
      return err(status::io);
    }
    return std::unique_ptr<link>(new bt::pipe_ref(std::move(p)));
  }

 private:
  void start_watcher() {
    watcher_ = adv::BluetoothLEAdvertisementWatcher();
    watcher_.ScanningMode(adv::BluetoothLEScanningMode::Active);
    watcher_.AdvertisementFilter().Advertisement().ServiceUuids().Append(guid_of(bt::kServiceUuid));
    std::shared_ptr<core> c = c_;
    received_token_ = watcher_.Received(
        [c](const adv::BluetoothLEAdvertisementWatcher&, const adv::BluetoothLEAdvertisementReceivedEventArgs& args) {
          on_advertisement(c, args.BluetoothAddress());
        });
    watcher_.Start();
  }

  void start_provider(const std::string& info) {
    auto made = gatt::GattServiceProvider::CreateAsync(guid_of(bt::kServiceUuid)).get();
    if (made.Error() != wdb::BluetoothError::Success) return;  // can find, cannot be found
    provider_ = made.ServiceProvider();

    gatt::GattLocalCharacteristicParameters ip;
    ip.CharacteristicProperties(gatt::GattCharacteristicProperties::Read);
    ip.ReadProtectionLevel(gatt::GattProtectionLevel::Plain);
    ip.StaticValue(buffer_of({reinterpret_cast<const std::uint8_t*>(info.data()), info.size()}));
    info_local_ = provider_.Service().CreateCharacteristicAsync(guid_of(bt::kInfoUuid), ip).get().Characteristic();

    gatt::GattLocalCharacteristicParameters wp;
    wp.CharacteristicProperties(gatt::GattCharacteristicProperties::WriteWithoutResponse |
                                gatt::GattCharacteristicProperties::Write);
    wp.WriteProtectionLevel(gatt::GattProtectionLevel::Plain);
    c2p_local_ = provider_.Service().CreateCharacteristicAsync(guid_of(bt::kC2PUuid), wp).get().Characteristic();

    gatt::GattLocalCharacteristicParameters np;
    np.CharacteristicProperties(gatt::GattCharacteristicProperties::Notify);
    np.ReadProtectionLevel(gatt::GattProtectionLevel::Plain);
    p2c_local_ = provider_.Service().CreateCharacteristicAsync(guid_of(bt::kP2CUuid), np).get().Characteristic();
    if (!info_local_ || !c2p_local_ || !p2c_local_) {
      provider_ = nullptr;
      return;
    }
    c_->p2c = p2c_local_;
    std::shared_ptr<core> c = c_;
    write_token_ = c2p_local_.WriteRequested(
        [c](const gatt::GattLocalCharacteristic&, const gatt::GattWriteRequestedEventArgs& args) { on_write(c, args); });
    subscribe_token_ = p2c_local_.SubscribedClientsChanged(
        [c](const gatt::GattLocalCharacteristic& ch, const winrt::Windows::Foundation::IInspectable&) {
          on_subscribers(c, ch);
        });

    gatt::GattServiceProviderAdvertisingParameters ap;
    ap.IsConnectable(true);
    ap.IsDiscoverable(true);
    provider_.StartAdvertising(ap);
  }

  CO_MTA_USAGE_COOKIE mta_{};
  std::mutex m_;              // guards c_ itself (connect runs on the engine's thread)
  std::shared_ptr<core> c_;   // set and reset under m_; used freely by start/stop (the control thread)
  adv::BluetoothLEAdvertisementWatcher watcher_{nullptr};
  winrt::event_token received_token_{};
  gatt::GattServiceProvider provider_{nullptr};
  gatt::GattLocalCharacteristic info_local_{nullptr}, c2p_local_{nullptr}, p2c_local_{nullptr};
  winrt::event_token write_token_{}, subscribe_token_{};
};

}  // namespace

std::unique_ptr<bluetooth> make_bluetooth() {
  try {
    return std::make_unique<win_bluetooth>();
  } catch (...) {
    return nullptr;
  }
}

}  // namespace mv::sharepoint

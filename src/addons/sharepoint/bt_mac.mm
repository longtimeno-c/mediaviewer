// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// make_bluetooth on macOS: CoreBluetooth, both roles (docs/design/28
// "Bluetooth"; the pipe is bt_pipe.h).
//
//   Peripheral (receiving side): advertises the service; a central that
//   subscribes to P2C is an incoming link, its writes to C2P are its bytes.
//   Central (sending side): scans for the service, connects to each new
//   peripheral once to read INFO (who it is), and opens a link by connecting,
//   subscribing to P2C and writing C2P without response.
//
// Threads: both managers run on one serial queue of their own. Callbacks only
// move bytes into pipes, update tables and signal; anything that may wait
// (handing an incoming link to the engine) goes to a global queue, and a
// pipe's sends wait on a condition from the engine's thread, never on the
// queue. Using CoreBluetooth asks the person once (NSBluetoothAlwaysUsage-
// Description, in the app's Info.plist); nothing touches it until the
// Sharepoint window opens.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "addons/sharepoint/bt_pipe.h"
#include "addons/sharepoint/presence.h"
#include "addons/sharepoint/wire.h"

namespace sp = mv::sharepoint;

namespace {

// Signals between the queue's callbacks and the engine's threads.
struct shared_state {
  std::mutex m;
  std::condition_variable cv;
  bool powered_known = false;  // both managers reported a state
  bool central_on = false, peripheral_on = false;
  std::string why;              // when not on
  bool ready_to_notify = true;  // the peripheral's queue has room
  std::uint64_t write_room = 0; // bumps on "ready to send write without response"
};

std::string text(NSString* s) { return s ? std::string(s.UTF8String ?: "") : std::string(); }

std::span<const std::uint8_t> bytes(NSData* d) {
  return {static_cast<const std::uint8_t*>(d.bytes), d.length};
}

NSData* data_of(std::span<const std::uint8_t> b) { return [NSData dataWithBytes:b.data() length:b.size()]; }

}  // namespace

// One outgoing link's state on the central side.
@interface MVSharepointOutgoing : NSObject
@property(nonatomic, strong) CBPeripheral* peripheral;
@property(nonatomic, strong) CBCharacteristic* c2p;
@property(nonatomic, strong) CBCharacteristic* p2c;
@end
@implementation MVSharepointOutgoing
@end

@interface MVSharepointBluetooth : NSObject <CBCentralManagerDelegate, CBPeripheralManagerDelegate, CBPeripheralDelegate>
- (instancetype)initWithId:(NSString*)ident name:(NSString*)name state:(std::shared_ptr<shared_state>)state
                      gate:(std::shared_ptr<sp::bt::callback_gate>)gate;
- (void)stop;
@property(nonatomic) dispatch_queue_t queue;
@property(nonatomic) sp::bluetooth::peer_fn onPeer;
@property(nonatomic) sp::bluetooth::link_fn onLink;
// The engine's thread: a link to `handle` (a peripheral's identifier).
- (std::shared_ptr<sp::bt::pipe>)connect:(NSString*)handle timeoutMs:(int)timeout;
@end

@implementation MVSharepointBluetooth {
  std::shared_ptr<shared_state> _s;
  std::shared_ptr<sp::bt::callback_gate> _gate;
  CBCentralManager* _central;
  CBPeripheralManager* _peripheralManager;
  CBMutableCharacteristic* _info;
  CBMutableCharacteristic* _c2p;
  CBMutableCharacteristic* _p2c;
  NSData* _infoValue;
  BOOL _serviceAdded;
  BOOL _stopped;
  // Peripheral role: subscribed centrals and their pipes.
  NSMutableDictionary<NSUUID*, CBCentral*>* _centrals;
  std::map<std::string, std::weak_ptr<sp::bt::pipe>> _incoming;
  // Central role: what each peripheral is, from INFO.
  NSMutableDictionary<NSUUID*, CBPeripheral*>* _known;
  NSMutableSet<NSUUID*>* _probing;
  std::map<std::string, std::pair<std::string, std::string>> _info_of;  // identifier -> (id, name)
  std::map<std::string, std::chrono::steady_clock::time_point> _reported;
  // Outgoing links by peripheral identifier.
  NSMutableDictionary<NSUUID*, MVSharepointOutgoing*>* _outgoing;
  std::map<std::string, std::weak_ptr<sp::bt::pipe>> _out_pipes;
  std::map<std::string, int> _out_state;  // 0 connecting, 1 ready, -1 failed
}

- (instancetype)initWithId:(NSString*)ident name:(NSString*)name state:(std::shared_ptr<shared_state>)state
                      gate:(std::shared_ptr<sp::bt::callback_gate>)gate {
  if ((self = [super init])) {
    _s = std::move(state);
    _gate = std::move(gate);
    _queue = dispatch_queue_create("mediaviewer.sharepoint.bluetooth", DISPATCH_QUEUE_SERIAL);
    _centrals = [NSMutableDictionary dictionary];
    _known = [NSMutableDictionary dictionary];
    _probing = [NSMutableSet set];
    _outgoing = [NSMutableDictionary dictionary];
    const std::string info = sp::bt::info_json(text(ident), text(name));
    _infoValue = [NSData dataWithBytes:info.data() length:info.size()];
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:_queue options:@{CBCentralManagerOptionShowPowerAlertKey : @NO}];
    _peripheralManager = [[CBPeripheralManager alloc] initWithDelegate:self queue:_queue options:nil];
  }
  return self;
}

- (void)stop {
  dispatch_sync(_queue, ^{
    self->_stopped = YES;
    if (self->_central.state == CBManagerStatePoweredOn) [self->_central stopScan];
    if (self->_peripheralManager.isAdvertising) [self->_peripheralManager stopAdvertising];
    [self->_peripheralManager removeAllServices];
    for (CBPeripheral* p in self->_known.allValues) [self->_central cancelPeripheralConnection:p];
    for (MVSharepointOutgoing* o in self->_outgoing.allValues) [self->_central cancelPeripheralConnection:o.peripheral];
    for (auto& [k, w] : self->_incoming) {
      if (auto p = w.lock()) p->closed_by_peer();
    }
    for (auto& [k, w] : self->_out_pipes) {
      if (auto p = w.lock()) p->closed_by_peer();
    }
    self->_incoming.clear();
    self->_out_pipes.clear();
    self->_central.delegate = nil;
    self->_peripheralManager.delegate = nil;
  });
  std::lock_guard lock(_s->m);
  _s->cv.notify_all();
}

// ---- power ------------------------------------------------------------------------------

- (void)noteState {
  std::lock_guard lock(_s->m);
  const CBManagerState c = _central.state, p = _peripheralManager.state;
  _s->central_on = c == CBManagerStatePoweredOn;
  _s->peripheral_on = p == CBManagerStatePoweredOn;
  _s->powered_known = c != CBManagerStateUnknown && c != CBManagerStateResetting && p != CBManagerStateUnknown &&
                      p != CBManagerStateResetting;
  if (c == CBManagerStateUnauthorized || p == CBManagerStateUnauthorized) {
    _s->why = "MediaViewer may not use Bluetooth. Allow it in System Settings, Privacy & Security, Bluetooth.";
  } else if (c == CBManagerStatePoweredOff || p == CBManagerStatePoweredOff) {
    _s->why = "Bluetooth is off.";
  } else if (c == CBManagerStateUnsupported || p == CBManagerStateUnsupported) {
    _s->why = "This Mac has no Bluetooth LE.";
  } else {
    _s->why.clear();
  }
  _s->cv.notify_all();
}

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
  [self noteState];
  if (_stopped || central.state != CBManagerStatePoweredOn) return;
  [central scanForPeripheralsWithServices:@[ [CBUUID UUIDWithString:@(sp::bt::kServiceUuid)] ]
                                  options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @YES}];
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)pm {
  [self noteState];
  if (_stopped || pm.state != CBManagerStatePoweredOn || _serviceAdded) return;
  _info = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:@(sp::bt::kInfoUuid)]
                                             properties:CBCharacteristicPropertyRead
                                                  value:_infoValue
                                            permissions:CBAttributePermissionsReadable];
  _c2p = [[CBMutableCharacteristic alloc]
      initWithType:[CBUUID UUIDWithString:@(sp::bt::kC2PUuid)]
        properties:CBCharacteristicPropertyWriteWithoutResponse | CBCharacteristicPropertyWrite
             value:nil
       permissions:CBAttributePermissionsWriteable];
  _p2c = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:@(sp::bt::kP2CUuid)]
                                            properties:CBCharacteristicPropertyNotify
                                                 value:nil
                                           permissions:CBAttributePermissionsReadable];
  CBMutableService* service = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:@(sp::bt::kServiceUuid)]
                                                             primary:YES];
  service.characteristics = @[ _info, _c2p, _p2c ];
  [pm addService:service];
  _serviceAdded = YES;
}

- (void)peripheralManager:(CBPeripheralManager*)pm didAddService:(CBService*)service error:(NSError*)error {
  if (error || _stopped) return;
  [pm startAdvertising:@{
    CBAdvertisementDataServiceUUIDsKey : @[ [CBUUID UUIDWithString:@(sp::bt::kServiceUuid)] ],
    CBAdvertisementDataLocalNameKey : @"MediaViewer",
  }];
}

// ---- peripheral role: incoming links ---------------------------------------------------------

- (void)peripheralManager:(CBPeripheralManager*)pm
                         central:(CBCentral*)central
    didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
  if (_stopped || ![characteristic.UUID isEqual:_p2c.UUID]) return;
  const std::string key = text(central.identifier.UUIDString);
  if (auto old = _incoming[key].lock()) old->closed_by_peer();
  _centrals[central.identifier] = central;
  auto s = _s;
  __weak MVSharepointBluetooth* weakSelf = self;
  CBPeripheralManager* manager = pm;
  CBMutableCharacteristic* p2c = _p2c;
  dispatch_queue_t queue = _queue;
  const std::size_t size = central.maximumUpdateValueLength;
  auto send = [s, manager, p2c, central, queue](std::span<const std::uint8_t> packet) -> bool {
    NSData* value = data_of(packet);
    for (int attempt = 0; attempt < 2000; ++attempt) {
      __block BOOL sent = NO;
      __block BOOL gone = NO;
      dispatch_sync(queue, ^{
        if (!manager.delegate) {
          gone = YES;
          return;
        }
        sent = [manager updateValue:value forCharacteristic:p2c onSubscribedCentrals:@[ central ]];
        if (!sent) {
          std::lock_guard lock(s->m);
          s->ready_to_notify = false;
        }
      });
      if (gone) return false;
      if (sent) return true;
      std::unique_lock lock(s->m);
      s->cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return s->ready_to_notify; });
    }
    return false;
  };
  auto closed = [weakSelf, queue, key]() {
    dispatch_async(queue, ^{
      MVSharepointBluetooth* me = weakSelf;
      if (!me) return;
      [me closeIncoming:key];
    });
  };
  auto pipe = std::make_shared<sp::bt::pipe>(size, send, closed);
  _incoming[key] = pipe;
  sp::bluetooth::link_fn onLink = _onLink;
  auto gate = _gate;
  // The engine may wait (it joins an old transfer): never on this queue. The
  // gate keeps a late handoff from reaching an engine that has stopped.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    gate->call([&] {
      if (onLink) onLink(std::unique_ptr<sp::link>(new sp::bt::pipe_ref(pipe)));
    });
  });
}

- (void)closeIncoming:(const std::string&)key {
  CBMutableCharacteristic* p2c = _p2c;
  for (CBCentral* c in _centrals.allValues) {
    if (text(c.identifier.UUIDString) == key && p2c) {
      const std::uint8_t close[1] = {sp::bt::kPacketClose};
      (void)[_peripheralManager updateValue:data_of(close) forCharacteristic:p2c onSubscribedCentrals:@[ c ]];
    }
  }
  _incoming.erase(key);
}

- (void)peripheralManager:(CBPeripheralManager*)pm
                             central:(CBCentral*)central
    didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
  const std::string key = text(central.identifier.UUIDString);
  if (auto p = _incoming[key].lock()) p->closed_by_peer();
  _incoming.erase(key);
  [_centrals removeObjectForKey:central.identifier];
}

- (void)peripheralManager:(CBPeripheralManager*)pm didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
  BOOL respond = NO;
  for (CBATTRequest* r in requests) {
    if (![r.characteristic.UUID isEqual:_c2p.UUID] || !r.value) continue;
    if (auto p = _incoming[text(r.central.identifier.UUIDString)].lock()) p->deliver(bytes(r.value));
    respond = YES;
  }
  // Only a write with response waits for one; answering the first answers all.
  if (respond && requests.count > 0) [pm respondToRequest:requests[0] withResult:CBATTErrorSuccess];
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager*)pm {
  std::lock_guard lock(_s->m);
  _s->ready_to_notify = true;
  _s->cv.notify_all();
}

// ---- central role: finding who is near ---------------------------------------------------------

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                     RSSI:(NSNumber*)RSSI {
  if (_stopped) return;
  const std::string key = text(peripheral.identifier.UUIDString);
  // Bounded, however many devices advertise around us: forget the old ones.
  if (_info_of.size() > 256 || _known.count > 256) {
    _info_of.clear();
    _reported.clear();
    for (NSUUID* k in _known.allKeys) {
      if (!_outgoing[k]) [_known removeObjectForKey:k];
    }
  }
  _known[peripheral.identifier] = peripheral;
  const auto info = _info_of.find(key);
  if (info != _info_of.end()) {
    const auto now = std::chrono::steady_clock::now();
    auto& last = _reported[key];
    if (now - last < std::chrono::seconds(1)) return;  // a report a second is plenty
    last = now;
    const sp::bt_peer peer{key, info->second.first, info->second.second};
    _gate->call([&] {
      if (_onPeer) _onPeer(peer);
    });
    return;
  }
  if ([_probing containsObject:peripheral.identifier] || _outgoing[peripheral.identifier]) return;
  [_probing addObject:peripheral.identifier];
  peripheral.delegate = self;
  [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
  peripheral.delegate = self;
  [peripheral discoverServices:@[ [CBUUID UUIDWithString:@(sp::bt::kServiceUuid)] ]];
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
  [_probing removeObject:peripheral.identifier];
  [self outgoing:peripheral failed:YES];
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                      error:(NSError*)error {
  [_probing removeObject:peripheral.identifier];
  [self outgoing:peripheral failed:YES];
}

- (void)outgoing:(CBPeripheral*)peripheral failed:(BOOL)failed {
  const std::string key = text(peripheral.identifier.UUIDString);
  if (auto p = _out_pipes[key].lock()) p->closed_by_peer();
  _out_pipes.erase(key);
  if (_outgoing[peripheral.identifier]) {
    [_outgoing removeObjectForKey:peripheral.identifier];
    std::lock_guard lock(_s->m);
    _out_state[key] = -1;
    _s->cv.notify_all();
  }
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
  CBUUID* svc = [CBUUID UUIDWithString:@(sp::bt::kServiceUuid)];
  for (CBService* s in peripheral.services) {
    if ([s.UUID isEqual:svc]) {
      [peripheral discoverCharacteristics:@[
        [CBUUID UUIDWithString:@(sp::bt::kInfoUuid)], [CBUUID UUIDWithString:@(sp::bt::kC2PUuid)],
        [CBUUID UUIDWithString:@(sp::bt::kP2CUuid)]
      ]
                               forService:s];
      return;
    }
  }
  [_central cancelPeripheralConnection:peripheral];
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverCharacteristicsForService:(CBService*)service error:(NSError*)error {
  CBCharacteristic *info = nil, *c2p = nil, *p2c = nil;
  for (CBCharacteristic* c in service.characteristics) {
    if ([c.UUID isEqual:[CBUUID UUIDWithString:@(sp::bt::kInfoUuid)]]) info = c;
    if ([c.UUID isEqual:[CBUUID UUIDWithString:@(sp::bt::kC2PUuid)]]) c2p = c;
    if ([c.UUID isEqual:[CBUUID UUIDWithString:@(sp::bt::kP2CUuid)]]) p2c = c;
  }
  MVSharepointOutgoing* out = _outgoing[peripheral.identifier];
  if (out) {
    if (!c2p || !p2c) {
      [_central cancelPeripheralConnection:peripheral];
      return;
    }
    out.c2p = c2p;
    out.p2c = p2c;
    [peripheral setNotifyValue:YES forCharacteristic:p2c];
    return;
  }
  if (info) {
    [peripheral readValueForCharacteristic:info];
  } else {
    [_central cancelPeripheralConnection:peripheral];
  }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
                                          error:(NSError*)error {
  MVSharepointOutgoing* out = _outgoing[peripheral.identifier];
  if (!out || ![characteristic.UUID isEqual:out.p2c.UUID]) return;
  const std::string key = text(peripheral.identifier.UUIDString);
  std::lock_guard lock(_s->m);
  _out_state[key] = error || !characteristic.isNotifying ? -1 : 1;
  _s->cv.notify_all();
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
                              error:(NSError*)error {
  if (error || !characteristic.value) return;
  const std::string key = text(peripheral.identifier.UUIDString);
  if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:@(sp::bt::kP2CUuid)]]) {
    if (auto p = _out_pipes[key].lock()) p->deliver(bytes(characteristic.value));
    return;
  }
  if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:@(sp::bt::kInfoUuid)]]) {
    std::string id, name;
    if (sp::bt::parse_info(bytes(characteristic.value), id, name)) {
      _info_of[key] = {id, name};
      _reported[key] = std::chrono::steady_clock::now();
      const sp::bt_peer peer{key, id, name};
      _gate->call([&] {
        if (_onPeer) _onPeer(peer);
      });
    }
    [_probing removeObject:peripheral.identifier];
    if (!_outgoing[peripheral.identifier]) [_central cancelPeripheralConnection:peripheral];
  }
}

- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral*)peripheral {
  std::lock_guard lock(_s->m);
  ++_s->write_room;
  _s->cv.notify_all();
}

// ---- central role: an outgoing link --------------------------------------------------------------

- (std::shared_ptr<sp::bt::pipe>)connect:(NSString*)handle timeoutMs:(int)timeout {
  NSUUID* ident = [[NSUUID alloc] initWithUUIDString:handle];
  if (!ident) return nullptr;
  const std::string key = text(handle);
  __block BOOL started = NO;
  dispatch_sync(_queue, ^{
    if (self->_stopped || self->_central.state != CBManagerStatePoweredOn) return;
    CBPeripheral* p = self->_known[ident];
    if (!p) p = [self->_central retrievePeripheralsWithIdentifiers:@[ ident ]].firstObject;
    if (!p) return;
    MVSharepointOutgoing* out = [MVSharepointOutgoing new];
    out.peripheral = p;
    self->_outgoing[ident] = out;
    {
      std::lock_guard lock(self->_s->m);
      self->_out_state[key] = 0;
    }
    p.delegate = self;
    if (p.state == CBPeripheralStateConnected) {
      [p discoverServices:@[ [CBUUID UUIDWithString:@(sp::bt::kServiceUuid)] ]];
    } else {
      [self->_central connectPeripheral:p options:nil];
    }
    started = YES;
  });
  if (!started) return nullptr;
  {
    std::unique_lock lock(_s->m);
    const bool done = _s->cv.wait_for(lock, std::chrono::milliseconds(timeout),
                                      [&] { return _out_state[key] != 0; });
    if (!done || _out_state[key] != 1) {
      lock.unlock();
      dispatch_sync(_queue, ^{
        MVSharepointOutgoing* out = self->_outgoing[ident];
        if (out) {
          [self->_outgoing removeObjectForKey:ident];
          [self->_central cancelPeripheralConnection:out.peripheral];
        }
      });
      return nullptr;
    }
  }
  __block MVSharepointOutgoing* out = nil;
  __block std::size_t size = 20;
  dispatch_sync(_queue, ^{
    out = self->_outgoing[ident];
    if (out) size = [out.peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
  });
  if (!out) return nullptr;
  auto s = _s;
  dispatch_queue_t queue = _queue;
  CBPeripheral* peripheral = out.peripheral;
  CBCharacteristic* c2p = out.c2p;
  auto send = [s, queue, peripheral, c2p](std::span<const std::uint8_t> packet) -> bool {
    NSData* value = data_of(packet);
    for (int attempt = 0; attempt < 2000; ++attempt) {
      __block BOOL sent = NO, gone = NO;
      std::uint64_t room = 0;
      {
        std::lock_guard lock(s->m);
        room = s->write_room;
      }
      dispatch_sync(queue, ^{
        if (peripheral.state != CBPeripheralStateConnected) {
          gone = YES;
          return;
        }
        if (peripheral.canSendWriteWithoutResponse) {
          [peripheral writeValue:value forCharacteristic:c2p type:CBCharacteristicWriteWithoutResponse];
          sent = YES;
        }
      });
      if (gone) return false;
      if (sent) return true;
      std::unique_lock lock(s->m);
      s->cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return s->write_room != room; });
    }
    return false;
  };
  __weak MVSharepointBluetooth* weakSelf = self;
  auto closed = [weakSelf, queue, ident, peripheral, c2p]() {
    dispatch_async(queue, ^{
      MVSharepointBluetooth* me = weakSelf;
      if (!me) return;
      if (peripheral.state == CBPeripheralStateConnected && peripheral.canSendWriteWithoutResponse) {
        const std::uint8_t close[1] = {sp::bt::kPacketClose};
        [peripheral writeValue:data_of(close) forCharacteristic:c2p type:CBCharacteristicWriteWithoutResponse];
      }
      [me dropOutgoing:ident];
    });
  };
  auto pipe = std::make_shared<sp::bt::pipe>(size, send, closed);
  dispatch_sync(_queue, ^{
    self->_out_pipes[key] = pipe;
  });
  return pipe;
}

- (void)dropOutgoing:(NSUUID*)ident {
  MVSharepointOutgoing* out = _outgoing[ident];
  if (!out) return;
  [_outgoing removeObjectForKey:ident];
  _out_pipes.erase(text(ident.UUIDString));
  [_central cancelPeripheralConnection:out.peripheral];
}

@end

namespace mv::sharepoint {


namespace {

class mac_bluetooth final : public bluetooth {
 public:
  ~mac_bluetooth() override { stop(); }

  bool start(const std::string& id, const std::string& name, peer_fn on_peer, link_fn on_link,
             std::string& why) override {
    stop();
    state_ = std::make_shared<shared_state>();
    gate_ = std::make_shared<bt::callback_gate>();
    gate_->open();
    @autoreleasepool {
      MVSharepointBluetooth* impl =
          [[MVSharepointBluetooth alloc] initWithId:@(id.c_str()) name:@(name.c_str()) state:state_ gate:gate_];
      impl.onPeer = std::move(on_peer);
      impl.onLink = std::move(on_link);
      std::lock_guard lock(m_);
      impl_ = impl;
    }
    // The managers report their state within moments (the first time,
    // after the person answers the Bluetooth question).
    std::unique_lock lock(state_->m);
    state_->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state_->powered_known; });
    if (!state_->powered_known) return true;  // still deciding: it starts when allowed
    if (!state_->central_on || !state_->peripheral_on) {
      why = state_->why.empty() ? std::string("Bluetooth is off.") : state_->why;
      lock.unlock();
      stop();
      return false;
    }
    return true;
  }

  void stop() noexcept override {
    MVSharepointBluetooth* impl = nil;
    {
      std::lock_guard lock(m_);
      impl = impl_;
      impl_ = nil;
    }
    if (!impl) return;
    gate_->close();  // no call into the engine after this
    @autoreleasepool {
      [impl stop];
    }
  }

  result<std::unique_ptr<link>> connect(const std::string& handle, int timeout_ms) override {
    MVSharepointBluetooth* impl = nil;  // strong: a stop meanwhile cannot free it under the call
    {
      std::lock_guard lock(m_);
      impl = impl_;
    }
    if (!impl) return err(status::not_found);
    std::shared_ptr<bt::pipe> p;
    @autoreleasepool {
      p = [impl connect:@(handle.c_str()) timeoutMs:timeout_ms];
    }
    if (!p) return err(status::io);
    return std::unique_ptr<link>(new bt::pipe_ref(std::move(p)));
  }

 private:
  std::shared_ptr<shared_state> state_;
  std::shared_ptr<bt::callback_gate> gate_;
  std::mutex m_;
  MVSharepointBluetooth* impl_ = nil;  // under m_
};

}  // namespace

std::unique_ptr<bluetooth> make_bluetooth() { return std::make_unique<mac_bluetooth>(); }

}  // namespace mv::sharepoint

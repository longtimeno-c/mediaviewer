// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Noise Protocol Framework (revision 34), as Sharepoint's protocol 2 uses
// it (docs/design/28 "The wire", issue #472): 25519, ChaChaPoly, BLAKE2b, and
// the hybrid forward secrecy modifier ("hfs") with ML-KEM-768 as its KEM.
//
//   XXhfs        the first link of a transfer:
//                  -> e, e1
//                  <- e, ee, ekem1, s, es
//                  -> s, se
//   NNhfs+psk0   a rejoin over any bearer, keyed by the resume secret of the
//                link before it:
//                  -> psk, e, e1
//                  <- e, ee, ekem1
//
// "hfs": e1 is an ML-KEM-768 key made for this handshake alone, ekem1 the
// ciphertext encapsulated to it, and its shared secret is mixed into the
// chaining key beside the X25519 one. Breaking a recorded session needs both
// X25519 and ML-KEM-768 broken. XX, NN and NNpsk0 (no hfs) are here for the
// published test vectors (Cacophony), which check this code against others.
//
// Every secret lives in this object and is wiped (sodium_memzero) when it
// goes. Portable (D9): libsodium only.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::sharepoint::noise {

inline constexpr std::size_t kHashBytes = 64;  // BLAKE2b
inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kDhBytes = 32;
inline constexpr std::size_t kTagBytes = 16;
inline constexpr std::size_t kKemPublicBytes = 1184;  // ML-KEM-768
inline constexpr std::size_t kKemCiphertextBytes = 1088;
inline constexpr std::size_t kMaxMessageBytes = 65535;

enum class token : std::uint8_t { e, s, ee, es, se, ss, psk, e1, ekem1 };

struct pattern {
  const char* name;  // the protocol name hashed into h
  std::span<const std::span<const token>> messages;
  bool psk = false;  // "e" also mixes the key (the psk modifier)
};

[[nodiscard]] const pattern& xx();           // Noise_XX_25519_ChaChaPoly_BLAKE2b
[[nodiscard]] const pattern& nn_psk0();      // Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b
[[nodiscard]] const pattern& xx_hfs();       // Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b
[[nodiscard]] const pattern& nn_hfs_psk0();  // Noise_NNhfs+psk0_25519+MLKEM768_ChaChaPoly_BLAKE2b

// Noise's CipherState: ChaChaPoly (IETF), the nonce a 64-bit counter.
class cipher_state {
 public:
  ~cipher_state() { clear(); }
  void init(std::span<const std::uint8_t, kKeyBytes> k) noexcept;
  void clear() noexcept;
  [[nodiscard]] bool has_key() const noexcept { return has_key_; }
  // out = ciphertext and tag (or the plaintext when there is no key yet).
  [[nodiscard]] expected encrypt(std::span<const std::uint8_t> ad, std::span<const std::uint8_t> plain,
                                 std::vector<std::uint8_t>& out);
  [[nodiscard]] expected decrypt(std::span<const std::uint8_t> ad, std::span<const std::uint8_t> sealed,
                                 std::vector<std::uint8_t>& out);

 private:
  std::array<std::uint8_t, kKeyBytes> k_{};
  std::uint64_t n_ = 0;
  bool has_key_ = false;
};

// What a finished handshake hands on: one key per direction, the handshake
// hash (channel binding), and a third secret from the same HKDF as the two
// keys (Split's third output), which protocol 2 keeps for the next rejoin.
struct split_keys {
  std::array<std::uint8_t, kKeyBytes> initiator_to_responder{};
  std::array<std::uint8_t, kKeyBytes> responder_to_initiator{};
  std::array<std::uint8_t, kKeyBytes> extra{};
  std::array<std::uint8_t, kHashBytes> h{};
  ~split_keys();
};

// Fixed keys instead of fresh random ones: the test vectors only.
struct test_keys {
  std::optional<std::array<std::uint8_t, 32>> e;         // X25519 private
  std::optional<std::array<std::uint8_t, 64>> e1_seed;   // ML-KEM-768 key seed
  std::optional<std::array<std::uint8_t, 32>> kem_seed;  // ML-KEM-768 encapsulation seed
};

class handshake {
 public:
  // `s`: this side's X25519 private key, for a pattern with s. `psk`: for a
  // pattern with psk.
  handshake(const pattern& p, bool initiator, std::span<const std::uint8_t> prologue,
            std::optional<std::array<std::uint8_t, 32>> s = std::nullopt,
            std::optional<std::array<std::uint8_t, 32>> psk = std::nullopt, test_keys fixed = {});
  ~handshake();
  handshake(const handshake&) = delete;
  handshake& operator=(const handshake&) = delete;

  // The next message, with `payload` (encrypted once there is a key).
  // status::invalid_arg when it is not this side's turn.
  [[nodiscard]] expected write_message(std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out);
  // The peer's next message; `payload` its payload. status::corrupt for a
  // message that is the wrong length, does not open, or carries a key that is
  // not a usable point.
  [[nodiscard]] expected read_message(std::span<const std::uint8_t> message, std::vector<std::uint8_t>& payload);

  [[nodiscard]] bool done() const noexcept;
  // h now (after the last message read or written).
  [[nodiscard]] std::array<std::uint8_t, kHashBytes> hash() const noexcept;
  // The peer's static key, once its "s" arrived.
  [[nodiscard]] std::optional<std::array<std::uint8_t, 32>> remote_static() const;
  // Once done(): the transport keys. Ends the handshake (its secrets wiped).
  [[nodiscard]] result<split_keys> split();

 private:
  struct state;
  std::unique_ptr<state> st_;
};

// HMAC-BLAKE2b and Noise's HKDF over it (exposed for the tests).
void hmac(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data,
          std::span<std::uint8_t, kHashBytes> out) noexcept;
void hkdf(std::span<const std::uint8_t, kHashBytes> chaining_key, std::span<const std::uint8_t> ikm,
          std::span<std::uint8_t, kHashBytes> out1, std::span<std::uint8_t, kHashBytes> out2,
          std::span<std::uint8_t, kHashBytes> out3) noexcept;

}  // namespace mv::sharepoint::noise

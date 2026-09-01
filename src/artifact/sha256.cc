// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cloth {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t read_big_endian(std::span<const std::uint8_t, 4> bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

void write_big_endian(std::uint32_t value, std::uint8_t* output) {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

int lowercase_hex_value(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  return -1;
}

}  // namespace

namespace artifact_internal {

void Sha256Hasher::process_block(std::span<const std::uint8_t, 64> block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = read_big_endian(
        std::span<const std::uint8_t, 4>{block.data() + index * 4, 4});
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const std::uint32_t first = std::rotr(words[index - 15], 7) ^
                                std::rotr(words[index - 15], 18) ^
                                (words[index - 15] >> 3U);
    const std::uint32_t second = std::rotr(words[index - 2], 17) ^
                                 std::rotr(words[index - 2], 19) ^
                                 (words[index - 2] >> 10U);
    words[index] = words[index - 16] + first + words[index - 7] + second;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const std::uint32_t upper_e =
        std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t first =
        h + upper_e + choice + kRoundConstants[index] + words[index];
    const std::uint32_t upper_a =
        std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t second = upper_a + majority;
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256Hasher::update(std::span<const std::uint8_t> bytes) {
  if (finished_ || bytes.empty()) return;
  byte_count_ += static_cast<std::uint64_t>(bytes.size());
  while (!bytes.empty()) {
    const std::size_t count =
        std::min(bytes.size(), pending_.size() - pending_size_);
    std::ranges::copy(bytes.first(count), std::span<std::uint8_t>{pending_}
                                              .subspan(pending_size_, count)
                                              .begin());
    pending_size_ += count;
    bytes = bytes.subspan(count);
    if (pending_size_ == pending_.size()) {
      process_block(std::span<const std::uint8_t, 64>{pending_});
      pending_size_ = 0;
    }
  }
}

ArtifactDigest Sha256Hasher::finish() {
  if (!finished_) {
    const std::uint64_t bit_count = byte_count_ * 8U;
    pending_[pending_size_++] = 0x80U;
    if (pending_size_ > 56) {
      std::ranges::fill(
          std::span<std::uint8_t>{pending_}.subspan(pending_size_), 0);
      process_block(std::span<const std::uint8_t, 64>{pending_});
      pending_size_ = 0;
    }
    std::ranges::fill(std::span<std::uint8_t>{pending_}.subspan(
                          pending_size_, 56 - pending_size_),
                      0);
    for (std::size_t index = 0; index < 8; ++index) {
      pending_[56 + index] = static_cast<std::uint8_t>(
          bit_count >> static_cast<unsigned int>((7 - index) * 8));
    }
    process_block(std::span<const std::uint8_t, 64>{pending_});
    finished_ = true;
  }
  ArtifactDigest digest;
  for (std::size_t index = 0; index < state_.size(); ++index) {
    write_big_endian(state_[index], digest.bytes.data() + index * 4);
  }
  return digest;
}

}  // namespace artifact_internal

ArtifactDigest sha256(std::span<const std::uint8_t> bytes) {
  artifact_internal::Sha256Hasher hasher;
  hasher.update(bytes);
  return hasher.finish();
}

ArtifactDigest sha256(std::string_view bytes) {
  return sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

std::string artifact_digest_hex(const ArtifactDigest& digest) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result;
  result.reserve(digest.bytes.size() * 2);
  for (const std::uint8_t byte : digest.bytes) {
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

std::optional<ArtifactDigest> parse_artifact_digest(std::string_view text) {
  ArtifactDigest result;
  if (text.size() != result.bytes.size() * 2) return std::nullopt;
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    const int upper = lowercase_hex_value(text[index * 2]);
    const int lower = lowercase_hex_value(text[index * 2 + 1]);
    if (upper < 0 || lower < 0) return std::nullopt;
    result.bytes[index] = static_cast<std::uint8_t>((upper << 4) | lower);
  }
  return result;
}

}  // namespace cloth

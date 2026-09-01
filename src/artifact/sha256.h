/**
 * @Author: Your name
 * @Date:   2026-08-31 23:41:36
 * @Last Modified by:   Your name
 * @Last Modified time: 2026-09-01 00:08:03
 */
// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SRC_ARTIFACT_SHA256_H_
#define CLOTH_SRC_ARTIFACT_SHA256_H_

#include "cloth/artifact/package_artifact.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cloth::artifact_internal {

class Sha256Hasher {
 public:
  void update(std::span<const std::uint8_t> bytes);
  [[nodiscard]] ArtifactDigest finish();

 private:
  void process_block(std::span<const std::uint8_t, 64> block);

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> pending_{};
  std::size_t pending_size_{0};
  std::uint64_t byte_count_{0};
  bool finished_{false};
};

}  // namespace cloth::artifact_internal

#endif  // CLOTH_SRC_ARTIFACT_SHA256_H_

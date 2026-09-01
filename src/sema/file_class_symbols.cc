// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/file_class_symbols.h"

#include <utility>

namespace cloth {

FileClassSymbols::FileClassSymbols(std::string name, Visibility visibility,
                                   SourceRange range)
    : name_(std::move(name)), visibility_(visibility), range_(range) {}

std::size_t FileClassSymbols::add(MemberSymbol symbol) {
  const std::size_t index = members_.size();
  members_.push_back(std::move(symbol));
  return index;
}

const std::string& FileClassSymbols::name() const noexcept { return name_; }

Visibility FileClassSymbols::visibility() const noexcept { return visibility_; }

SourceRange FileClassSymbols::range() const noexcept { return range_; }

std::span<const MemberSymbol> FileClassSymbols::members() const noexcept {
  return members_;
}

const MemberSymbol& FileClassSymbols::member(std::size_t index) const {
  return members_.at(index);
}

}  // namespace cloth

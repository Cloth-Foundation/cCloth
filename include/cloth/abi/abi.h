// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_ABI_ABI_H_
#define CLOTH_ABI_ABI_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/target/data_layout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cloth {

enum class AbiTypeKind {
  kInvalid,
  kVoid,
  kInteger,
  kFloat,
  kReference,
  kAggregate,
};

struct AbiTypeLayout {
  TypeId type;
  AbiTypeKind kind;
  std::uint32_t bit_width;
  SizeAlignment storage;
  std::vector<std::uint64_t> reference_offsets{};

  friend bool operator==(const AbiTypeLayout&, const AbiTypeLayout&) = default;
};

struct AbiFieldLayout {
  SymbolId symbol;
  TypeId type;
  std::uint64_t offset;

  friend bool operator==(const AbiFieldLayout&,
                         const AbiFieldLayout&) = default;
};

struct AbiClassLayout {
  std::uint64_t header_size;
  std::uint64_t size;
  std::uint64_t alignment;
  std::vector<AbiFieldLayout> fields;

  friend bool operator==(const AbiClassLayout&,
                         const AbiClassLayout&) = default;
};

enum class AbiHeapObjectKind : std::uint64_t {
  kFileClass = 0,
  kString = 1,
  kArray = 2,
};

struct AbiTypeDescriptor {
  AbiHeapObjectKind kind;
  std::optional<FileId> parent_file;
  std::string name;
  std::uint64_t size;
  std::uint64_t alignment;
  std::vector<std::uint64_t> reference_offsets;
  std::vector<SymbolId> virtual_functions;
  struct InterfaceDispatch {
    FileId interface_file;
    std::uint64_t interface_id;
    std::vector<SymbolId> functions;

    friend bool operator==(const InterfaceDispatch&,
                           const InterfaceDispatch&) = default;
  };
  std::vector<InterfaceDispatch> interfaces{};
  std::string mangled_name{};

  friend bool operator==(const AbiTypeDescriptor&,
                         const AbiTypeDescriptor&) = default;
};

enum class AbiCallableKind {
  kFunction,
  kConstructor,
};

enum class AbiLinkage {
  kInternal,
  kExternal,
};

struct AbiStaticField {
  SymbolId symbol;
  TypeId type;
  AbiLinkage linkage;
  std::string mangled_name;

  friend bool operator==(const AbiStaticField&,
                         const AbiStaticField&) = default;
};

enum class AbiCallingConvention {
  kC,
};

enum class AbiParameterKind {
  kResult,
  kReceiver,
  kExplicit,
};

enum class AbiPassingMode { kDirect, kValuePointer, kResultPointer };
enum class AbiReturnMode { kVoid, kDirect, kIndirect };
enum class AbiReceiverMode { kNone, kReference, kReadOnlyValue, kConstruction };

[[nodiscard]] std::string_view abi_passing_mode_name(AbiPassingMode mode);
[[nodiscard]] std::string_view abi_return_mode_name(AbiReturnMode mode);
[[nodiscard]] std::string_view abi_receiver_mode_name(AbiReceiverMode mode);

struct AbiParameter {
  AbiParameterKind kind;
  std::optional<SymbolId> symbol;
  TypeId type;
  AbiPassingMode passing{AbiPassingMode::kDirect};

  friend bool operator==(const AbiParameter&, const AbiParameter&) = default;
};

struct AbiCallable {
  SymbolId symbol;
  AbiCallableKind kind;
  AbiLinkage linkage;
  AbiCallingConvention calling_convention;
  std::string mangled_name;
  std::string initializer_mangled_name;
  TypeId return_type;
  std::vector<AbiParameter> parameters;
  AbiLinkage initializer_linkage{AbiLinkage::kInternal};
  AbiReturnMode return_mode{AbiReturnMode::kDirect};
  AbiReceiverMode receiver_mode{AbiReceiverMode::kNone};

  friend bool operator==(const AbiCallable&, const AbiCallable&) = default;
};

struct AbiFileClass {
  FileId file;
  SymbolId symbol;
  std::optional<FileId> base_file;
  AbiClassLayout layout;
  std::optional<AbiTypeDescriptor> type_descriptor;
  std::vector<AbiStaticField> static_fields;
  std::vector<AbiCallable> functions;
  std::vector<AbiCallable> constructors;
  std::vector<MemberReference> member_order;
  FileTypeKind kind{FileTypeKind::kClass};

  friend bool operator==(const AbiFileClass&, const AbiFileClass&) = default;
};

struct AbiModule {
  TargetDataLayout target;
  std::vector<AbiTypeLayout> types;
  std::vector<AbiFileClass> files;

  friend bool operator==(const AbiModule&, const AbiModule&) = default;
};

[[nodiscard]] std::optional<AbiModule> lower_to_abi(
    const MirModule& mir, const SemanticModel& semantics,
    TargetDataLayout target, DiagnosticEngine& diagnostics);

[[nodiscard]] std::string mangle_abi_symbol(const SemanticSymbol& symbol,
                                            const SemanticModel& semantics);

[[nodiscard]] std::string mangle_abi_constructor_initializer(
    const SemanticSymbol& symbol, const SemanticModel& semantics);

[[nodiscard]] std::string mangle_abi_static_field(
    const SemanticSymbol& symbol, const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_ABI_ABI_H_

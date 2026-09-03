// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_ARTIFACT_IMPORTED_PACKAGE_H_
#define CLOTH_ARTIFACT_IMPORTED_PACKAGE_H_

#include "cloth/abi/abi.h"
#include "cloth/identity/canonical_identity.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/sema/visibility.h"
#include "cloth/target/data_layout.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cloth {

// This is an owned compiler record despite the "view" name: it is the
// declaration and ABI view presented to a package consumer. It intentionally
// contains no syntax tree, executable body, or process-local compiler ID.
struct ImportedSourceLocation {
  std::string path;
  std::uint64_t line;
  std::uint64_t column;

  friend bool operator==(const ImportedSourceLocation&,
                         const ImportedSourceLocation&) = default;
};

struct ImportedType {
  std::string identity;
  TypeKind kind;
  std::string display_name;
  std::optional<std::string> element_identity;
  std::optional<NominalIdentity> nominal_identity;
  AbiTypeKind abi_kind;
  std::uint32_t bit_width;
  SizeAlignment storage;
  std::vector<std::uint64_t> reference_offsets;

  friend bool operator==(const ImportedType&, const ImportedType&) = default;
};

struct ImportedParameter {
  std::string name;
  std::string type_identity;
  bool is_final;

  friend bool operator==(const ImportedParameter&,
                         const ImportedParameter&) = default;
};

enum class ImportedMemberKind {
  kField,
  kFunction,
  kConstructor,
};

// The member's canonical type identity supplies nominal ownership; kind and
// zero-extended bits preserve the scalar representation without source text.
struct ImportedScalarConstant {
  TypeKind kind;
  std::uint64_t bits;

  friend bool operator==(const ImportedScalarConstant&,
                         const ImportedScalarConstant&) = default;
};

struct ImportedMember {
  std::string identity;
  std::string owner_identity;
  std::string name;
  ImportedMemberKind kind;
  Visibility visibility;
  std::string type_identity;
  std::vector<ImportedParameter> parameters;
  ImportedSourceLocation location;
  bool is_final;
  bool is_static;
  bool is_override;
  bool is_abstract;
  std::optional<std::uint64_t> virtual_slot;
  std::optional<std::string> overridden_identity;
  std::optional<std::string> base_constructor_identity;
  std::optional<ImportedScalarConstant> static_value;

  friend bool operator==(const ImportedMember&,
                         const ImportedMember&) = default;
};

struct ImportedInterfaceImplementation {
  std::string interface_identity;
  std::vector<std::string> function_identities;

  friend bool operator==(const ImportedInterfaceImplementation&,
                         const ImportedInterfaceImplementation&) = default;
};

struct ImportedFieldLayout {
  std::string field_identity;
  std::string type_identity;
  std::uint64_t offset;

  friend bool operator==(const ImportedFieldLayout&,
                         const ImportedFieldLayout&) = default;
};

struct ImportedInterfaceDispatch {
  std::string interface_identity;
  std::uint64_t interface_id;
  std::vector<std::string> function_identities;

  friend bool operator==(const ImportedInterfaceDispatch&,
                         const ImportedInterfaceDispatch&) = default;
};

struct ImportedTypeDescriptor {
  AbiHeapObjectKind kind;
  std::string identity;
  std::optional<std::string> parent_identity;
  std::string display_name;
  std::uint64_t size;
  std::uint64_t alignment;
  std::vector<std::uint64_t> reference_offsets;
  std::vector<std::string> virtual_function_identities;
  std::vector<ImportedInterfaceDispatch> interfaces;
  std::string mangled_name;

  friend bool operator==(const ImportedTypeDescriptor&,
                         const ImportedTypeDescriptor&) = default;
};

struct ImportedStaticFieldAbi {
  std::string member_identity;
  std::string type_identity;
  AbiLinkage linkage;
  std::string mangled_name;

  friend bool operator==(const ImportedStaticFieldAbi&,
                         const ImportedStaticFieldAbi&) = default;
};

struct ImportedAbiParameter {
  AbiParameterKind kind;
  std::string type_identity;
  AbiPassingMode passing = AbiPassingMode::kDirect;

  friend bool operator==(const ImportedAbiParameter&,
                         const ImportedAbiParameter&) = default;
};

struct ImportedCallableAbi {
  std::string member_identity;
  AbiCallableKind kind;
  AbiLinkage linkage;
  AbiCallingConvention calling_convention;
  std::string mangled_name;
  std::optional<std::string> initializer_identity;
  std::string initializer_mangled_name;
  AbiLinkage initializer_linkage;
  std::optional<std::string> initializer_return_type_identity;
  std::vector<ImportedAbiParameter> initializer_parameters;
  std::string return_type_identity;
  std::vector<ImportedAbiParameter> parameters;
  AbiReturnMode return_mode = AbiReturnMode::kDirect;
  AbiReceiverMode receiver_mode = AbiReceiverMode::kNone;
  AbiReturnMode initializer_return_mode = AbiReturnMode::kVoid;
  AbiReceiverMode initializer_receiver_mode = AbiReceiverMode::kNone;

  friend bool operator==(const ImportedCallableAbi&,
                         const ImportedCallableAbi&) = default;
};

struct ImportedClassAbi {
  std::uint64_t header_size;
  std::uint64_t size;
  std::uint64_t alignment;
  std::vector<ImportedFieldLayout> fields;
  std::optional<ImportedTypeDescriptor> descriptor;
  std::vector<ImportedStaticFieldAbi> static_fields;
  std::vector<ImportedCallableAbi> callables;

  friend bool operator==(const ImportedClassAbi&,
                         const ImportedClassAbi&) = default;
};

struct ImportedEnumCase {
  std::string identity;
  std::string name;
  std::uint32_t tag;
  ImportedSourceLocation location;

  friend bool operator==(const ImportedEnumCase&,
                         const ImportedEnumCase&) = default;
};

struct ImportedFile {
  NominalIdentity nominal_identity;
  std::string identity;
  std::string logical_path;
  ImportedSourceLocation location;
  Visibility visibility;
  FileTypeKind kind;
  bool is_abstract;
  bool is_sealed;
  std::optional<std::string> base_identity;
  std::vector<std::string> direct_interface_identities;
  std::vector<std::string> interface_identities;
  std::optional<std::uint64_t> interface_id;
  std::vector<ImportedMember> members;
  std::vector<std::string> member_order;
  std::vector<std::string> virtual_function_identities;
  std::vector<std::string> abstract_function_identities;
  std::vector<std::string> interface_function_identities;
  std::vector<ImportedInterfaceImplementation> interface_implementations;
  ImportedClassAbi abi;
  std::vector<ImportedEnumCase> enum_cases{};

  friend bool operator==(const ImportedFile&, const ImportedFile&) = default;
};

struct ImportedPackageView {
  PackageIdentity package;
  TargetDataLayout target;
  std::vector<ImportedType> types;
  std::vector<ImportedFile> files;

  friend bool operator==(const ImportedPackageView&,
                         const ImportedPackageView&) = default;
};

enum class ImportedPackageIssueCode {
  kInvalidModel,
  kLimitExceeded,
};

struct ImportedPackageIssue {
  std::string record;
  std::string message;
  ImportedPackageIssueCode code = ImportedPackageIssueCode::kInvalidModel;

  friend bool operator==(const ImportedPackageIssue&,
                         const ImportedPackageIssue&) = default;
};

struct ImportedPackageResult {
  std::optional<ImportedPackageView> view;
  std::vector<ImportedPackageIssue> issues;

  [[nodiscard]] bool is_valid() const noexcept {
    return view.has_value() && issues.empty();
  }
};

// Inputs must have passed semantic, MIR, and ABI verification. The result is
// reverified after local IDs and source-backed strings have been detached.
[[nodiscard]] ImportedPackageResult build_imported_package_view(
    const PackageIdentity& package, const SemanticModel& semantics,
    const MirModule& mir, const AbiModule& abi);

[[nodiscard]] std::vector<ImportedPackageIssue> verify_imported_package_view(
    const ImportedPackageView& view);

// Checks dependency-owned type claims and reconstructs layouts before
// source-free declarations may enter semantic analysis or a native link.
[[nodiscard]] std::vector<ImportedPackageIssue> verify_imported_package_closure(
    std::span<const ImportedPackageView* const> packages);

[[nodiscard]] std::string imported_callable_signature(
    AbiReturnMode return_mode, AbiReceiverMode receiver_mode,
    std::string_view return_type,
    std::span<const ImportedAbiParameter> parameters);

}  // namespace cloth

#endif  // CLOTH_ARTIFACT_IMPORTED_PACKAGE_H_

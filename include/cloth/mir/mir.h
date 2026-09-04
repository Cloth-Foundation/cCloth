// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_MIR_MIR_H_
#define CLOTH_MIR_MIR_H_

#include "cloth/ast/ast.h"
#include "cloth/hir/hir.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cloth {

struct MirValueId {
  std::size_t value;

  friend bool operator==(const MirValueId&, const MirValueId&) = default;
};

struct MirBlockId {
  std::size_t value;

  friend bool operator==(const MirBlockId&, const MirBlockId&) = default;
};

struct MirInvalidInstruction {
  friend bool operator==(const MirInvalidInstruction&,
                         const MirInvalidInstruction&) = default;
};

struct MirLiteralInstruction {
  LiteralKind kind;
  std::string lexeme;

  friend bool operator==(const MirLiteralInstruction&,
                         const MirLiteralInstruction&) = default;
};

// A target-independent scalar value with canonical bits. Unlike a source
// literal, this instruction does not retain or synthesize source spelling.
struct MirScalarConstantInstruction {
  ScalarConstant value;

  friend bool operator==(const MirScalarConstantInstruction&,
                         const MirScalarConstantInstruction&) = default;
};

struct MirLoadSymbolInstruction {
  SymbolId symbol;

  friend bool operator==(const MirLoadSymbolInstruction&,
                         const MirLoadSymbolInstruction&) = default;
};

struct MirDeclareLocalInstruction {
  SymbolId symbol;
  std::optional<MirValueId> initializer;

  friend bool operator==(const MirDeclareLocalInstruction&,
                         const MirDeclareLocalInstruction&) = default;
};

struct MirStoreSymbolInstruction {
  SymbolId symbol;
  MirValueId value;

  friend bool operator==(const MirStoreSymbolInstruction&,
                         const MirStoreSymbolInstruction&) = default;
};

struct MirLoadMemberInstruction {
  MirValueId object;
  SymbolId member;

  friend bool operator==(const MirLoadMemberInstruction&,
                         const MirLoadMemberInstruction&) = default;
};

struct MirStoreMemberInstruction {
  MirValueId object;
  SymbolId member;
  MirValueId value;

  friend bool operator==(const MirStoreMemberInstruction&,
                         const MirStoreMemberInstruction&) = default;
};

// A compiler-only storage recipe, never a Cloth value or escaping pointer.
// Exactly one root is present. An index selects an element of an object root;
// subsequent fields project through inline struct storage only.
struct MirStoragePath {
  std::optional<SymbolId> symbol;
  std::optional<MirValueId> object;
  std::optional<MirValueId> index;
  std::vector<SymbolId> fields;

  friend bool operator==(const MirStoragePath&,
                         const MirStoragePath&) = default;
};

struct MirLoadStorageInstruction {
  MirStoragePath path;

  friend bool operator==(const MirLoadStorageInstruction&,
                         const MirLoadStorageInstruction&) = default;
};

struct MirStoreStorageInstruction {
  MirStoragePath path;
  MirValueId value;

  friend bool operator==(const MirStoreStorageInstruction&,
                         const MirStoreStorageInstruction&) = default;
};

struct MirArrayLiteralInstruction {
  TypeId element_type;
  std::vector<MirValueId> elements;

  friend bool operator==(const MirArrayLiteralInstruction&,
                         const MirArrayLiteralInstruction&) = default;
};

struct MirArrayLoadInstruction {
  MirValueId array;
  MirValueId index;

  friend bool operator==(const MirArrayLoadInstruction&,
                         const MirArrayLoadInstruction&) = default;
};

struct MirArrayStoreInstruction {
  MirValueId array;
  MirValueId index;
  MirValueId value;

  friend bool operator==(const MirArrayStoreInstruction&,
                         const MirArrayStoreInstruction&) = default;
};

struct MirArrayLengthInstruction {
  MirValueId array;

  friend bool operator==(const MirArrayLengthInstruction&,
                         const MirArrayLengthInstruction&) = default;
};

struct MirStringMetaInstruction {
  MirValueId string;
  StringMetaQuery query;

  friend bool operator==(const MirStringMetaInstruction&,
                         const MirStringMetaInstruction&) = default;
};

struct MirObjectMetaInstruction {
  MirValueId object;

  friend bool operator==(const MirObjectMetaInstruction&,
                         const MirObjectMetaInstruction&) = default;
};

struct MirIntegerWriteInstruction {
  MirValueId value;
  MirValueId destination;
  MirValueId offset;
  IntegerByteOrder byte_order;

  friend bool operator==(const MirIntegerWriteInstruction&,
                         const MirIntegerWriteInstruction&) = default;
};

struct MirIntegerReadInstruction {
  MirValueId source;
  MirValueId offset;
  IntegerByteOrder byte_order;

  friend bool operator==(const MirIntegerReadInstruction&,
                         const MirIntegerReadInstruction&) = default;
};

struct MirUnaryInstruction {
  TokenKind operation;
  MirValueId operand;

  friend bool operator==(const MirUnaryInstruction&,
                         const MirUnaryInstruction&) = default;
};

struct MirBinaryInstruction {
  MirValueId left;
  TokenKind operation;
  MirValueId right;

  friend bool operator==(const MirBinaryInstruction&,
                         const MirBinaryInstruction&) = default;
};

enum class MirConversionKind {
  kWidenNumeric,
  kCheckedNumeric,
  kWrapInteger,
  kSaturateInteger,
  kWidenReference,
  kToNullable,
  kFromNullable,
};

struct MirConvertInstruction {
  MirValueId value;
  MirConversionKind kind;

  friend bool operator==(const MirConvertInstruction&,
                         const MirConvertInstruction&) = default;
};

struct MirIsNonNullInstruction {
  MirValueId value;

  friend bool operator==(const MirIsNonNullInstruction&,
                         const MirIsNonNullInstruction&) = default;
};

struct MirNullAssertInstruction {
  MirValueId value;

  friend bool operator==(const MirNullAssertInstruction&,
                         const MirNullAssertInstruction&) = default;
};

struct MirTypeTestInstruction {
  MirValueId value;
  TypeId target;

  friend bool operator==(const MirTypeTestInstruction&,
                         const MirTypeTestInstruction&) = default;
};

struct MirCheckedCastInstruction {
  MirValueId value;
  TypeId target;

  friend bool operator==(const MirCheckedCastInstruction&,
                         const MirCheckedCastInstruction&) = default;
};

enum class MirCallKind {
  kUnqualified,
  kClassQualified,
  kInstance,
  kBaseQualified,
  kConstructor,
  kBaseConstructor,
};

enum class MirDispatchKind {
  kDirect,
  kVirtual,
  kInterface,
};

struct MirCallInstruction {
  MirCallKind kind;
  MirDispatchKind dispatch;
  bool receiver_is_self;
  SymbolId callable;
  std::optional<MirValueId> receiver;
  std::vector<MirValueId> arguments;
  std::optional<FileId> interface_file{};
  std::optional<std::size_t> interface_slot{};
  StructReceiverMode struct_receiver{StructReceiverMode::kNone};

  friend bool operator==(const MirCallInstruction&,
                         const MirCallInstruction&) = default;
};

struct MirInitializeFieldsInstruction {
  friend bool operator==(const MirInitializeFieldsInstruction&,
                         const MirInitializeFieldsInstruction&) = default;
};

struct MirPhiIncoming {
  MirBlockId predecessor;
  MirValueId value;

  friend bool operator==(const MirPhiIncoming&,
                         const MirPhiIncoming&) = default;
};

struct MirPhiInstruction {
  std::vector<MirPhiIncoming> incoming;

  friend bool operator==(const MirPhiInstruction&,
                         const MirPhiInstruction&) = default;
};

using MirInstructionData = std::variant<
    MirInvalidInstruction, MirLiteralInstruction, MirScalarConstantInstruction,
    MirLoadSymbolInstruction, MirDeclareLocalInstruction,
    MirStoreSymbolInstruction, MirLoadMemberInstruction,
    MirStoreMemberInstruction, MirLoadStorageInstruction,
    MirStoreStorageInstruction, MirArrayLiteralInstruction,
    MirArrayLoadInstruction, MirArrayStoreInstruction,
    MirArrayLengthInstruction, MirStringMetaInstruction,
    MirObjectMetaInstruction, MirIntegerWriteInstruction,
    MirIntegerReadInstruction, MirUnaryInstruction, MirBinaryInstruction,
    MirConvertInstruction, MirIsNonNullInstruction, MirNullAssertInstruction,
    MirTypeTestInstruction, MirCheckedCastInstruction, MirCallInstruction,
    MirInitializeFieldsInstruction, MirPhiInstruction>;

struct MirInstruction {
  std::optional<MirValueId> result;
  TypeId type;
  SourceRange range;
  MirInstructionData data;

  friend bool operator==(const MirInstruction&,
                         const MirInstruction&) = default;
};

struct MirJumpTerminator {
  MirBlockId target;

  friend bool operator==(const MirJumpTerminator&,
                         const MirJumpTerminator&) = default;
};

struct MirBranchTerminator {
  MirValueId condition;
  MirBlockId then_block;
  MirBlockId else_block;

  friend bool operator==(const MirBranchTerminator&,
                         const MirBranchTerminator&) = default;
};

struct MirSwitchCase {
  ScalarConstant value;
  MirBlockId target;

  friend bool operator==(const MirSwitchCase&, const MirSwitchCase&) = default;
};

// Cases are sorted by normalized bits, with exact selector types. Enum
// dispatch checks unmatched tags before default; invalid tags take
// invalid_block.
struct MirSwitchTerminator {
  MirValueId selector;
  TypeId selector_type;
  std::vector<MirSwitchCase> cases;
  MirBlockId default_block;
  std::optional<MirBlockId> invalid_block;

  friend bool operator==(const MirSwitchTerminator&,
                         const MirSwitchTerminator&) = default;
};

struct MirReturnTerminator {
  std::optional<MirValueId> value;

  friend bool operator==(const MirReturnTerminator&,
                         const MirReturnTerminator&) = default;
};

struct MirUnreachableTerminator {
  friend bool operator==(const MirUnreachableTerminator&,
                         const MirUnreachableTerminator&) = default;
};
struct MirTrapTerminator {
  friend bool operator==(const MirTrapTerminator&,
                         const MirTrapTerminator&) = default;
};

using MirTerminatorData =
    std::variant<MirJumpTerminator, MirBranchTerminator, MirSwitchTerminator,
                 MirReturnTerminator, MirUnreachableTerminator,
                 MirTrapTerminator>;

struct MirTerminator {
  SourceRange range;
  MirTerminatorData data;

  friend bool operator==(const MirTerminator&, const MirTerminator&) = default;
};

// Sorted unique successors, including the enum invalid-tag path. Phi incoming
// values are per predecessor block, not per label or physical LLVM edge.
[[nodiscard]] std::vector<MirBlockId> mir_successors(
    const MirTerminator& terminator);

struct MirBasicBlock {
  bool is_reachable;
  std::vector<MirInstruction> instructions;
  MirTerminator terminator;

  friend bool operator==(const MirBasicBlock&, const MirBasicBlock&) = default;
};

struct MirBody {
  SourceRange range;
  MirBlockId entry;
  std::vector<MirBasicBlock> blocks;
  std::size_t value_count;

  friend bool operator==(const MirBody&, const MirBody&) = default;
};

struct MirField {
  SymbolId symbol;
  std::optional<MirBody> initializer;
  // Static fields carry data only; initializer bodies belong to instances.
  std::optional<ScalarConstant> static_constant{};

  friend bool operator==(const MirField&, const MirField&) = default;
};

struct MirCallable {
  SymbolId symbol;
  std::vector<SymbolId> parameters;
  MirBody body;
  StructReceiverMode struct_receiver{StructReceiverMode::kNone};

  friend bool operator==(const MirCallable&, const MirCallable&) = default;
};

struct MirFileClass {
  FileId file;
  SymbolId symbol;
  std::optional<FileId> base_file;
  std::vector<MirField> fields;
  std::vector<MirCallable> functions;
  std::vector<MirCallable> constructors;
  std::vector<MemberReference> member_order;
  bool is_imported_declaration{false};

  friend bool operator==(const MirFileClass&, const MirFileClass&) = default;
};

struct MirModule {
  std::vector<MirFileClass> files;

  friend bool operator==(const MirModule&, const MirModule&) = default;
};

// Requires verified HIR.
[[nodiscard]] MirModule lower_to_mir(const HirModule& hir,
                                     const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_H_

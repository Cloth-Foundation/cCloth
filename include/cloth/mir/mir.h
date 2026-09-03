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

struct MirInvalidInstruction {};

struct MirLiteralInstruction {
  LiteralKind kind;
  std::string lexeme;
};

struct MirLoadSymbolInstruction {
  SymbolId symbol;
};

struct MirDeclareLocalInstruction {
  SymbolId symbol;
  std::optional<MirValueId> initializer;
};

struct MirStoreSymbolInstruction {
  SymbolId symbol;
  MirValueId value;
};

struct MirLoadMemberInstruction {
  MirValueId object;
  SymbolId member;
};

struct MirStoreMemberInstruction {
  MirValueId object;
  SymbolId member;
  MirValueId value;
};

// A compiler-only storage recipe, never a Cloth value or escaping pointer.
// Exactly one root is present. An index selects an element of an object root;
// subsequent fields project through inline struct storage only.
struct MirStoragePath {
  std::optional<SymbolId> symbol;
  std::optional<MirValueId> object;
  std::optional<MirValueId> index;
  std::vector<SymbolId> fields;
};

struct MirLoadStorageInstruction {
  MirStoragePath path;
};

struct MirStoreStorageInstruction {
  MirStoragePath path;
  MirValueId value;
};

struct MirArrayLiteralInstruction {
  TypeId element_type;
  std::vector<MirValueId> elements;
};

struct MirArrayLoadInstruction {
  MirValueId array;
  MirValueId index;
};

struct MirArrayStoreInstruction {
  MirValueId array;
  MirValueId index;
  MirValueId value;
};

struct MirArrayLengthInstruction {
  MirValueId array;
};

struct MirStringMetaInstruction {
  MirValueId string;
  StringMetaQuery query;
};

struct MirObjectMetaInstruction {
  MirValueId object;
};

struct MirIntegerWriteInstruction {
  MirValueId value;
  MirValueId destination;
  MirValueId offset;
  IntegerByteOrder byte_order;
};

struct MirIntegerReadInstruction {
  MirValueId source;
  MirValueId offset;
  IntegerByteOrder byte_order;
};

struct MirUnaryInstruction {
  TokenKind operation;
  MirValueId operand;
};

struct MirBinaryInstruction {
  MirValueId left;
  TokenKind operation;
  MirValueId right;
};

enum class MirConversionKind {
  kWidenNumeric,
  kCheckedNumeric,
  kWidenReference,
  kToNullable,
  kFromNullable,
};

struct MirConvertInstruction {
  MirValueId value;
  MirConversionKind kind;
};

struct MirIsNonNullInstruction {
  MirValueId value;
};

struct MirNullAssertInstruction {
  MirValueId value;
};

struct MirTypeTestInstruction {
  MirValueId value;
  TypeId target;
};

struct MirCheckedCastInstruction {
  MirValueId value;
  TypeId target;
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
};

struct MirInitializeFieldsInstruction {};

struct MirPhiIncoming {
  MirBlockId predecessor;
  MirValueId value;
};

struct MirPhiInstruction {
  std::vector<MirPhiIncoming> incoming;
};

using MirInstructionData = std::variant<
    MirInvalidInstruction, MirLiteralInstruction, MirLoadSymbolInstruction,
    MirDeclareLocalInstruction, MirStoreSymbolInstruction,
    MirLoadMemberInstruction, MirStoreMemberInstruction,
    MirLoadStorageInstruction, MirStoreStorageInstruction,
    MirArrayLiteralInstruction, MirArrayLoadInstruction,
    MirArrayStoreInstruction, MirArrayLengthInstruction,
    MirStringMetaInstruction, MirObjectMetaInstruction,
    MirIntegerWriteInstruction, MirIntegerReadInstruction, MirUnaryInstruction,
    MirBinaryInstruction, MirConvertInstruction, MirIsNonNullInstruction,
    MirNullAssertInstruction, MirTypeTestInstruction, MirCheckedCastInstruction,
    MirCallInstruction, MirInitializeFieldsInstruction, MirPhiInstruction>;

struct MirInstruction {
  std::optional<MirValueId> result;
  TypeId type;
  SourceRange range;
  MirInstructionData data;
};

struct MirJumpTerminator {
  MirBlockId target;
};

struct MirBranchTerminator {
  MirValueId condition;
  MirBlockId then_block;
  MirBlockId else_block;
};

struct MirSwitchCase {
  ScalarConstant value;
  MirBlockId target;
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
};

struct MirReturnTerminator {
  std::optional<MirValueId> value;
};

struct MirUnreachableTerminator {};
struct MirTrapTerminator {};

using MirTerminatorData =
    std::variant<MirJumpTerminator, MirBranchTerminator, MirSwitchTerminator,
                 MirReturnTerminator, MirUnreachableTerminator,
                 MirTrapTerminator>;

struct MirTerminator {
  SourceRange range;
  MirTerminatorData data;
};

// Sorted unique successors, including the enum invalid-tag path. Phi incoming
// values are per predecessor block, not per label or physical LLVM edge.
[[nodiscard]] std::vector<MirBlockId> mir_successors(
    const MirTerminator& terminator);

struct MirBasicBlock {
  bool is_reachable;
  std::vector<MirInstruction> instructions;
  MirTerminator terminator;
};

struct MirBody {
  SourceRange range;
  MirBlockId entry;
  std::vector<MirBasicBlock> blocks;
  std::size_t value_count;
};

struct MirField {
  SymbolId symbol;
  std::optional<MirBody> initializer;
  // Static fields carry data only; initializer bodies belong to instances.
  std::optional<ScalarConstant> static_constant{};
};

struct MirCallable {
  SymbolId symbol;
  std::vector<SymbolId> parameters;
  MirBody body;
  StructReceiverMode struct_receiver{StructReceiverMode::kNone};
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
};

struct MirModule {
  std::vector<MirFileClass> files;
};

// Requires verified HIR.
[[nodiscard]] MirModule lower_to_mir(const HirModule& hir,
                                     const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_H_

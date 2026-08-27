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

struct MirUnaryInstruction {
  TokenKind operation;
  MirValueId operand;
};

struct MirBinaryInstruction {
  MirValueId left;
  TokenKind operation;
  MirValueId right;
};

struct MirConvertInstruction {
  MirValueId value;
};

enum class MirCallKind {
  kUnqualified,
  kClassQualified,
  kInstance,
  kConstructor,
};

struct MirCallInstruction {
  MirCallKind kind;
  SymbolId callable;
  std::optional<MirValueId> receiver;
  std::vector<MirValueId> arguments;
};

struct MirPhiIncoming {
  MirBlockId predecessor;
  MirValueId value;
};

struct MirPhiInstruction {
  std::vector<MirPhiIncoming> incoming;
};

using MirInstructionData =
    std::variant<MirInvalidInstruction, MirLiteralInstruction,
                 MirLoadSymbolInstruction, MirDeclareLocalInstruction,
                 MirStoreSymbolInstruction, MirLoadMemberInstruction,
                 MirStoreMemberInstruction, MirArrayLiteralInstruction,
                 MirArrayLoadInstruction, MirArrayStoreInstruction,
                 MirArrayLengthInstruction, MirUnaryInstruction,
                 MirBinaryInstruction, MirConvertInstruction,
                 MirCallInstruction, MirPhiInstruction>;

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

struct MirReturnTerminator {
  std::optional<MirValueId> value;
};

struct MirUnreachableTerminator {};

using MirTerminatorData =
    std::variant<MirJumpTerminator, MirBranchTerminator, MirReturnTerminator,
                 MirUnreachableTerminator>;

struct MirTerminator {
  SourceRange range;
  MirTerminatorData data;
};

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
};

struct MirCallable {
  SymbolId symbol;
  std::vector<SymbolId> parameters;
  MirBody body;
};

struct MirFileClass {
  FileId file;
  SymbolId symbol;
  std::vector<MirField> fields;
  std::vector<MirCallable> functions;
  std::vector<MirCallable> constructors;
  std::vector<MemberReference> member_order;
};

struct MirModule {
  std::vector<MirFileClass> files;
};

[[nodiscard]] MirModule lower_to_mir(const HirModule& hir,
                                     const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_H_

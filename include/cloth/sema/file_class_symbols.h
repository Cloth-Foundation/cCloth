#ifndef CLOTH_SEMA_FILE_CLASS_SYMBOLS_H_
#define CLOTH_SEMA_FILE_CLASS_SYMBOLS_H_

#include "cloth/ast/ast.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cloth {

struct ParameterSymbol {
  TypeSyntax type;
  std::string_view name;
  SourceRange range;
  bool is_final{false};
};

struct MemberSymbol {
  std::string_view name;
  DeclarationKind kind;
  Visibility visibility;
  SourceRange range;
  std::vector<ParameterSymbol> parameters;
  std::optional<TypeSyntax> declared_type;
  bool is_valid{true};
  bool is_final{false};
  bool is_static{false};
  bool is_override{false};
  bool is_abstract{false};
};

class FileClassSymbols {
 public:
  FileClassSymbols(std::string name, Visibility visibility, SourceRange range);

  [[nodiscard]] std::size_t add(MemberSymbol symbol);

  [[nodiscard]] const std::string& name() const noexcept;
  [[nodiscard]] Visibility visibility() const noexcept;
  [[nodiscard]] SourceRange range() const noexcept;
  [[nodiscard]] std::span<const MemberSymbol> members() const noexcept;
  [[nodiscard]] const MemberSymbol& member(std::size_t index) const;

 private:
  std::string name_;
  Visibility visibility_;
  SourceRange range_;
  std::vector<MemberSymbol> members_;
};

}  // namespace cloth

#endif  // CLOTH_SEMA_FILE_CLASS_SYMBOLS_H_

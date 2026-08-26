#include "cloth/sema/visibility.h"

namespace cloth {
namespace {

constexpr bool is_ascii_letter(char character) noexcept {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z');
}

constexpr bool is_decimal_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

}  // namespace

bool is_valid_identifier(std::string_view name) noexcept {
  if (name.empty() || (!is_ascii_letter(name.front()) && name.front() != '_')) {
    return false;
  }

  for (const char character : name.substr(1)) {
    if (!is_ascii_letter(character) && !is_decimal_digit(character) &&
        character != '_') {
      return false;
    }
  }
  return true;
}

Visibility infer_visibility(std::string_view name) noexcept {
  if (!name.empty() && name.front() >= 'A' && name.front() <= 'Z') {
    return Visibility::kPublic;
  }
  return Visibility::kPrivate;
}

std::string_view visibility_name(Visibility visibility) noexcept {
  switch (visibility) {
    case Visibility::kPublic:
      return "public";
    case Visibility::kPrivate:
      return "private";
  }
  return "unknown";
}

}  // namespace cloth

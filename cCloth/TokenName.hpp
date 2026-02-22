#pragma once
#include "Token.hpp"

namespace cloth {
	namespace debug {
		[[nodiscard]] inline const char* to_string(cloth::token::TokenKind kind) noexcept {
			using TK = cloth::token::TokenKind;
			switch (kind) {
				case TK::EndOfFile: return "EndOfFile";
				case TK::Error: return "Error";
				case TK::Identifier: return "Identifier";
				case TK::Number: return "Number";
				case TK::String: return "String";
				case TK::Operator: return "Operator";
				case TK::Punctuation: return "Punctuation";
				case TK::Keyword: return "Keyword";
				case TK::Whitespace: return "Whitespace";
				case TK::Comment: return "Comment";
				case TK::Meta: return "Meta";
				default: return "Unknown";
			}
		}

		[[nodiscard]] inline const char* to_string(cloth::token::Keyword keyword) noexcept {
			using KW = cloth::token::Keyword;
			switch (keyword) {
				case KW::None: return "None";

					// Control flow
				case KW::If: return "If";
				case KW::Else: return "Else";
				case KW::For: return "For";
				case KW::While: return "While";
				case KW::Do: return "Do";
				case KW::Switch: return "Switch";
				case KW::Case: return "Case";
				case KW::Default: return "Default";
				case KW::Return: return "Return";
				case KW::Break: return "Break";
				case KW::Continue: return "Continue";

					// Declarations
				case KW::Func: return "Func";
				case KW::Struct: return "Struct";
				case KW::Enum: return "Enum";
				case KW::Interface: return "Interface";
				case KW::Class: return "Class";
				case KW::Let: return "Let";
				case KW::Var: return "Var";
				case KW::Const: return "Const";

					// Operators
				case KW::As: return "As";
				case KW::In: return "In";
				case KW::Or: return "Or";
				case KW::And: return "And";
				case KW::Is: return "Is";

					// Literals
				case KW::Null: return "Null";
				case KW::True: return "True";
				case KW::False: return "False";

					// Module system
				case KW::Import: return "Import";
				case KW::Static: return "Static";
				case KW::Public: return "Public";
				case KW::Private: return "Private";
				case KW::Internal: return "Internal";
				case KW::Module: return "Module";

					// Exception handling
				case KW::Try: return "Try";
				case KW::Catch: return "Catch";
				case KW::Finally: return "Finally";
				case KW::Throw: return "Throw";

					// Integer type
				case KW::I8: return "I8";
				case KW::I16: return "I16";
				case KW::I32: return "I32";
				case KW::I64: return "I64";
				case KW::U8: return "U8";
				case KW::U16: return "U16";
				case KW::U32: return "U32";
				case KW::U64: return "U64";

					// Float types
				case KW::F32: return "F32";
				case KW::F64: return "F64";

					// Other types
				case KW::String: return "String";
				case KW::Char: return "Char";
				case KW::Bool: return "Bool";
				case KW::Bit: return "Bit";
				case KW::Byte: return "Byte";
				case KW::Void: return "Void";
				case KW::Any: return "Any";

					// Async/memory management
				case KW::Defer: return "Defer";
				case KW::Async: return "Async";
				case KW::Await: return "Await";
				case KW::Atomic: return "Atomic";
				case KW::Shared: return "Shared";
				case KW::Owned: return "Owned";
				case KW::Delete: return "Delete";
				case KW::New: return "New";
				case KW::This: return "This";
				case KW::Super: return "Super";
				default:
					return "UnknownKeyword";
			}
		}

		[[nodiscard]] inline const char* to_string(cloth::token::Operator op) noexcept {
			using OP = cloth::token::Operator;
			switch (op) {
				case OP::None: return "None";

					// Arithmetic
				case OP::Plus: return "Plus (+)";
				case OP::Minus: return "Minus (-)";
				case OP::Star: return "Star (*)";
				case OP::Slash: return "Slash (/)";
				case OP::Percent: return "Percent (%)";
				case OP::PlusPlus: return "PlusPlus (++)";
				case OP::MinusMinus: return "MinusMinus (--)";

					// Assignment
				case OP::Assign: return "Assign (=)";
				case OP::PlusAssign: return "PlusAssign (+=)";
				case OP::MinusAssign: return "MinusAssign (-=)";
				case OP::StarAssign: return "StarAssign (*=)";
				case OP::SlashAssign: return "SlashAssign (/=)";
				case OP::PercentAssign: return "PercentAssign (%=)";

					// Comparison
				case OP::Equal: return "Equal (==)";
				case OP::NotEqual: return "NotEqual (!=)";
				case OP::Less: return "Less (<)";
				case OP::Greater: return "Greater (>)";
				case OP::LessEqual: return "LessEqual (<=)";
				case OP::GreaterEqual: return "GreaterEqual (>=)";

					// Bitwise/Logical
				case OP::Amp: return "Amp (&)";
				case OP::Pipe: return "Pipe (|)";
				case OP::Bang: return "Bang (!)";
				case OP::Tilde: return "Tilde (~)";

					// Punctuation
				case OP::Dot: return "Dot (.)";
				case OP::Comma: return "Comma (,)";
				case OP::Semicolon: return "Semicolon (;)";
				case OP::Colon: return "Colon (:)";
				case OP::Arrow: return "Arrow (->)";
				case OP::LeftParen: return "LeftParen (()";
				case OP::RightParen: return "RightParen ())";
				case OP::LeftBrace: return "LeftBrace ({)";
				case OP::RightBrace: return "RightBrace (})";
				case OP::LeftBracket: return "LeftBracket ([)";
				case OP::RightBracket: return "RightBracket (])";
				case OP::At: return "At (@)";
				case OP::Hash: return "Hash (#)";
				case OP::Dollar: return "Dollar ($)";
				case OP::Question: return "Question (?)";

					// Multi-char
				case OP::ColonColon: return "ColonColon (::)";
				case OP::DotDot: return "DotDot (..)";
				case OP::DotDotDot: return "DotDotDot (...)";
				default:
					return "UnknownOperator";
			}
		}
	} // namespace cloth::debug
} // namespace cloth
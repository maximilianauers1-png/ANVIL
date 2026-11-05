// Expression parser for input fields: "12.5 cm + 3 mm", "2*r", "sqrt(a^2+b^2)".
//
// Grammar (precedence low -> high, all binary operators left-associative
// except '^'):
//   expr    := term (('+' | '-') term)*
//   term    := unary (('*' | '/' | <juxtaposition>) unary)*
//   unary   := ('+' | '-') unary | power
//   power   := primary ('^' unary)?
//   primary := number | identifier | identifier '(' args ')' | '(' expr ')'
// Juxtaposition ("12.5 cm") has the same precedence as '*', so "1/2 mm" is
// (1/2)*mm = 0.5 mm, which matches how people write it.
#pragma once
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "anvil/units.hpp"

namespace anvil {

struct ExprError : std::runtime_error {
    std::size_t pos;  // byte offset in the input where the error was detected
    ExprError(const std::string& msg, std::size_t p) : std::runtime_error(msg), pos(p) {}
};

/// Resolves a variable name to a quantity; returns nullopt if unknown.
using VariableResolver = std::function<std::optional<RtQuantity>(std::string_view)>;

/// Evaluates `text`. Units (mm, cm, m, in, inch, deg, rad, um) and the
/// constant `pi` are built in; other identifiers go to `vars`.
RtQuantity eval_expr(std::string_view text, const VariableResolver& vars = {});

/// Evaluates and requires a length (or a bare number, interpreted in `default_unit`).
double eval_length_mm(std::string_view text, const VariableResolver& vars = {},
                      LengthUnit default_unit = LengthUnit::Millimeter);

/// Lists the identifiers (variables) an expression references, without evaluating.
std::vector<std::string> expr_identifiers(std::string_view text);

}  // namespace anvil

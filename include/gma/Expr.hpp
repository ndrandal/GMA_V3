#pragma once

#include <functional>
#include <set>
#include <string>
#include <unordered_map>

#include <rapidjson/document.h>

namespace gma::expr {

// Evaluation environment: named input value -> double. The Expr node (ENC-645)
// populates this from its wired inputs; absent names evaluate to 0.0.
using Env = std::unordered_map<std::string, double>;

// A compiled expression: a closure evaluated per tick over an Env. Compilation
// happens once (at subscribe/build time); evaluation does no parsing and is
// total and bounded — there are no loops, recursion, or unbounded work, so a
// compiled Expr preserves the engine's bounded-cost-per-tick invariant.
using Compiled = std::function<double(const Env&)>;

// Compile a JSON expression tree into a Compiled closure. Grammar:
//   number / bool           -> literal
//   {"lit": <number|bool>}  -> literal
//   {"ref": "<name>"}       -> named input reference (0.0 if absent at eval)
//   {"op": "<operator>", "args": [<expr>, ...]}
//   {"op": "fn", "name": "<builtin>", "args": [<expr>, ...]}  -> FunctionMap call
//
// Operators (numeric; comparisons/logical yield 1.0/0.0, truthy = value > 0.5):
//   n-ary  : add mul min max and or
//   binary : sub div mod gt lt gte lte eq neq
//   unary  : neg abs not
// div/mod by zero yield 0.0 (kept finite/total — no NaN/inf propagation).
//
// Throws std::runtime_error on any malformed expression (unknown op/fn, wrong
// arity, bad node shape) so errors surface at build time, never at eval time.
// If `refs` is non-null, every referenced input name is collected into it (so
// a caller can discover which inputs an expression needs to be wired).
Compiled compile(const rapidjson::Value& spec, std::set<std::string>* refs = nullptr);

} // namespace gma::expr

#include "gma/Expr.hpp"
#include "gma/FunctionMap.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace gma::expr {
namespace {

// Recursion guard for compile-time tree walking. Matches JsonValidator's
// MAX_TREE_DEPTH=32 convention so a deeply-nested Expr can't blow the stack
// during compilation (ENC-804/L2). buildTree/buildNode skip JsonValidator, so
// the cap is enforced here rather than relied upon upstream.
constexpr int MAX_EXPR_DEPTH = 32;

// Denominator guard for div/mod. Matches BuiltinFunctions.cpp's EPSILON (1e-6)
// so a near-zero denominator yields 0.0 instead of inf, preserving the
// "no NaN/inf propagation" invariant documented on Expr.hpp (ENC-804/L3).
constexpr double EPSILON = 1e-6;

Compiled compileNode(const rapidjson::Value& v, std::set<std::string>* refs,
                     int depth);

std::vector<Compiled> compileArgs(const rapidjson::Value& v,
                                  std::set<std::string>* refs, int depth) {
  if (!v.HasMember("args") || !v["args"].IsArray())
    throw std::runtime_error("expr: operator requires an 'args' array");
  std::vector<Compiled> out;
  out.reserve(v["args"].Size());
  for (const auto& a : v["args"].GetArray())
    out.push_back(compileNode(a, refs, depth + 1));
  return out;
}

Compiled compileNode(const rapidjson::Value& v, std::set<std::string>* refs,
                     int depth) {
  if (depth > MAX_EXPR_DEPTH)
    throw std::runtime_error("expr: expression exceeds maximum depth of " +
                             std::to_string(MAX_EXPR_DEPTH));

  // Bare literals.
  if (v.IsBool())   { double c = v.GetBool() ? 1.0 : 0.0; return [c](const Env&) { return c; }; }
  if (v.IsNumber()) { double c = v.GetDouble();           return [c](const Env&) { return c; }; }
  if (!v.IsObject())
    throw std::runtime_error("expr: node must be a number, bool, or object");

  // {"ref": "name"}
  if (v.HasMember("ref")) {
    if (!v["ref"].IsString())
      throw std::runtime_error("expr: 'ref' must be a string");
    std::string name = v["ref"].GetString();
    if (refs) refs->insert(name);
    return [name](const Env& e) {
      auto it = e.find(name);
      return it == e.end() ? 0.0 : it->second;  // absent input -> 0.0
    };
  }

  // {"lit": <number|bool>}
  if (v.HasMember("lit")) {
    const auto& l = v["lit"];
    if (l.IsBool())   { double c = l.GetBool() ? 1.0 : 0.0; return [c](const Env&) { return c; }; }
    if (l.IsNumber()) { double c = l.GetDouble();           return [c](const Env&) { return c; }; }
    throw std::runtime_error("expr: 'lit' must be a number or bool");
  }

  if (!v.HasMember("op") || !v["op"].IsString())
    throw std::runtime_error("expr: object must have 'ref', 'lit', or 'op'");
  const std::string op = v["op"].GetString();

  // {"op":"fn","name":...} — bridge to the FunctionMap builtin registry.
  if (op == "fn") {
    if (!v.HasMember("name") || !v["name"].IsString())
      throw std::runtime_error("expr: 'fn' requires a string 'name'");
    const std::string fnName = v["name"].GetString();
    gma::Func fn;
    try {
      fn = gma::FunctionMap::instance().getFunction(fnName);
    } catch (...) {
      throw std::runtime_error("expr: unknown fn '" + fnName + "'");
    }
    auto args = compileArgs(v, refs, depth);
    return [fn, args](const Env& e) {
      std::vector<double> xs;
      xs.reserve(args.size());
      for (const auto& a : args) xs.push_back(a(e));
      return fn(xs);
    };
  }

  auto args = compileArgs(v, refs, depth);
  const auto needN = [&](std::size_t n) {
    if (args.size() != n)
      throw std::runtime_error("expr: '" + op + "' requires " + std::to_string(n) + " args");
  };
  const auto needAtLeast = [&](std::size_t n) {
    if (args.size() < n)
      throw std::runtime_error("expr: '" + op + "' requires at least " + std::to_string(n) + " args");
  };

  // --- n-ary ---
  if (op == "add") { needAtLeast(1); return [args](const Env& e) { double s = 0.0; for (const auto& a : args) s += a(e); return s; }; }
  if (op == "mul") { needAtLeast(1); return [args](const Env& e) { double s = 1.0; for (const auto& a : args) s *= a(e); return s; }; }
  if (op == "min") { needAtLeast(1); return [args](const Env& e) { double s = args[0](e); for (std::size_t i = 1; i < args.size(); ++i) s = std::min(s, args[i](e)); return s; }; }
  if (op == "max") { needAtLeast(1); return [args](const Env& e) { double s = args[0](e); for (std::size_t i = 1; i < args.size(); ++i) s = std::max(s, args[i](e)); return s; }; }
  if (op == "and") { needAtLeast(1); return [args](const Env& e) { for (const auto& a : args) if (!(a(e) > 0.5)) return 0.0; return 1.0; }; }
  if (op == "or")  { needAtLeast(1); return [args](const Env& e) { for (const auto& a : args) if (a(e) > 0.5) return 1.0; return 0.0; }; }

  // --- binary ---
  if (op == "sub") { needN(2); return [args](const Env& e) { return args[0](e) - args[1](e); }; }
  if (op == "div") { needN(2); return [args](const Env& e) { double d = args[1](e); return std::abs(d) > EPSILON ? args[0](e) / d : 0.0; }; }
  if (op == "mod") { needN(2); return [args](const Env& e) { double d = args[1](e); return std::abs(d) > EPSILON ? std::fmod(args[0](e), d) : 0.0; }; }
  if (op == "gt")  { needN(2); return [args](const Env& e) { return args[0](e) >  args[1](e) ? 1.0 : 0.0; }; }
  if (op == "lt")  { needN(2); return [args](const Env& e) { return args[0](e) <  args[1](e) ? 1.0 : 0.0; }; }
  if (op == "gte") { needN(2); return [args](const Env& e) { return args[0](e) >= args[1](e) ? 1.0 : 0.0; }; }
  if (op == "lte") { needN(2); return [args](const Env& e) { return args[0](e) <= args[1](e) ? 1.0 : 0.0; }; }
  if (op == "eq")  { needN(2); return [args](const Env& e) { return args[0](e) == args[1](e) ? 1.0 : 0.0; }; }
  if (op == "neq") { needN(2); return [args](const Env& e) { return args[0](e) != args[1](e) ? 1.0 : 0.0; }; }

  // --- unary ---
  if (op == "neg") { needN(1); return [args](const Env& e) { return -args[0](e); }; }
  if (op == "abs") { needN(1); return [args](const Env& e) { return std::fabs(args[0](e)); }; }
  if (op == "not") { needN(1); return [args](const Env& e) { return args[0](e) > 0.5 ? 0.0 : 1.0; }; }

  throw std::runtime_error("expr: unknown op '" + op + "'");
}

} // namespace

Compiled compile(const rapidjson::Value& spec, std::set<std::string>* refs) {
  return compileNode(spec, refs, 0);
}

} // namespace gma::expr

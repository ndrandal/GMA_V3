#pragma once

// Keep this header *light* to avoid include-depth blowups.
// Only include what we must expose in the API surface.

#include <functional>
#include <memory>
#include <string>

// ---- span support -----------------------------------------------------------
// Prefer C++20 <span>. If unavailable (older /Std or toolchain), use a tiny
// fallback that mimics the subset we need: span<const T>.
#if __has_include(<span>)
  #include <span>
  namespace gma_detail {
    template <typename T> using span_c = std::span<const T>;
  }
#else
  // Minimal fallback span (const-only view) — enough for std::function signatures.
  #include <cstddef>
  namespace gma_detail {
    template <typename T>
    class span_c {
    public:
      using element_type = T;
      using value_type   = std::remove_cv_t<T>;
      using size_type    = std::size_t;
      using pointer      = const T*;
      using iterator     = const T*;

      span_c() : data_(nullptr), size_(0) {}
      span_c(const T* p, size_type n) : data_(p), size_(n) {}

      iterator begin() const { return data_; }
      iterator end()   const { return data_ + size_; }
      pointer  data()  const { return data_; }
      size_type size() const { return size_; }
      bool empty()     const { return size_ == 0; }
      const T& operator[](size_type i) const { return data_[i]; }
    private:
      const T* data_;
      size_type size_;
    };
  }
#endif
// -----------------------------------------------------------------------------


// We must not redefine ArgType or INode here. Include their canonical headers.
#include "gma/SymbolValue.hpp"   // defines gma::ArgType (keep this single-source of truth)
#include "gma/nodes/INode.hpp"   // defines gma::INode base interface

// RapidJSON value type used by the builder
#include <rapidjson/document.h>  // brings rapidjson::Value into scope

namespace gma {

// Public ABI typedefs used across builder APIs
using ArgType = ::gma::ArgType;                    // from SymbolValue.hpp
using INode   = ::gma::INode;                      // from nodes/INode.hpp
using Span    = gma_detail::span_c<ArgType>;       // view over const ArgType
using Span_t  = Span;                              // legacy alias used in .cpps

namespace tree {

// Forward declarations of runtime dependencies.
class ThreadPool;          // your pool/executor type
class MarketDispatcher;    // your event bus / dispatch core

// All external stuff the TreeBuilder needs at construction time.
struct Deps {
  MarketDispatcher* dispatcher { nullptr };
  ThreadPool*       pool       { nullptr };
  // Add other cross-cutting services here as needed (logger, clock, etc.)
};

// Function node factory type: takes N args and returns a computed ArgType.
using Fn = std::function<ArgType(Span)>;

// Resolve a callable by name (math/op/aggregation/etc.)
Fn fnFromName(const std::string& name);

// Build a single node from a JSON spec, wiring children as needed.
// - spec: JSON object with fields like {"type":"Aggregate","op":"sum","children":[...]}
// - dir : optional name, useful for diagnostics / paths
// - deps: runtime services (dispatcher, pool)
// - parent: parent node (if creating hierarchy bottom-up)
std::shared_ptr<INode>
buildOne(const rapidjson::Value& spec,
         const std::string&      dir,
         const Deps&             deps,
         std::shared_ptr<INode>  parent = nullptr);

// Build a full tree from a JSON array/object root.
std::shared_ptr<INode>
buildTree(const rapidjson::Value& rootSpec,
          const Deps&             deps);

} // namespace tree
} // namespace gma

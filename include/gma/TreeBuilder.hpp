#pragma once

#include <functional>
#include <memory>
#include <string>
#include <cstddef>

#include "gma/Span.hpp"          // <-- single source of truth for Span
#include "gma/SymbolValue.hpp"   // ArgType
#include "gma/nodes/*"   // INode
#include <rapidjson/document.h>  // rapidjson::Value
// ---- Minimal, safe span-like view (works in C++17) --------------------------
namespace gma_detail {
  template <class T>
  class basic_span {
  public:
    using element_type = T;
    using value_type   = typename std::remove_cv<T>::type;
    using size_type    = std::size_t;
    using pointer      = const T*;
    using iterator     = const T*;

    basic_span() : p_(nullptr), n_(0) {}
    basic_span(const T* p, size_type n) : p_(p), n_(n) {}

    iterator begin() const { return p_; }
    iterator end()   const { return p_ + n_; }
    pointer  data()  const { return p_; }
    size_type size() const { return n_; }
    bool empty()     const { return n_ == 0; }
    const T& operator[](size_type i) const { return p_[i]; }
  private:
    const T* p_;
    size_type n_;
  };
} // namespace gma_detail
// -----------------------------------------------------------------------------

#include "gma/SymbolValue.hpp"   // canonical ArgType
#include "gma/nodes/INode.hpp"   // canonical INode
#include <rapidjson/document.h>  // rapidjson::Value

namespace gma {
  using ArgType = ::gma::ArgType;
  using INode   = ::gma::INode;

  // Our public “Span” type is always this local span-like view
  using Span   = gma_detail::basic_span<const ArgType>;
  using Span_t = Span; // legacy alias some .cpps use

  namespace tree {
    class MarketDispatcher;
    class ThreadPool;

    struct Deps {
      MarketDispatcher* dispatcher { nullptr };
      ThreadPool*       pool       { nullptr };
    };

    using Fn = std::function<ArgType(Span)>;

    Fn fnFromName(const std::string& name);

    std::shared_ptr<INode> buildOne(
      const rapidjson::Value& spec,
      const std::string&      dir,
      const Deps&             deps,
      std::shared_ptr<INode>  parent = nullptr
    );

    std::shared_ptr<INode> buildTree(
      const rapidjson::Value& rootSpec,
      const Deps&             deps
    );
  } // namespace tree
} // namespace gma

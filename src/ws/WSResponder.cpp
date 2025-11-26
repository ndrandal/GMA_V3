#include <ostream>
#include <variant>
#include <vector>
#include "gma/SymbolValue.hpp"

namespace gma {
  inline std::ostream& operator<<(std::ostream& os, const ArgType& v) {
    std::visit([&](const auto& x) {
      using X = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<X, std::monostate>) {
        os << "null";
      } else if constexpr (std::is_same_v<X, std::vector<double>>) {
        os << '[';
        for (size_t i = 0; i < x.size(); ++i) {
          if (i) os << ',';
          os << x[i];
        }
        os << ']';
      } else {
        os << x;
      }
    }, v);
    return os;
  }
}

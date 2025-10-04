#include "gma/ta/TAComputer.hpp"
#include <stdexcept>

namespace gma::ta {

TAComputer::SymState& TAComputer::sym(const std::string& symbol) {
  std::lock_guard<std::mutex> lk(mx_);
  return map_[symbol]; // creates if missing
}

const TAComputer::SymState& TAComputer::symConst(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(mx_);
  auto it = map_.find(symbol);
  if (it == map_.end()) {
    throw std::out_of_range("TAComputer::symConst: symbol not found: " + symbol);
  }
  return it->second;
}

void TAComputer::setLastPrice(const std::string& symbol, double px) {
  auto& s = sym(symbol);
  s.lastPrice = px;
}

double TAComputer::getLastPrice(const std::string& symbol) const {
  return symConst(symbol).lastPrice;
}

} // namespace gma::ta

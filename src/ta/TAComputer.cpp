#include "gma/ta/TAComputer.hpp"
#include <stdexcept>

namespace gma::ta {

void TAComputer::setLastPrice(const std::string& symbol, double px) {
  std::lock_guard<std::mutex> lk(mx_);
  map_[symbol].lastPrice = px;
}

double TAComputer::getLastPrice(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(mx_);
  auto it = map_.find(symbol);
  if (it == map_.end()) {
    throw std::out_of_range("TAComputer::getLastPrice: symbol not found: " + symbol);
  }
  return it->second.lastPrice;
}

TAComputer::SymState TAComputer::getState(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(mx_);
  auto it = map_.find(symbol);
  if (it == map_.end()) {
    throw std::out_of_range("TAComputer::getState: symbol not found: " + symbol);
  }
  return it->second; // return by value — safe
}

bool TAComputer::has(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(mx_);
  return map_.find(symbol) != map_.end();
}

} // namespace gma::ta

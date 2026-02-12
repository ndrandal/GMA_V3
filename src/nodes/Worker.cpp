#include "gma/nodes/Worker.hpp"
#include "gma/util/Logger.hpp"

namespace gma {

Worker::Worker(Fn fn, std::shared_ptr<INode> downstream)
  : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

void Worker::onValue(const SymbolValue& sv) {
  ArgType out;
  {
    std::lock_guard<std::mutex> lk(mx_);
    auto& vec = acc_[sv.symbol];
    vec.push_back(sv.value);
    out = fn_(Span<const ArgType>(vec.data(), vec.size()));
    vec.clear();
  }

  if (auto ds = downstream_.lock()) {
    ds->onValue(SymbolValue{ sv.symbol, out });
  }
}

void Worker::shutdown() noexcept {
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
  acc_.clear();
}

} // namespace gma

#include "gma/nodes/Pack.hpp"
#include "gma/util/Logger.hpp"
#include <stdexcept>

namespace gma {

PackPort::PackPort(std::weak_ptr<Pack> owner, std::size_t idx)
  : owner_(std::move(owner)), idx_(idx) {}

void PackPort::onValue(const StreamValue& sv) {
  if (auto p = owner_.lock()) p->onField(idx_, sv);
}

Pack::Pack(std::vector<std::string> names, std::shared_ptr<INode> downstream)
  : names_(std::move(names)), downstream_(std::move(downstream)) {
  if (names_.empty())
    throw std::invalid_argument("Pack: at least one field required");
}

void Pack::addPort(std::shared_ptr<INode> port) {
  ports_.push_back(std::move(port));
}

void Pack::onField(std::size_t idx, const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;
  if (idx >= names_.size()) return;

  Record rec;
  std::shared_ptr<INode> ds;
  {
    std::lock_guard<std::mutex> lk(mx_);

    auto it = state_.find(sv.symbol);
    if (it == state_.end()) {
      if (state_.size() >= MAX_SYMBOLS) {
        gma::util::logger().log(gma::util::LogLevel::Warn,
          "Pack: max symbols reached, dropping", {{"symbol", sv.symbol}});
        return;
      }
      it = state_.emplace(sv.symbol, SymState{}).first;
      it->second.latest.resize(names_.size());
    }

    auto& st = it->second;
    if (!st.latest[idx].has_value()) ++st.filled;
    st.latest[idx] = ArgValue{sv.value};

    if (st.filled < names_.size()) return;   // not yet complete for this symbol

    // Assemble the record from the latest per-field values, in declared order.
    rec.fields.reserve(names_.size());
    for (std::size_t i = 0; i < names_.size(); ++i)
      rec.fields.push_back(RecordField{names_[i], *st.latest[i]});
    ds = downstream_;
  }

  if (ds) ds->onValue(StreamValue{sv.symbol, ArgType{std::move(rec)}});
}

void Pack::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);

  std::vector<std::shared_ptr<INode>> ports;
  {
    std::lock_guard<std::mutex> lk(mx_);
    state_.clear();
    downstream_.reset();
    ports.swap(ports_);
  }
  for (auto& p : ports)
    if (p) p->shutdown();
}

} // namespace gma

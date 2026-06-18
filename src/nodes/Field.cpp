#include "gma/nodes/Field.hpp"
#include <variant>

namespace gma {

Field::Field(std::string name, std::shared_ptr<INode> downstream)
  : name_(std::move(name)), downstream_(std::move(downstream)) {}

void Field::onValue(const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::shared_ptr<INode> ds;
  {
    std::lock_guard<std::mutex> lk(mx_);
    ds = downstream_;
  }
  if (!ds) return;

  const Record* rec = std::get_if<Record>(&sv.value);
  if (!rec) return;                          // not a record -> drop
  const ArgType* f = recordFind(*rec, name_);
  if (!f) return;                            // field absent -> drop

  ds->onValue(StreamValue{sv.symbol, *f});
}

void Field::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
}

} // namespace gma

#include "gma/synthetic/SyntheticConnector.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <rapidjson/document.h>

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/engine/EventComputerRegistry.hpp"

namespace gma::synthetic {

// ---------- SyntheticEventComputer ----------

void SyntheticEventComputer::compute(const Event& e, engine::ComputeContext& ctx) {
  if (!ctx.store || !e.payload) return;
  if (!e.payload->HasMember("counter")) return;
  const auto& v = (*e.payload)["counter"];
  if (!v.IsNumber()) return;
  double n = v.GetDouble();
  ctx.store->set(e.symbol, "synthetic.sin", std::sin(n * 0.1));
  ctx.store->set(e.symbol, "synthetic.cos", std::cos(n * 0.1));
}

// ---------- SyntheticConnector ----------

SyntheticConnector::SyntheticConnector() = default;
SyntheticConnector::SyntheticConnector(Options opts) : _opts(std::move(opts)) {}
SyntheticConnector::~SyntheticConnector() = default;

void SyntheticConnector::registerWith(engine::EngineRegistries& reg) {
  if (!reg.dispatcher || !reg.io || !reg.computers) {
    throw std::runtime_error("SyntheticConnector: incomplete EngineRegistries");
  }

  // Register the synthetic.tick computer factory. Each Dispatcher's onTick
  // lazily instantiates one fresh SyntheticEventComputer per type.
  reg.computers->registerFactory("synthetic.tick", [] {
    return std::make_unique<SyntheticEventComputer>();
  });

  // Allocate the timer here; arming happens in start(), cancel in stop().
  _timer = std::make_unique<boost::asio::steady_timer>(*reg.io);
  _dispatcher = reg.dispatcher;
}

void SyntheticConnector::start() {
  if (!_timer || !_dispatcher) return;

  auto* timer        = _timer.get();
  auto  streamKey    = _opts.streamKey;
  auto  period       = std::chrono::milliseconds(std::max(1, _opts.tickMs));
  int   maxTicks     = _opts.maxTicks;
  auto  counter      = std::make_shared<std::uint64_t>(0);
  auto* dispatcher   = _dispatcher;

  auto selfRef = std::make_shared<std::function<void(const boost::system::error_code&)>>();
  *selfRef = [timer, streamKey, period, maxTicks, counter, dispatcher, selfRef]
             (const boost::system::error_code& ec) {
    if (ec) return;
    if (maxTicks > 0 && *counter >= static_cast<std::uint64_t>(maxTicks)) return;

    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    doc->AddMember("counter",
                   rapidjson::Value(static_cast<double>(*counter)),
                   doc->GetAllocator());
    ++*counter;

    Event ev;
    ev.type    = "synthetic.tick";
    ev.symbol  = streamKey;
    ev.payload = std::move(doc);
    dispatcher->onTick(ev);

    timer->expires_after(period);
    timer->async_wait(*selfRef);
  };

  timer->expires_after(period);
  timer->async_wait(*selfRef);
}

void SyntheticConnector::stop() noexcept {
  if (!_timer) return;
  try { _timer->cancel(); } catch (...) {}
}

} // namespace gma::synthetic

#include "gma/synthetic/SyntheticConnector.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <rapidjson/document.h>

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"

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

void SyntheticConnector::start() {
  // TODO: phase-4 — move timer arm (expires_after + async_wait) here.
}

void SyntheticConnector::stop() noexcept {
  // TODO: phase-4 — cancel the timer here (currently done via the
  // synthetic-timer-cancel ShutdownCoordinator step).
}

void SyntheticConnector::registerWith(engine::EngineRegistries& reg) {
  if (!reg.dispatcher || !reg.io) {
    throw std::runtime_error("SyntheticConnector: incomplete EngineRegistries");
  }

  // Hand the dispatcher our computer — it will filter events by eventType().
  reg.dispatcher->addComputer(std::make_unique<SyntheticEventComputer>());

  // Fire an event every tickMs. Tick loop re-arms itself until maxTicks is
  // reached (0 = unbounded).
  _timer = std::make_unique<boost::asio::steady_timer>(*reg.io);
  auto* timer = _timer.get();
  auto streamKey = _opts.streamKey;
  auto period    = std::chrono::milliseconds(std::max(1, _opts.tickMs));
  int  maxTicks  = _opts.maxTicks;
  auto counter   = std::make_shared<std::uint64_t>(0);
  auto dispatcher = reg.dispatcher;

  std::function<void(const boost::system::error_code&)> onExpiry;
  auto selfRef = std::make_shared<std::function<void(const boost::system::error_code&)>>();
  onExpiry = [timer, streamKey, period, maxTicks, counter, dispatcher, selfRef]
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
  *selfRef = std::move(onExpiry);

  timer->expires_after(period);
  timer->async_wait(*selfRef);

  if (reg.shutdown) {
    auto* cancelable = _timer.get();
    reg.shutdown->registerStep("synthetic-timer-cancel", 58, [cancelable] {
      try { cancelable->cancel(); } catch (...) {}
    });
  }
}

} // namespace gma::synthetic

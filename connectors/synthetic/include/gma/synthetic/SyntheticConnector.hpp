#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/steady_timer.hpp>

#include "gma/engine/IConnector.hpp"
#include "gma/engine/IEventComputer.hpp"

namespace gma::synthetic {

// Demo connector that proves the engine/connector boundary: it lives entirely
// under connectors/synthetic/ and touches zero files outside this directory.
// Emits synthetic events of type "synthetic.tick" every tickMs milliseconds
// carrying a monotonic counter; installs a computer that writes sin()/cos() of
// the counter to AtomicStore under keys "synthetic.sin" and "synthetic.cos".
class SyntheticConnector final : public engine::IConnector {
public:
  struct Options {
    std::string streamKey = "SYN";
    int         tickMs    = 100;
    int         maxTicks  = 0;    // 0 = unbounded
  };

  SyntheticConnector();
  explicit SyntheticConnector(Options opts);
  ~SyntheticConnector() override;

  std::string_view name() const override { return "synthetic"; }
  void registerWith(engine::EngineRegistries& reg) override;

private:
  Options                               _opts;
  std::unique_ptr<boost::asio::steady_timer> _timer;
  std::uint64_t                         _counter { 0 };
};

// The computer is also useful in isolation (unit tests), so it's public.
class SyntheticEventComputer final : public engine::IEventComputer {
public:
  std::string_view eventType() const override { return "synthetic.tick"; }
  void compute(const Event& e, engine::ComputeContext& ctx) override;
};

} // namespace gma::synthetic

// Runs once per test-binary boot. Registers engine builtins (worker functions
// and node types) and installs the market connector's default computer factory
// so every Dispatcher constructed inside any test picks up its own
// MarketTickComputer.
#include "gma/FunctionRegistry.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/market/MarketConnector.hpp"
#include <gtest/gtest.h>

namespace {

class BuiltinsEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    gma::registerBuiltinFunctions();
    gma::registerBuiltinNodeTypes();
    gma::market::MarketConnector::installDefaults();
  }
};

::testing::Environment* const kBuiltinsEnv =
    ::testing::AddGlobalTestEnvironment(new BuiltinsEnvironment);

} // namespace

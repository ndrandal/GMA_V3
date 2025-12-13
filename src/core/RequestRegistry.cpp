#include "gma/RequestRegistry.hpp"

#include "gma/util/Logger.hpp"

#include <utility>   // std::move
#include <string>

namespace gma {

void RequestRegistry::registerRequest(const std::string& id, std::shared_ptr<INode> root) {
  std::unique_lock lock(_mutex);
  _requests[id] = std::move(root);

  gma::util::logger().log(
    gma::util::LogLevel::Info,
    "Registered request",
    { {"id", id} }
  );
}

void RequestRegistry::unregisterRequest(const std::string& id) {
  std::shared_ptr<INode> root;

  {
    std::unique_lock lock(_mutex);
    auto it = _requests.find(id);
    if (it == _requests.end()) {
      return;
    }
    root = std::move(it->second);
    _requests.erase(it);
  }

  if (root) {
    gma::util::logger().log(
      gma::util::LogLevel::Info,
      "Shutting down request",
      { {"id", id} }
    );
    root->shutdown();
  }
}

void RequestRegistry::shutdownAll() noexcept {
  // Move all nodes out under lock, then shutdown outside lock
  std::unordered_map<std::string, std::shared_ptr<INode>> tmp;

  {
    std::unique_lock lock(_mutex);
    tmp = std::move(_requests);
    _requests.clear();
  }

  for (auto& kv : tmp) {
    const std::string& id = kv.first;
    auto& node = kv.second;

    if (node) {
      gma::util::logger().log(
        gma::util::LogLevel::Info,
        "Shutdown request (shutdownAll)",
        { {"id", id} }
      );
      try { node->shutdown(); } catch (...) {}
    }
  }
}

} // namespace gma

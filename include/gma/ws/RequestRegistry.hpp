#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <mutex>

class INode;

namespace gma::ws {

class RequestRegistry {
public:
  void add(const std::string& rid, std::shared_ptr<INode> root);
  void remove(const std::string& rid);
  void removeAll(); // for session close
  bool exists(const std::string& rid) const;

private:
  mutable std::mutex mx_;
  std::unordered_map<std::string, std::shared_ptr<INode>> m_;
};

} // namespace gma::ws

#include "gma/ws/RequestRegistry.hpp"
#include "INode.hpp"

namespace gma::ws {

void RequestRegistry::add(const std::string& rid, std::shared_ptr<INode> root){
  std::lock_guard<std::mutex> lk(mx_);
  m_[rid] = std::move(root);
}

void RequestRegistry::remove(const std::string& rid){
  std::shared_ptr<INode> n;
  {
    std::lock_guard<std::mutex> lk(mx_);
    auto it = m_.find(rid);
    if (it == m_.end()) return;
    n = std::move(it->second);
    m_.erase(it);
  }
  if (n) n->shutdown();
}

void RequestRegistry::removeAll(){
  std::unordered_map<std::string, std::shared_ptr<INode>> tmp;
  {
    std::lock_guard<std::mutex> lk(mx_);
    tmp.swap(m_);
  }
  for (auto& kv : tmp) if (kv.second) kv.second->shutdown();
}

bool RequestRegistry::exists(const std::string& rid) const{
  std::lock_guard<std::mutex> lk(mx_);
  return m_.count(rid) > 0;
}

} // namespace gma::ws

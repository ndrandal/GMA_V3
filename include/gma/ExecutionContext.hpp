// include/gma/ExecutionContext.hpp
#pragma once

#include "gma/AtomicStore.hpp"
#include "gma/ThreadPool.hpp"

namespace gma {

class ExecutionContext {
public:
  ExecutionContext(AtomicStore* store, ThreadPool* pool)
    : _store(store), _pool(pool) {}

  AtomicStore* store() const noexcept { return _store; }
  ThreadPool* pool() const noexcept { return _pool; }

private:
  AtomicStore* _store;
  ThreadPool* _pool;
};

} // namespace gma

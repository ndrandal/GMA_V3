// include/gma/ExecutionContext.hpp
#pragma once

#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"   // <-- IMPORTANT: rt::ThreadPool

namespace gma {

class ExecutionContext {
public:
  ExecutionContext(AtomicStore* store, rt::ThreadPool* pool)
    : _store(store), _pool(pool) {}

  AtomicStore* store() const noexcept { return _store; }
  rt::ThreadPool* pool() const noexcept { return _pool; }

private:
  AtomicStore*    _store = nullptr;
  rt::ThreadPool* _pool  = nullptr;
};

} // namespace gma

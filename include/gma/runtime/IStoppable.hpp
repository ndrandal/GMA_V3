#pragma once
namespace gma::rt {
struct IStoppable {
  virtual ~IStoppable() = default;
  virtual void stop() = 0; // idempotent
};
} // namespace gma::rt

#pragma once
#include "runtime.hpp"
#include <atomic>
namespace ios {
struct Asset {
  std::string_view route, type;
  const unsigned char *data;
  std::size_t size;
};
const std::vector<Asset> &assets();
class Web {
  Runtime &runtime_;
  std::atomic<bool> stopping_{};
  std::function<Binding()> binding_source_;
  std::thread worker_;
  void run();

public:
  // Authored fixtures can supply interface transitions without changing a NIC.
  explicit Web(Runtime &r, std::function<Binding()> source = {})
      : runtime_(r), binding_source_(std::move(source)), worker_([this] { run(); }) {}
  ~Web() {
    stopping_ = true;
    if (worker_.joinable())
      worker_.join();
  }
};
} // namespace ios

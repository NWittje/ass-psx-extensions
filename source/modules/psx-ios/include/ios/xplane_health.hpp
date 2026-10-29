#pragma once
#include <psx/health/consumer.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace ios {
// IOS interpretation of read-only health evidence. Times are local monotonic ms;
// remote ages retain their original provenance and continue aging in this cache.
class XplaneHealthModel final {
  struct Data;
  std::unique_ptr<Data> data_;
public:
  XplaneHealthModel();
  ~XplaneHealthModel();
  bool status(std::string_view, std::uint64_t received_ms);
  bool logs(std::string_view);
  void disconnected();
  bool history_supported() const;
  bool has_more() const;
  psx::health::LogCursor cursor() const;
  std::string snapshot(std::uint64_t now_ms) const;
};

// A single worker owns ConsumerClient. HTTP and the fast PSX readout only copy
// cached results; pipe deadlines and reconnection never run on those threads.
class XplaneHealth final {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  explicit XplaneHealth(std::wstring endpoint = {}, std::wstring server_image = L"psx-xp-coordinator.exe");
  ~XplaneHealth();
  std::string snapshot() const;
};
}

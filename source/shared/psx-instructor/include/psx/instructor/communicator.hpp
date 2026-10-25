#pragma once
#include <psx/network/io.hpp>
#include <functional>
#include <optional>
#include <string_view>

namespace psx::instructor {
using Tick = std::uint64_t; // steady-clock milliseconds
Tick now();
std::optional<int> integer(std::string_view, unsigned token = 0);
struct Framer {
  std::string pending;
  bool dropping{};
  unsigned oversized{};
  void feed(std::string_view, const std::function<void(std::string_view)> &);
  void reset() { pending.clear(); dropping = false; }
};
// Single-owner TCP communication; reconnection policy belongs to the application.
class PsxCommunicator {
  network::Stream stream_;
  Framer framer_;
public:
  bool connect(const network::Adapter &, const network::Endpoint &);
  void close();
  void pump();
  void read(const std::function<void(std::string_view)> &);
  bool send(std::string_view);
  network::State state() const { return stream_.state(); }
  int error() const { return stream_.error(); }
  bool sending() const { return stream_.sending(); }
  HANDLE event() const { return stream_.event(); }
};
}

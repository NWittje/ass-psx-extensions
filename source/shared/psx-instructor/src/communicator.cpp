#include <psx/instructor/communicator.hpp>
#include <charconv>
#include <chrono>

namespace psx::instructor {
Tick now() {
  return Tick(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::optional<int> integer(std::string_view s, unsigned token) {
  while (token--) {
    auto p = s.find(';');
    if (p == std::string_view::npos) return {};
    s.remove_prefix(p + 1);
  }
  s = s.substr(0, s.find(';'));
  int value{};
  auto result = std::from_chars(s.data(), s.data() + s.size(), value);
  if (result.ec != std::errc{} || result.ptr != s.data() + s.size()) return {};
  return value;
}
void Framer::feed(std::string_view bytes, const std::function<void(std::string_view)> &record) {
  for (char c : bytes) {
    if (c == '\n') {
      if (!dropping) {
        if (!pending.empty() && pending.back() == '\r') pending.pop_back();
        record(pending);
      }
      reset();
    } else if (!dropping) {
      if (pending.size() == 100000) { ++oversized; pending.clear(); dropping = true; }
      else pending += c;
    }
  }
}
bool PsxCommunicator::connect(const network::Adapter &adapter, const network::Endpoint &endpoint) {
  framer_.reset();
  return stream_.connect(adapter, endpoint);
}
void PsxCommunicator::close() { stream_.close(); framer_.reset(); }
void PsxCommunicator::pump() { stream_.pump(); }
void PsxCommunicator::read(const std::function<void(std::string_view)> &record) {
  auto bytes = stream_.received();
  framer_.feed({reinterpret_cast<const char *>(bytes.data()), bytes.size()}, record);
  stream_.consume(bytes.size());
}
bool PsxCommunicator::send(std::string_view bytes) {
  return stream_.send({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}
}

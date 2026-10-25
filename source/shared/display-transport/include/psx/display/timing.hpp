#pragma once
#include "transport.hpp"
#include <filesystem>
namespace psx::du::transport {
// Explicit capture only; independent DPT1 lease and collector worker.
class TimingCapture {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 TimingCapture(std::filesystem::path,uint32_t pid,uint64_t created);
 ~TimingCapture();TimingCapture(const TimingCapture&)=delete;
 void request_stop() noexcept;
 bool failed()const noexcept;
};
}

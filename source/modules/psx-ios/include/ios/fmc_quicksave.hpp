#pragma once
#include "core.hpp"
#include <psx/situation/fmc_native_service.hpp>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace ios {
class FmcQuicksaveStore {
  std::filesystem::path file_;
  psx::fmc::native_records::Profile profile_;
public:
  explicit FmcQuicksaveStore(std::filesystem::path directory);
  std::vector<std::uint8_t> read() const;
  void write(std::span<const std::uint8_t>);
  const std::filesystem::path& path() const { return file_; }
};

// One owner for the shared native client and binary file I/O. HTTP and the Main
// readout worker only exchange small snapshots/jobs with this worker.
class FmcQuicksave {
public:
  struct State {
    bool save_available{},recall_available{},saved_file{},busy{};
    std::string save_reason,recall_reason;
  };
private:
  struct Job {std::uint64_t id{};bool recall{};psx::situation::ProcessIdentity target{};};
  struct Outcome {std::uint64_t id{};std::string state,detail,result{"null"};};
  FmcQuicksaveStore store_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool ending_{},main_ready_{},busy_{},file_valid_{};
  psx::situation::ProcessIdentity target_{};
  Tick checked_{};
  std::uint32_t capabilities_{};
  std::string reason_{"FMC service unavailable"},file_reason_{"No FMC quicksave saved"};
  std::optional<Job> job_;
  std::optional<Outcome> outcome_;
  std::thread worker_;
  State state_locked() const;
  void run();
public:
  explicit FmcQuicksave(std::filesystem::path directory);
  ~FmcQuicksave();
  FmcQuicksave(const FmcQuicksave&)=delete;
  FmcQuicksave& operator=(const FmcQuicksave&)=delete;
  void context(bool ready,psx::situation::ProcessIdentity);
  State state() const;
  std::string json() const;
  bool busy() const;
  void command(Operation&,bool first);
  void apply(Engine&) const;
};
}

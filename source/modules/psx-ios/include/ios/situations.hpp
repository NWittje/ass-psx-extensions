#pragma once
#include "core.hpp"
#include <psx/situation/protocol.hpp>
#include <psx/hwio/situation_controls.hpp>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace ios {
struct SituationEntry {
  std::string id,name,description,hash,error;
  std::uintmax_t bytes{};
  std::filesystem::file_time_type modified{};
};
struct SituationSelection {std::string id,hash;};
struct SituationRecycleItem {std::filesystem::path path;std::string hash;};
struct SituationRecycleResult {std::vector<std::size_t> recycled;std::string error;};
using SituationRecycler=std::function<SituationRecycleResult(const std::vector<SituationRecycleItem>&)>;
SituationRecycleResult recycle_situations(const std::vector<SituationRecycleItem>&);
bool situation_recycle_allowed(const SituationRecycleItem&,std::uint32_t flags,std::filesystem::path source);
// File work is owned by the situation worker. Native payloads retain every
// unknown PSX field; only the native description block is edited on Save.
class SituationStore {
  std::filesystem::path root_;
public:
  explicit SituationStore(std::filesystem::path root):root_(std::move(root)){}
  static std::filesystem::path from_database(std::filesystem::path);
  static std::string filename(std::string_view);
  static std::string file_hash(const std::filesystem::path&);
  static std::string description(std::span<const std::uint8_t>);
  static std::vector<std::uint8_t> describe(std::span<const std::uint8_t>,std::string_view);
  std::vector<SituationEntry> catalogue(const std::vector<SituationEntry>& previous={}) const;
  std::vector<std::uint8_t> read(std::string_view id) const;
  void write(std::string_view id,std::span<const std::uint8_t>,std::string_view expected_hash={}) const;
  SituationRecycleResult recycle(const std::vector<SituationSelection>&,const SituationRecycler& recycler={}) const;
  const auto& root() const {return root_;}
};
struct SituationCheck {std::string id,label,target;bool available{},match{};std::string source;};
struct SituationLevers {
  std::optional<int> gear,flaps;
  std::optional<std::array<int,4>> throttles;
  void record(std::string_view);
  std::vector<SituationCheck> compare(const psx::situation::Snapshot&,const std::optional<psx::hwio::situation_controls::Snapshot>& physical={}) const;
};
void validate_situation_command(std::string_view,const Object&);
std::vector<SituationSelection> resolve_situation_deletion(const Object&,std::span<const SituationEntry>,std::uint64_t catalogue_revision);

class Situations {
  struct Job {std::uint64_t id{};bool load{},remove{};Object input;psx::situation::ProcessIdentity target;std::filesystem::path database;std::vector<SituationSelection> deletion;};
  struct Outcome {std::uint64_t id{};std::string state,detail,result{"null"};};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool ending_{},ready_{},busy_{},can_cancel_{};
  psx::situation::ProcessIdentity target_{};
  std::filesystem::path database_,root_;
  SituationLevers levers_;
  std::optional<psx::hwio::situation_controls::Snapshot> physical_;
  std::vector<SituationEntry> entries_;
  std::optional<Job> job_;
  std::optional<Outcome> outcome_;
  std::uint64_t revision_{1},cancel_{};
  Tick checked_{};
  std::uint32_t capabilities_{};
  std::string reason_{"Situation service unavailable"},folder_error_{"PSX situation folder unavailable"};
  std::string last_kind_;
  SituationRecycler recycler_;
  std::thread worker_;
  void run();
public:
  explicit Situations(SituationRecycler recycler={});~Situations();
  void context(bool,psx::situation::ProcessIdentity,std::filesystem::path database);
  void record(std::string_view);
  void physical(std::optional<psx::hwio::situation_controls::Snapshot>);
  bool busy() const;
  std::string status() const;
  std::string catalogue() const;
  void command(Operation&,bool first);
  void apply(Engine&) const;
  void cancel(std::uint64_t operation);
};
}

#pragma once
#include "core.hpp"
#include "flightillusion.hpp"

namespace hwio::flightillusion {
// Needles use PSX ECON/DEMAND observations until their connection/load boundary.
// The caller samples PC UTC once and derives Windows local calendar time from
// that same instant. Each clock's persisted source choice selects PSX/UTC/local.
// ExporterSource then adds the independently timestamped maximum-speed channel.
Sources host_sources(const Mapper&, std::uint64_t sequence, std::uint64_t monotonic_ms,
                     std::int64_t pc_utc_epoch_ms,std::optional<std::int64_t> pc_local_epoch_ms);
const std::vector<RecordKey>& simulator_interests();
class Connection {
    bool enabled_{};std::uint64_t next_{},searched_{},search_after_{};
public:
    void update(Provider&,bool enabled,std::uint64_t monotonic_ms);
};
class Inputs {
    std::string ownership_;std::uint64_t epoch_{},bus_{},sequence_{},baro_sequence_{};
    std::optional<double> baro_;
    bool active_{};
    std::vector<std::uint64_t> cancelled_;
public:
    std::vector<hwio::Command> update(const Mapper&,const View&,std::span<const InputEvent>,
                                     const std::set<std::string>& blocked,std::uint64_t monotonic_ms,std::int64_t qpc,std::int64_t frequency);
    std::vector<std::uint64_t> cancellations(){auto out=std::move(cancelled_);cancelled_.clear();return out;}
};
}

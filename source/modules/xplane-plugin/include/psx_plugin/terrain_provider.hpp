#pragma once
#include "sdk_api.hpp"
#include "terrain_types.hpp"
#include "motion_support.hpp"
#include <XPLMScenery.h>

namespace psx::xp {
// SDK owner only. Geographic requests are independent of MotionStatus/display pose.
class TerrainProvider final {
public:
    bool begin(SdkApi&,motion::InstanceId,std::uint64_t frequency,
               std::int64_t (*clock)() noexcept) noexcept;
    void retire() noexcept;
    void lifecycle(std::uint64_t aircraft,std::uint64_t scenery,bool loading) noexcept;
    void profile(const MotionProfile&) noexcept;
    bool tick(const TerrainInput&,TerrainResult&) noexcept;
    const TerrainHost& host() const noexcept {return host_;}
    terrain::Reason diagnostics_reason() const noexcept {return terrain::Reason::unsupported;}
private:
    friend class TerrainCapture;
    SdkApi* sdk_{};
    XPLMProbeRef probe_{};
    motion::InstanceId provider_{};
    TerrainHost host_{};
    std::uint64_t frequency_{},last_connection_{},last_revision_{},last_lease_{};
    std::int64_t (*clock_)() noexcept{};
    std::int64_t last_probe_{},invalidated_qpc_{};
    terrain::Reason invalidation_reason_{terrain::Reason::scenery};
    motion::Vec3 origin_{},origin_north_{},origin_east_{};
    bool origin_known_{},probed_{},pending_{},seed_known_{};
    TerrainInput frozen_{};
    TerrainResult refinement_{};
    MotionProfile profile_{};
    XPLMDataRef cg_y_{},cg_z_{};
    struct Station {
        terrain::Query target{};
        double seed{},east{},north{};
        motion::Vec3 hit{},normal{};
        bool cold{},complete{};
    };
    std::array<Station,5> stations_{};
    std::array<terrain::Surface,5> seeds_{};
    std::array<motion::Vec3,5> seed_hits_{},seed_normals_{};
    terrain::Control domain_{};
    std::uint64_t last_query_id_{};
    bool local_frame() noexcept;
    void invalidate_refinement() noexcept;
    bool fresh(const terrain::Reference&,std::int64_t) const noexcept;
    bool geometry_valid() noexcept;
    bool freeze(const TerrainInput&,TerrainResult&) noexcept;
    double warm_seed(const terrain::Reference&,unsigned) noexcept;
    bool query(const terrain::Query&,double,terrain::Surface&,motion::Vec3&,motion::Vec3&,double&,terrain::ProbeAttempt&,XPLMProbeRef=nullptr) noexcept;
};
}

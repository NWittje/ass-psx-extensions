#pragma once
#include "catalogue.hpp"
#include "hid.hpp"
#include <deque>
#include <functional>

namespace hwio {
struct CockpitBinding {
    std::string device;
    unsigned a{}, b{UINT16_MAX};
    bool enabled{}, software{}, inverted{};
    unsigned counts{1};
    int minimum{}, maximum{4095};
    double hysteresis{.01};
    std::vector<double> thresholds;
    bool operator==(const CockpitBinding&) const = default;
};
struct CockpitRow {CockpitBinding applied,draft;std::string pending;std::vector<CockpitBinding> module_sources;}; // Compiled CDU fan-in only; never generic persisted sources.
struct CockpitConfiguration {std::map<std::string,CockpitRow> rows;};
bool cockpit_valid(const CockpitConfiguration&,std::string&);
std::string cockpit_encode(const CockpitConfiguration&);
bool cockpit_decode(std::string_view,CockpitConfiguration&,std::string&);
struct CockpitReadout {std::string id,reason;bool available{};int raw{},desired{};};
// A/B transition table consumes a coherent complete device report. Invalid jumps
// discard phase history; they never guess a direction or a missed transition.
class Quadrature {
    int previous_{-1};
public:
    void reset() noexcept {previous_=-1;}
    int report(bool valid,int a,int b) noexcept;
    bool known() const noexcept {return previous_>=0;}
};
class CockpitRouter {
    struct Route {
        const CockpitDefinition* definition{};
        CockpitBinding binding;
        int device{-1},a{-1},b{-1};
        std::uint64_t generation{},mapping{};
        std::int64_t remainder{};
        Quadrature quadrature;
        Filter selector_filter;
        std::uint64_t sequence{};
        std::vector<SourceValue> source{SourceValue{}};
        CockpitReadout view;
        bool shared_cdu{},movement_known{};int movement_raw{};
    };
    struct Turn {std::size_t route{};int count{};std::int64_t qpc{};};
    std::vector<Route> routes_;
    std::deque<Turn> turns_;
    std::map<std::string,double> last_turn_;
    std::uint64_t epoch_{};
    std::set<std::string> override_encoders_;
    struct SharedCdu {std::size_t route{};std::int64_t qpc{};std::uint64_t sequence{};};
    std::map<std::string,SharedCdu> shared_cdu_owner_;
    std::function<bool(const std::vector<std::string>&)> capture_;
    void turn(std::size_t,int,std::int64_t,Mapper&);
public:
    void capture_handler(std::function<bool(const std::vector<std::string>&)> handler){capture_=std::move(handler);}
    std::vector<std::uint64_t> overrides(const std::set<std::string>&);
    bool overflow{};
    void configure(const CockpitConfiguration&,const std::vector<DeviceInfo>&,Mapper&,const std::set<std::string>& module_devices={});
    void boundary(std::uint64_t) noexcept;
    void input(const Sample&,Mapper&);
    void report(std::uint16_t,const SourceBank&,Mapper&);
    void update(const SourceBank&,Mapper&,double,int);
    std::optional<Command> next(Mapper&,double,std::int64_t);
    std::vector<CockpitReadout> snapshot(const Mapper&) const;
    std::vector<RecordKey> interests() const;
    std::vector<std::uint64_t> command_ids() const;
};
bool cockpit_destination_overlap(const TargetDescriptor&,const TargetDescriptor&) noexcept;
bool cockpit_source_overlap(const Source&,bool encoder,const CockpitBinding&);
} // namespace hwio

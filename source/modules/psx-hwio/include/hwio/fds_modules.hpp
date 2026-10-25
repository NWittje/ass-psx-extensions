#pragma once
#include "fds.hpp"
#include "catalogue.hpp"
#include "digital_outputs.hpp"
#include "cockpit.hpp"
#include <functional>

namespace hwio {
// Complete-module routing consumes provider metadata, not an SDK board object.
struct ModuleDevice {
    DeviceInfo input;
    std::string key,model,status,parent;
    unsigned update{};
    std::vector<HardwareResource> resources;
    bool baseline{},generic{},readouts_known{};
    bool addressed_windows{}; // True: one resource per display, rather than per digit.
};
std::vector<ModuleDevice> module_devices(const std::vector<fds::Device>&);
using ModuleOutput=fds::Output; // Existing value-only slot/generation/intent contract.
using ModuleSink=std::function<bool(const ModuleOutput&)>;
struct AxisConfiguration;
struct ModuleInput {
    unsigned channel{};std::string action, name, association;
    // Explicit contact truth table. A state is a bit mask over contacts, not a
    // claim that individual SDK callbacks form a coherent hardware report.
    std::vector<unsigned> contacts,states;
};
struct ModuleAnalogue {unsigned channel{},minimum{},maximum{4095},bits{12};std::string definition,name,association;bool inverted{},optional{},rotary{};};
struct ModuleLamp {unsigned channel{};std::string output, name, association;};
struct ModuleActuator {unsigned channel{};std::string output,name,association;};
struct ModuleDisplay {unsigned channel{},width{};std::string display, name, association;bool loss_pattern{};};
struct ModuleUnused {unsigned channel{};std::string name, reason;};
struct ModuleAssociation {std::string id,model,role;bool required{};};
struct ModuleProfile {
    std::string id,name,model,reference,unavailable;
    unsigned version{},minimum_update{},maximum_update{UINT16_MAX};
    BackendKind backend{BackendKind::fds_usb};
    std::vector<std::string> roles;
    std::vector<ModuleInput> inputs;
    std::vector<ModuleAnalogue> analogue;
    std::vector<ModuleLamp> lamps;
    std::vector<ModuleActuator> actuators; // Qualified Boolean datalines; never lighting/test banks.
    std::vector<ModuleDisplay> displays;
    std::vector<ModuleUnused> unused;
    std::vector<ModuleAssociation> associations;
    bool input_only{};
    std::string category,confidence{"complete"},limitations;
    std::vector<unsigned> identification_lamps;
    bool identification_brightness{};
};
struct ModuleLibrary {unsigned version{1};std::string revision;std::vector<ModuleProfile> profiles;std::string problem;};
ModuleLibrary read_modules(const std::filesystem::path&);
void write_modules(const ModuleLibrary&,const std::filesystem::path&);
std::size_t merge_modules(ModuleLibrary&,const ModuleLibrary&);
// Explicit offline preparation: unify stock 331A CDU profiles and qualify the
// separate 3305 update1/update2 output banks. Custom mappings/assignments survive.
std::pair<std::size_t,std::size_t> merge_fds_display_definitions(Catalogue&,ModuleLibrary&,const Catalogue&,const ModuleLibrary&);
struct ModuleChoice {
    std::string profile,role;
    unsigned version{};
    std::map<std::string,std::string> associations;
    bool additional_cdu{};
    bool operator==(const ModuleChoice&) const = default;
};
struct ModuleRow {std::string device,model;unsigned update{};ModuleChoice applied,draft;std::string pending;};
struct ModuleConfiguration {std::map<std::string,ModuleRow> rows;std::map<std::string,bool> automatic;std::set<std::string> additional_cdu_roles;};
std::string modules_encode(const ModuleConfiguration&);
bool modules_decode(std::string_view,ModuleConfiguration&,std::string&);
bool modules_valid(const ModuleConfiguration&,std::string&);
const ModuleProfile* module_profile(const ModuleLibrary&,std::string_view) noexcept;
std::string module_reference(const ModuleProfile&,const ModuleChoice&,std::string,std::string_view association={});
struct ModuleView {
    std::string device,name,model,connection,profile,role,completeness,inspection,source,parent;
    unsigned update{};
    BackendKind backend{BackendKind::fds_usb};
    bool connected{},applied{},pending{},eligible{},readouts_known{};
};
struct ModuleCompilation {
    ButtonConfiguration buttons;
    CockpitConfiguration cockpit;
    std::set<std::string> devices;
    std::vector<ModuleView> views;
    std::string problem;
};
ModuleCompilation compile_modules(const ModuleLibrary&,const ModuleConfiguration&,const std::vector<fds::Device>&,
                                 const Catalogue&,const ButtonConfiguration&,const CockpitConfiguration&,
                                 const HardwareConfiguration&,const std::vector<Profile>&,const AxisConfiguration* axes=nullptr);
bool module_conflicts(const ModuleLibrary&,const ModuleConfiguration&,const ButtonConfiguration&,
                      const CockpitConfiguration&,const HardwareConfiguration&,const std::vector<Profile>&,std::string&,const AxisConfiguration* axes=nullptr);
bool format_display(const CatalogueDisplay&,const Catalogue&,const Mapper&,unsigned width,std::string&,std::uint64_t test_seconds=0);
struct ModuleAutoChoice {std::string category,role,device,profile;};
std::vector<ModuleAutoChoice> module_auto_choices(const ModuleLibrary&,const ModuleConfiguration&,const std::vector<fds::Device>&);
ModuleCompilation compile_module_devices(const ModuleLibrary&,const ModuleConfiguration&,const std::vector<ModuleDevice>&,const Catalogue&,const ButtonConfiguration&,const CockpitConfiguration&,const HardwareConfiguration&,const std::vector<Profile>&,const AxisConfiguration* axes=nullptr);
std::vector<ModuleAutoChoice> module_device_auto_choices(const ModuleLibrary&,const ModuleConfiguration&,const std::vector<ModuleDevice>&);
class ModuleOutputs {
    struct Route {
        std::string device,id;
        std::uint16_t slot{},channel{},width{};
        std::uint64_t generation{};
        bool display{},pattern{},current{};
        ResourceKind kind{ResourceKind::boolean_output};
        HardwareOutputIntent previous;
        bool submitted{};
    };
    std::vector<Route> routes_;
    std::vector<fds::Output> releases_;
    DigitalOutputs digital_;
    const Catalogue* catalogue_{};
    std::uint64_t sequence_{};
    std::map<std::string,bool> tests_;
    std::string identifying_;std::vector<unsigned> identification_lamps_;
public:
    void configure_devices(const ModuleLibrary&,const ModuleConfiguration&,const ModuleCompilation&,const std::vector<ModuleDevice>&,const Catalogue&,const ModuleSink&);
    void update_to(const Mapper&,std::uint64_t,const ModuleSink&,const DigitalOutputs* evaluated=nullptr);
    void release_to(const ModuleSink&);
    void identification(std::string,std::span<const unsigned>);
    void configure(const ModuleLibrary&,const ModuleConfiguration&,const ModuleCompilation&,const std::vector<fds::Device>&,
                   const Catalogue&,fds::Provider&);
    void update(const Mapper&,std::uint64_t seconds,fds::Provider&,const DigitalOutputs* evaluated=nullptr);
    std::string source_status(std::string_view,bool ready) const;
    void release(fds::Provider&);
    bool test(std::string_view device,bool on);
    void clear_tests() noexcept {tests_.clear();}
};
} // namespace hwio

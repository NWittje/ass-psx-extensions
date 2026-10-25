#pragma once
#include "button_setup.hpp"
#include <filesystem>
#include <stdexcept>

namespace hwio {
// XML is configuration, never part of the input-processing path. The production
// instance is constructed once and remains at a stable address until process exit.
struct CatalogueTarget {
    unsigned source_line{};
    TargetDescriptor descriptor;
    std::string role, reference, label, reverse_label;
    bool numeric_fields{};
    std::vector<int> detents;
};
enum class ObservationEncoding {number, text, character, decimal_slice};
struct CatalogueObservation {
    unsigned source_line{};
    std::string id, name, unit, reference;
    RecordKey record{};
    ObservationEncoding encoding{ObservationEncoding::number};
    unsigned field{}, fields{1}, character{};
    unsigned width{}, payload_minimum{}, payload_maximum{}, guard_character{}, guard_value{};
    int minimum{INT32_MIN}, maximum{INT32_MAX};
    std::uint32_t mask{};
    unsigned shift{};
    bool motor_target{}, demand{};unsigned divisor{1};std::string power_record;std::uint32_t power_mask{};
};
enum class DigitalTest {positive, any, not_all, clear, equal, greater, valid, at_most};
enum class DigitalEncoding {numbers, trailing_numbers, characters, digits, mixed_fields};
struct DigitalOperand {
    RecordKey record{};
    unsigned field{}, fields{1};
    int minimum{}, maximum{INT32_MAX}, value{};
    std::uint32_t mask{};
    DigitalTest test{DigitalTest::positive};
    DigitalEncoding encoding{DigitalEncoding::numbers};
    unsigned length_min{}, length_max{};
    std::string prefix, allowed;
};
struct CatalogueDigitalOutput {
    unsigned source_line{};
    int chapter{};
    std::string id, name, reference, kind, unavailable;
    bool qualified{};
    std::vector<std::vector<DigitalOperand>> branches;
    std::vector<DigitalOperand> requirements;
    std::string requirement_reason;
    std::vector<std::pair<RecordKey,std::vector<int>>> applicability;
};
struct CockpitDefinition {
    unsigned source_line{};
    int chapter{};
    bool encoder{};
    std::string id, name, reference, group, decrease, increase;
    TargetDescriptor target;
    int baseline_minimum{},baseline_maximum{};
    std::vector<int> detents;
    std::vector<std::pair<RecordKey,std::vector<int>>> applicability;
};
struct CatalogueDisplay {
    unsigned source_line{};
    std::string id,name,reference,observation,power,mode,style;
    unsigned width{};
};
struct CatalogueGroup {
    unsigned source_line{};
    std::string id, name;
    std::string panel, coordinate;
    int chapter{};
    bool breakers{};
    std::vector<CatalogueTarget> targets;
    std::vector<ButtonAction> actions;
    std::vector<CatalogueObservation> observations;
    std::vector<CatalogueDigitalOutput> digital_outputs;
    std::vector<CockpitDefinition> cockpit;
    std::vector<CatalogueDisplay> displays;
};
struct Catalogue {
    unsigned schema_version{1};
    std::string revision;
    std::map<int, std::string> chapters;
    std::vector<CatalogueGroup> groups;
    RecordKey authority_record{};
    unsigned authority_field{};
    std::vector<int> physical_modes, servo_modes;
    // Finalized, immutable indexes. No pointers into temporary XML strings.
    std::vector<TargetDescriptor> targets;
    std::vector<ButtonAction> actions;
    std::map<std::string, std::size_t, std::less<>> target_ids, action_ids, group_ids;
    std::map<std::string, std::vector<std::size_t>, std::less<>> group_actions;
    std::map<int, std::vector<std::size_t>> chapter_actions;
    std::array<const CatalogueTarget *, flight_slots> roles{};
    std::array<std::wstring, flight_slots> labels{};
    std::map<std::string, std::vector<const TargetDescriptor *>, std::less<>> numeric_records;
    std::set<std::string, std::less<>> observed_controls;
    std::map<std::string,const CatalogueObservation*,std::less<>> observations;
    std::map<std::string,const CatalogueDigitalOutput*,std::less<>> digital_outputs;
    std::map<std::string,const CockpitDefinition*,std::less<>> cockpit;
    std::map<std::string,const CatalogueDisplay*,std::less<>> displays;
    std::string authority_prefix;
    double load_ms{};
    unsigned validation_line{};
    void finalize();
    const ButtonAction *action(std::string_view) const noexcept;
    const TargetDescriptor *target(std::string_view) const noexcept;
    AtAuthority authority(std::string_view payload) const noexcept;
};
class CatalogueError : public std::runtime_error {
  public:
    bool missing;
    explicit CatalogueError(std::string detail, bool absent = false)
        : std::runtime_error(std::move(detail)), missing(absent) {}
    const wchar_t *popup() const noexcept {
        return missing ? L"Error: catalogue.xml missing. Please repair PSX-HWIO installation."
                       : L"Error: catalogue.xml invalid. Please repair PSX-HWIO installation.";
    }
};
std::unique_ptr<Catalogue> read_catalogue(const std::filesystem::path &);
void write_catalogue(const Catalogue &, const std::filesystem::path &);
std::size_t merge_digital_catalogue(Catalogue &, const Catalogue &);
const Catalogue &active_catalogue();
// Offline replay selects its captured catalogue before any catalogue consumer.
void catalogue_source(const std::filesystem::path&);
const CatalogueTarget *flight_target(std::size_t slot) noexcept;
double flight_fraction(std::size_t slot, int value) noexcept;
int flight_detent(int position) noexcept;
int flight_detent_index(int value) noexcept;
bool catalogue_record_valid(const TargetDescriptor &, const std::vector<std::string_view> &) noexcept;
// Rebind only known legacy catalogue IDs. Bespoke legacy descriptors retain their semantics.
void resolve_catalogue_target(TargetDescriptor &);
bool catalogue_target_missing(const TargetDescriptor &) noexcept;
std::string catalogue_fingerprint(const Catalogue &);
} // namespace hwio

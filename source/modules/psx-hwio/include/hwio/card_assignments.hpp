#pragma once
#include "storage.hpp"
#include <filesystem>

namespace hwio {
// Stable controller identity, independent of its current network/USB path.
struct CardIdentity {
    BackendKind provider{};
    std::string key,model,serial,name;
    bool operator==(const CardIdentity&)const=default;
};
struct CardAssignments {
    std::vector<Profile> profiles;
    DisplayAssignments display;
    AxisConfiguration axes;
    ButtonConfiguration buttons;
    HardwareConfiguration hardware;
    CockpitConfiguration cockpit;
    ModuleConfiguration modules;
};
struct CardArchive {
    CardIdentity identity;
    std::unique_ptr<CardAssignments> assignments{std::make_unique<CardAssignments>()};
};
enum class CardMatch {exact,model_only,incompatible};
struct CardImport {
    CardIdentity source,target;
    CardMatch match{CardMatch::incompatible};
    std::string preimage;
    std::unique_ptr<CardAssignments> replacement;
};
std::string card_assignments_signature(const CardAssignments&);
CardMatch card_match(const CardIdentity&,const CardIdentity&)noexcept;
std::string fds_card_serial(std::string_view,BackendKind);
bool card_profile(const Profile&,std::string_view)noexcept;
std::unique_ptr<CardAssignments> card_slice(const CardAssignments&,const CardIdentity&);
// Pure merge: failure leaves both arguments unchanged. Only the selected card
// I/O is removed/replaced; flight axes, card settings and actuator objects are
// retained. A conflicting destination on another card is rejected.
std::unique_ptr<CardAssignments> card_merge(const CardAssignments&,const CardArchive&,const CardIdentity&,std::string&);
bool card_assignments_valid(const CardAssignments&,std::string&);
bool write_card_archive(const std::filesystem::path&,const CardArchive&,std::string&);
bool read_card_archive(const std::filesystem::path&,CardArchive&,std::string&);
} // namespace hwio

#pragma once
#include "catalogue.hpp"
#include <mutex>

namespace hwio {
struct InputOverrideControl {
    std::string id,name;
    int chapter{};
    bool encoder{},assigned{};
    std::vector<std::string> actions;
};
struct InputOverrideConfiguration {
    std::set<std::string> controls;
    bool operator==(const InputOverrideConfiguration&) const = default;
};
struct InputOverrideView {
    InputOverrideConfiguration configuration;
    std::uint64_t revision{};
    bool capturing{},valid{true};
    std::string status;
};
std::vector<InputOverrideControl> input_override_controls(const Catalogue&);
std::string input_overrides_encode(const InputOverrideConfiguration&);
bool input_overrides_decode(std::string_view,InputOverrideConfiguration&,std::string&);
class InputOverrides {
    mutable std::mutex mutex_;
    std::wstring directory_;
    InputOverrideView view_;
public:
    void load(const std::wstring&);
    InputOverrideView snapshot() const;
    bool replace(InputOverrideConfiguration,std::string&,std::uint64_t expected_revision=UINT64_MAX);
    void capture(bool);
    void status(std::string);
    std::uint64_t revision() const {std::lock_guard lock(mutex_);return view_.revision;}
    bool capturing() const {std::lock_guard lock(mutex_);return view_.capturing;}
};
// Worker-owned compiled lookup. Acquisition callbacks do not call this object.
class InputOverridePolicy {
    std::vector<InputOverrideControl> controls_;
    std::map<std::string,std::string> actions_,encoders_;
    std::set<std::string> blocked_actions_,blocked_encoders_;
    bool invalid_{};
public:
    explicit InputOverridePolicy(const Catalogue&);
    const std::vector<InputOverrideControl>& controls() const noexcept{return controls_;}
    void configure(const InputOverrideView&);
    std::string button_control(std::string_view) const;
    std::string encoder_control(std::string_view) const;
    bool button_blocked(std::string_view) const;
    bool encoder_blocked(std::string_view) const;
    const std::set<std::string>& blocked_actions() const noexcept{return blocked_actions_;}
    const std::set<std::string>& blocked_encoders() const noexcept{return blocked_encoders_;}
};
}

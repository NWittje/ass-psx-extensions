#pragma once
#include "runtime.hpp"
#include "hardware_panel.hpp"
#include "output_identification_panel.hpp"
#include "flightillusion_panel.hpp"
#include <ass_graphics/graphics.hpp>
#include <application-icon.hpp>
#include <future>
#include <optional>
namespace hwio {
class ModulePanel {
    friend struct ModulePanelProbe;
    struct Row {std::string key,category,role;std::vector<ass::graphics::Control> controls;std::vector<std::pair<std::string,std::string>> choices;std::vector<std::wstring> labels;std::wstring test_text{L"Test"},status_text;std::optional<bool> test_enabled;bool additional_cdu{};};
    struct InventoryRow {std::string key;unsigned group{};ass::graphics::Control label,inspect,assign_outputs;std::unique_ptr<ass::graphics::Canvas> marker;std::wstring text;};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};
    ass::graphics::Control pane_,message_,merge_,merge_profiles_,diagnostics_,output_cards_,output_launch_;std::array<ass::graphics::Control,6> groups_;
    ass::graphics::Control fi_open_,fi_status_;
    std::unique_ptr<ass::graphics::Window> fi_window_;
    std::optional<psx::windows::WindowIcons> fi_icons_;
    flightillusion::Panel fi_panel_;bool fi_visible_{};
    static void __cdecl fi_event(void*,std::uint32_t,std::uint32_t,double) noexcept;
    void show_flightillusion();void hide_flightillusion();
    ass::graphics::Control cp_port_label_,cp_port_,cp_refresh_,cp_connect_,cp_status_;
    std::vector<psx::hardware::SerialPort> cp_ports_;
    std::wstring cp_status_text_,cp_connect_text_;
    std::vector<std::string> output_card_keys_;std::string output_card_;std::map<std::string,std::wstring> role_errors_;
    ass::graphics::Control trim_heading_,trim_port_label_,trim_port_choice_,trim_identify_,trim_message_;
    std::array<std::array<ass::graphics::Control,3>,2> trim_roles_;
    std::vector<TrimPort> trim_ports_;std::wstring trim_selected_port_,trim_message_text_;
    std::array<std::vector<std::uint64_t>,2> trim_choices_;
    std::array<std::wstring,2> trim_role_status_;
    ass::graphics::Control fdstq_label_,fdstq_choice_,fdstq_setup_,fdstq_test_;
    HardwarePanel card_;
    OutputIdentificationPanel output_wizard_;
    std::vector<int> fdstq_serials_;std::vector<std::string> fdstq_output_ids_;
    std::unique_ptr<ass::graphics::Window> inspection_window_;
    std::optional<psx::windows::WindowIcons> inspection_icons_;ass::graphics::Control inspection_pane_,inspection_text_;
    CardTransferPanel transfer_;
    std::vector<Row> rows_;std::vector<InventoryRow> inventory_;ModuleConfiguration configuration_;RuntimeView view_;
    std::vector<EthernetHealth> retained_hardware_;std::vector<PhidgetDevice> retained_phidgets_;
    struct Event{unsigned id{},kind{};std::uint64_t generation{};std::string key;int selection{-1};};std::vector<Event> events_;
    std::string signature_,pending_signature_;std::uint64_t generation_{};bool rebuilding_{},visible_{},has_view_{},layout_dirty_{true};int width_{800},height_{664},x_{},y_{};bool arranged_{};unsigned layout_dpi_{};
    std::wstring message_text_,fdstq_test_text_{L"Test"};bool message_dirty_{true},diagnostics_enabled_{true},diagnostics_applied_{true};std::optional<bool> fdstq_test_enabled_;
    std::future<std::string> export_;std::vector<ass::graphics::Control> association_controls_;std::vector<std::vector<std::string>> association_choices_;std::string inspected_;std::vector<std::string> inspection_profiles_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double) noexcept;
    void rebuild();void layout();void inspect(const std::string&);void update_states();bool choice_open() const;void apply_visuals();void set_message(std::wstring);
    void configure_fdstq(bool show=true);
    void inspect_phidget(const std::string&);
    void inspect_generic(const std::string&);
    void refresh_phidget();
public:
    static constexpr unsigned dispatch_id=108;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void*);
    void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void dispatch();void clear_tests();void hide_inspection();
    void refresh_card(const RuntimeView& view){card_.refresh(view);}
};
}

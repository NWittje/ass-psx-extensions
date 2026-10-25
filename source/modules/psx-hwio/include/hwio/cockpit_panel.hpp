#pragma once
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
#include <deque>
namespace hwio {
class CockpitPanel {
    friend struct CockpitPanelProbe;
    static constexpr unsigned fields=15;
    struct Entry {std::string key,caption,group;int chapter{},y{},height{},caption_height{};const CockpitDefinition* definition{};};
    struct Row {
        std::array<ass::graphics::Control,fields> controls;
        std::array<std::wstring,fields> texts;
        std::array<int,fields> shown{},enabled{};
        std::array<double,fields> values;
        std::string key;std::uint64_t generation{},layout{},choices{UINT64_MAX};
        std::vector<std::string> devices;
        std::vector<unsigned> channels;
    };
    struct Event {unsigned id{},kind{};double value{};std::string key;std::uint64_t generation{};};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};
    ass::graphics::Control pane_,search_,clear_,up_,breadcrumb_,message_,list_,measure_;
    std::vector<Row> rows_;std::vector<Entry> entries_;
    std::map<int,std::vector<const CatalogueGroup*>> groups_;
    CockpitConfiguration configuration_;RuntimeView view_;
    std::map<std::pair<int,std::string>,float> positions_;
    std::deque<Event> events_;
    std::wstring query_;std::string group_;int chapter_{},width_{690},height_{664};
    bool encoder_{},rebuilding_{},realizing_{},visible_{};
    std::uint64_t generation_{},layout_{},choices_{},revision_{UINT64_MAX},inventory_{};
    unsigned dispatch_{};
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double) noexcept;
    void model();void realize();void bind(Row&,const Entry&);void commit(Row&,unsigned,double);
    void navigate(int,std::string);void keyboard(unsigned,int);
public:
    static constexpr unsigned encoder_dispatch=106,analogue_dispatch=107;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,bool,void*);
    void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void dispatch();
};
} // namespace hwio

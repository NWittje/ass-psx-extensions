#pragma once
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
#include <deque>

namespace hwio {
class OutputPanel {
    friend struct OutputPanelProbe;
    enum class Kind {chapter,group,output};
    struct Entry {const CatalogueDigitalOutput* output{};int y{},height{},caption_height{};Kind kind{Kind::output};int chapter{};std::string key,caption,group;};
    struct Row {
        std::array<ass::graphics::Control,6> controls;
        std::array<std::wstring,6> text;
        std::array<int,6> shown{-1,-1,-1,-1,-1,-1},enabled{-1,-1,-1,-1,-1,-1};
        std::array<unsigned,6> styles{UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX};
        std::uint64_t layout{UINT64_MAX};
        std::string id,key,device;
        std::uint64_t generation{};
        std::uint64_t choices{UINT64_MAX};
        std::vector<std::string> devices;
        std::vector<HardwareResource> resources;
    };
    struct Draft {std::string device;};
    struct Event {std::uint32_t id{},kind{};int selection{};std::string key;std::uint64_t generation{};};
    ass::graphics::Context* context_{};
    ass::graphics::Window* owner_{};
    Runtime* runtime_{};
    ass::graphics::Control pane_,list_,chapter_,search_,search_label_,clear_search_,up_,breadcrumb_,message_,import_,measure_;
    std::vector<Row> rows_;
    std::vector<Entry> entries_;
    std::map<int,std::vector<const CatalogueGroup*>> groups_;
    std::map<std::string,const CatalogueGroup*> output_groups_;
    std::map<std::pair<int,std::string>,float> positions_;
    std::string selected_group_;
    std::map<std::string,Draft> drafts_;
    std::map<std::string,bool> tests_;
    std::map<std::pair<int,std::string>,int> heights_;
    std::deque<Event> events_;
    HardwareConfiguration configuration_;
    RuntimeView view_;
    std::wstring query_;
    int selected_chapter_{},width_{690},height_{700};
    std::uint64_t generation_{},revision_{UINT64_MAX},inventory_{},choices_revision_{},layout_revision_{};
    UINT dpi_{};
    bool rebuilding_{},realizing_{},visible_{};
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double) noexcept;
    void model();
    void realize();
    void bind(Row&,const Entry&,std::size_t);
    static void show(Row&,unsigned,bool);
    static void enable(Row&,unsigned,bool);
    static void text(Row&,unsigned,const std::wstring&);
    static void style(Row&,unsigned,unsigned);
    void commit(Row&,unsigned,int);
    void merge_catalogue();
    void keyboard(unsigned,int);
    void navigate(int,const std::string&);
public:
    static constexpr unsigned dispatch_id=105;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void* parent=nullptr);
    void arrange(int,int,int,int,bool);
    void refresh(const RuntimeView&);
    void dispatch();
    void clear_tests();
};
} // namespace hwio

#include "hwio/input_override_panel.hpp"
#include <algorithm>
#include <cwctype>
#include <sstream>
namespace hwio {
namespace gfx=ass::graphics;
namespace {
std::wstring text(HWND hwnd){const int n=GetWindowTextLengthW(hwnd);std::wstring out(static_cast<std::size_t>(n)+1,L'\0');GetWindowTextW(hwnd,out.data(),n+1);out.resize(n);return out;}
std::wstring lower(std::wstring s){for(auto& c:s)c=static_cast<wchar_t>(std::towlower(c));return s;}
}
InputOverridePanel::~InputOverridePanel(){if(owner_)RemoveWindowSubclass(owner_,host_proc,1);if(pane_.handle())RemoveWindowSubclass(static_cast<HWND>(pane_.hwnd()),host_proc,1);if(search_.handle())RemoveWindowSubclass(static_cast<HWND>(search_.hwnd()),edit_proc,1);if(popup_){RemoveWindowSubclass(popup_,popup_proc,1);DestroyWindow(popup_);}}
void __cdecl InputOverridePanel::event(void* user,std::uint32_t id,std::uint32_t kind,double)noexcept{
    auto& p=*static_cast<InputOverridePanel*>(user);try{if(p.binding_)return;if(kind==AG_TEXT_CHANGED && id==3){p.selected_.clear();p.filter();}else if(kind==AG_CLICK)p.action(id);}catch(const std::exception& e){p.status_.text(wide(e.what()).c_str());}
}
void InputOverridePanel::attach(gfx::Context& c,Runtime& runtime,void* parent){
    context_=&c;runtime_=&runtime;controls_=input_override_controls(active_catalogue());catalogue_shape_={active_catalogue().actions.size(),active_catalogue().cockpit.size()};
    auto make=[&](unsigned kind,unsigned id,void* target,const wchar_t* caption){ag_control_desc d{};d.kind=kind;d.id=id;d.text=caption;d.bounds={0,0,1,1};return gfx::Control(c,target,d,event,this);};
    pane_=make(AG_PANEL,0,parent,L"");clear_=make(AG_BUTTON,1,pane_.hwnd(),L"Clear all");capture_=make(AG_BUTTON,2,pane_.hwnd(),L"Block next input");search_=make(AG_EDIT,3,pane_.hwnd(),L"");add_=make(AG_BUTTON,4,pane_.hwnd(),L"Add");status_=make(AG_LABEL,5,pane_.hwnd(),L"Type to find a button or encoder");list_=make(AG_PANEL,6,pane_.hwnd(),L"");
    ag_control_events(search_.handle(),AG_CONTROL_NOTIFY_TEXT|AG_CONTROL_NAV_TAB);
    const auto edit=static_cast<HWND>(search_.hwnd());SetWindowSubclass(edit,edit_proc,1,reinterpret_cast<DWORD_PTR>(this));
    // ASS layout uses DIPs. The native dropdown is an owned popup in screen
    // pixels, so sibling status/list controls cannot paint over its contents.
    owner_=GetAncestor(edit,GA_ROOT);
    popup_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"LISTBOX",L"",WS_POPUP|WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,0,0,1,1,owner_,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!popup_)throw std::runtime_error("Cannot create input override search dropdown");
    SendMessageW(popup_,WM_SETFONT,SendMessageW(edit,WM_GETFONT,0,0),FALSE);SetWindowSubclass(popup_,popup_proc,1,reinterpret_cast<DWORD_PTR>(this));SetWindowSubclass(owner_,host_proc,1,reinterpret_cast<DWORD_PTR>(this));SetWindowSubclass(static_cast<HWND>(pane_.hwnd()),host_proc,1,reinterpret_cast<DWORD_PTR>(this));arrange(width_,height_,false);
}
bool InputOverridePanel::position_popup(){
    if(!popup_)return false;
    const auto edit=static_cast<HWND>(search_.hwnd());
    if(!visible_ || !IsWindowVisible(edit) || IsIconic(owner_)){ShowWindow(popup_,SW_HIDE);return false;}
    RECT anchor{},previous{};if(!GetWindowRect(edit,&anchor))return false;
    const auto dpi=GetDpiForWindow(edit);const int gap=std::max(1,MulDiv(2,dpi,96));
    MONITORINFO monitor{sizeof(monitor)};if(!GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor))return false;
    // Always open below the field; shorten near a screen edge rather than
    // covering the text being entered. The list keeps its native scrollbar.
    const int height=std::min(MulDiv(180,dpi,96),int(monitor.rcWork.bottom-anchor.bottom-gap));
    if(height<=gap){ShowWindow(popup_,SW_HIDE);return false;}
    const RECT next{anchor.left,anchor.bottom+gap,anchor.right,anchor.bottom+gap+height};
    GetWindowRect(popup_,&previous);
    if(!EqualRect(&previous,&next) && !SetWindowPos(popup_,HWND_TOP,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOACTIVATE|SWP_NOOWNERZORDER))return false;
    const auto font=SendMessageW(edit,WM_GETFONT,0,0);if(SendMessageW(popup_,WM_GETFONT,0,0)!=font)SendMessageW(popup_,WM_SETFONT,font,TRUE);return true;
}
void InputOverridePanel::arrange(int width,int height,bool visible){
    width_=std::max(320,width);height_=std::max(240,height);visible_=visible;
    pane_.bounds({0,0,float(width_),float(height_)});ag_visible(pane_.handle(),visible?1u:0u);
    clear_.bounds({12,12,140,42});capture_.bounds({150,12,float(width_-12),42});search_.bounds({12,52,float(width_-100),80});add_.bounds({float(width_-90),52,float(width_-12),80});status_.bounds({12,88,float(width_-12),140});list_.bounds({12,148,float(width_-12),float(height_-12)});
    position_popup();
    int y=0;for(const auto& id:row_ids_){auto& row=rows_.at(id);row.first.bounds({4,float(y+4),float(width_-122),float(y+54)});row.second.bounds({float(width_-116),float(y+8),float(width_-32),float(y+38)});y+=58;}list_.extent(float(y));
    if(!visible)hide();
}
void InputOverridePanel::hide(){visible_=false;if(popup_)ShowWindow(popup_,SW_HIDE);if(runtime_ && runtime_->input_overrides().capturing)runtime_->capture_input_override(false);}
void InputOverridePanel::filter(){
    struct RedrawGuard{HWND hwnd;bool complete{};~RedrawGuard(){if(!complete){SendMessageW(hwnd,WM_SETREDRAW,TRUE,0);ShowWindow(hwnd,SW_HIDE);}RedrawWindow(hwnd,nullptr,nullptr,RDW_ERASE|RDW_FRAME|RDW_INVALIDATE);}} redraw{popup_};
    const auto query=lower(text(static_cast<HWND>(search_.hwnd())));matches_.clear();SendMessageW(popup_,WM_SETREDRAW,FALSE,0);SendMessageW(popup_,LB_RESETCONTENT,0,0);
    std::wistringstream input(query);std::vector<std::wstring> tokens;for(std::wstring word;input>>word;)tokens.push_back(std::move(word));
    for(std::size_t n=0;n<controls_.size();++n){const auto& c=controls_[n];std::wstring label=wide(c.name+" / ATA "+std::to_string(c.chapter)+(c.encoder?" / Encoder":" / Button")+(c.assigned?" / assigned":" / unassigned"));
        std::wstring keywords=lower(label);for(const auto& id:c.actions)if(const auto* a=active_catalogue().action(id))keywords+=L" "+lower(wide(a->name+" "+a->position));
        if(std::all_of(tokens.begin(),tokens.end(),[&](const auto& word){return keywords.find(word)!=keywords.npos;})){matches_.push_back(n);SendMessageW(popup_,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
    }
    SendMessageW(popup_,WM_SETREDRAW,TRUE,0);redraw.complete=true;if(!matches_.empty())SendMessageW(popup_,LB_SETCURSEL,0,0);const bool show=position_popup();ShowWindow(popup_,show?SW_SHOWNOACTIVATE:SW_HIDE);
}
void InputOverridePanel::select(){
    const auto n=SendMessageW(popup_,LB_GETCURSEL,0,0);if(n<0 || static_cast<std::size_t>(n)>=matches_.size())return;
    const auto& c=controls_[matches_[static_cast<std::size_t>(n)]];selected_=c.id;binding_=true;search_.text(wide(c.name).c_str());binding_=false;ShowWindow(popup_,SW_HIDE);SetFocus(static_cast<HWND>(search_.hwnd()));
}
LRESULT CALLBACK InputOverridePanel::edit_proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto& p=*reinterpret_cast<InputOverridePanel*>(data);try{
        if(msg==WM_KEYDOWN && (w==VK_DOWN || w==VK_UP)){if(!IsWindowVisible(p.popup_))p.filter();const auto n=SendMessageW(p.popup_,LB_GETCURSEL,0,0);const auto count=static_cast<LRESULT>(p.matches_.size());if(count)SendMessageW(p.popup_,LB_SETCURSEL,std::clamp(n+(w==VK_DOWN?1:-1),LRESULT(0),count-1),0);return 0;}
        if(msg==WM_KEYDOWN && w==VK_RETURN && IsWindowVisible(p.popup_)){p.select();return 0;}
        if(msg==WM_KEYDOWN && w==VK_ESCAPE){if(IsWindowVisible(p.popup_))ShowWindow(p.popup_,SW_HIDE);else p.runtime_->capture_input_override(false);return 0;}
        if(msg==WM_KILLFOCUS && reinterpret_cast<HWND>(w)!=p.popup_)ShowWindow(p.popup_,SW_HIDE);
    }catch(...){ShowWindow(p.popup_,SW_HIDE);}const auto result=DefSubclassProc(hwnd,msg,w,l);if(msg==WM_WINDOWPOSCHANGED || msg==WM_DPICHANGED_AFTERPARENT)p.position_popup();return result;
}
LRESULT CALLBACK InputOverridePanel::host_proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto& p=*reinterpret_cast<InputOverridePanel*>(data);const auto result=DefSubclassProc(hwnd,msg,w,l);
    if((msg==WM_ACTIVATE && LOWORD(w)==WA_INACTIVE) || (msg==WM_SHOWWINDOW && !w) || (msg==WM_SIZE && w==SIZE_MINIMIZED))ShowWindow(p.popup_,SW_HIDE);
    else if(msg==WM_WINDOWPOSCHANGED || msg==WM_DPICHANGED || msg==WM_VSCROLL || msg==WM_HSCROLL)p.position_popup();
    return result;
}
LRESULT CALLBACK InputOverridePanel::popup_proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto& p=*reinterpret_cast<InputOverridePanel*>(data);if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    // A stock top-level LISTBOX focuses itself on client mouse-down, which
    // deactivates its owner and dismisses autocomplete before mouse-up. Keep
    // edit focus and handle client selection; native scrollbars stay native.
    if(msg==WM_LBUTTONDOWN || (msg==WM_MOUSEMOVE && p.pointer_down_ && GetCapture()==hwnd)){
        const auto hit=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,l);RECT row{};const POINT point{short(LOWORD(l)),short(HIWORD(l))};
        const bool valid=!HIWORD(hit) && SendMessageW(hwnd,LB_GETITEMRECT,LOWORD(hit),reinterpret_cast<LPARAM>(&row))!=LB_ERR && PtInRect(&row,point);
        if(valid){SendMessageW(hwnd,LB_SETCURSEL,LOWORD(hit),0);if(msg==WM_LBUTTONDOWN){p.pointer_down_=true;SetCapture(hwnd);}}return 0;
    }
    if(msg==WM_LBUTTONUP){const bool choose=p.pointer_down_ && GetCapture()==hwnd;p.pointer_down_=false;if(GetCapture()==hwnd)ReleaseCapture();RECT area{};GetClientRect(hwnd,&area);const POINT point{short(LOWORD(l)),short(HIWORD(l))};if(choose && PtInRect(&area,point))try{p.select();}catch(...){ShowWindow(hwnd,SW_HIDE);}return 0;}
    if(msg==WM_CANCELMODE || msg==WM_CAPTURECHANGED || (msg==WM_SHOWWINDOW && !w)){p.pointer_down_=false;if(msg!=WM_CAPTURECHANGED && GetCapture()==hwnd)ReleaseCapture();}
    const auto result=DefSubclassProc(hwnd,msg,w,l);
    try{if(msg==WM_KEYDOWN && w==VK_RETURN)p.select();else if(msg==WM_KEYDOWN && w==VK_ESCAPE)ShowWindow(hwnd,SW_HIDE);}catch(...){ShowWindow(hwnd,SW_HIDE);}return result;
}
void InputOverridePanel::action(unsigned id){
    auto view=runtime_->input_overrides();std::string error;
    if(id==2)runtime_->capture_input_override(!view.capturing);
    else if(id==1){view.configuration.controls.clear();runtime_->save_input_overrides(std::move(view.configuration),error,view.revision);}
    else if(id==4){if(selected_.empty()){status_.text(L"Select a search result before Add");return;}if(view.configuration.controls.count(selected_)){const auto row=rows_.find(selected_);if(row!=rows_.end())ag_panel_reveal(list_.handle(),row->second.first.hwnd());status_.text(L"Already blocked; existing entry shown");return;}view.configuration.controls.insert(selected_);runtime_->save_input_overrides(std::move(view.configuration),error,view.revision);}
    else if(id>=100 && id-100<row_ids_.size()){view.configuration.controls.erase(row_ids_[id-100]);runtime_->save_input_overrides(std::move(view.configuration),error,view.revision);}
    if(!error.empty())status_.text(wide(error).c_str());
}
void InputOverridePanel::refresh(const RuntimeView& view){
    const std::pair<std::size_t,std::size_t> shape{active_catalogue().actions.size(),active_catalogue().cockpit.size()};if(shape!=catalogue_shape_ && !IsWindowVisible(popup_)){controls_=input_override_controls(active_catalogue());catalogue_shape_=shape;}
    const auto current=runtime_->input_overrides();capture_.text(current.capturing?L"Cancel capture":L"Block next input");status_.text(wide(current.status.empty()?"Type to find a button or encoder":current.status).c_str());
    for(auto& c:controls_){c.assigned=false;if(view.buttons)for(const auto& b:*view.buttons)if(b.assigned && std::find(c.actions.begin(),c.actions.end(),b.id)!=c.actions.end()){c.assigned=true;break;}if(c.encoder)for(const auto& b:view.cockpit)if(b.id==c.id.substr(8))c.assigned=true;}
    if(revision_!=current.revision){auto batch=list_.update_scope();rows_.clear();row_ids_.assign(current.configuration.controls.begin(),current.configuration.controls.end());
        for(std::size_t n=0;n<row_ids_.size();++n){const auto& id=row_ids_[n];ag_control_desc d{};d.kind=AG_LABEL;d.id=10000+static_cast<unsigned>(n);d.bounds={0,0,1,1};d.text=L"";gfx::Control label(*context_,list_.hwnd(),d,event,this);d.kind=AG_BUTTON;d.id=100+static_cast<unsigned>(n);d.text=L"Remove";gfx::Control remove(*context_,list_.hwnd(),d,event,this);rows_.emplace(id,std::make_pair(std::move(label),std::move(remove)));}revision_=current.revision;arrange(width_,height_,visible_);batch.finish();
    }
    for(const auto& id:row_ids_){const auto c=std::find_if(controls_.begin(),controls_.end(),[&](const auto& v){return v.id==id;});const auto label=c==controls_.end()?id+" / unavailable catalogue entry":c->name+(c->encoder?" / Encoder":" / Button")+(c->assigned?" / assigned":" / unassigned");rows_.at(id).first.text(wide(label).c_str());}
}
}

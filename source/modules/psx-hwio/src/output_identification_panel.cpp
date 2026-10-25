#include "hwio/output_identification_panel.hpp"
#include <algorithm>
#include <cmath>

namespace hwio {
namespace gfx=ass::graphics;
namespace {
bool same(IdentificationGesture a,IdentificationGesture b){return a.session==b.session && a.channel==b.channel && a.view==b.view;}
bool qualified(void*,const std::string& id){const auto d=active_catalogue().digital_outputs.find(id);return d!=active_catalogue().digital_outputs.end() && d->second->qualified && (d->second->kind=="lamp" || d->second->kind=="shutter");}
std::wstring phase(IdentificationPhase p){switch(p){case IdentificationPhase::acquiring:return L"Reserving card outputs";case IdentificationPhase::clearing:return L"Turning other outputs off";case IdentificationPhase::lighting:return L"Awaiting channel illumination command";case IdentificationPhase::selecting:return L"Select the illuminated output";case IdentificationPhase::channel_off:return L"Turning the current channel off";case IdentificationPhase::releasing:return L"Restoring current normal outputs";case IdentificationPhase::review:return L"Review assignments";case IdentificationPhase::paused:return L"Paused; draft retained";default:return L"Identification stopped";}}
}
OutputIdentificationPanel::~OutputIdentificationPanel(){try{hide();if(image_)image_->input(nullptr);}catch(...) {}}
void __cdecl OutputIdentificationPanel::event(void* user,std::uint32_t id,std::uint32_t kind,double)noexcept{
    auto& self=*static_cast<OutputIdentificationPanel*>(user);if(self.updating_)return;
    try{
        if(kind==AG_TIMER){self.service();return;}
        if(kind==AG_RESIZE){self.layout_dirty_=true;self.rendered_={};self.service();return;}
        if(kind==AG_DISPATCH){self.dispatch();return;}
        if(kind==AG_CLOSE){self.actions_.push_back({5,self.model_.token()});}
        else if(kind==AG_CLICK)self.actions_.push_back({id,self.model_.token()});else return;
        ag_window_post(self.window_->handle(),1);
    }catch(const std::exception& e){self.model_.pause(e.what());try{self.send_requests();}catch(...) {}}
}
std::uint32_t OutputIdentificationPanel::hit(ag_point p)const noexcept{
    if(atlas_.regions().empty() || p.x<fitted_.left || p.x>=fitted_.right || p.y<fitted_.top || p.y>=fitted_.bottom)return OutputAtlasRegion::none;
    const auto& w=view_;
    return atlas_.hit(region_,w.left+(p.x-fitted_.left)*(w.right-w.left)/(fitted_.right-fitted_.left),w.top+(p.y-fitted_.top)*(w.bottom-w.top)/(fitted_.bottom-fitted_.top));
}
bool OutputIdentificationPanel::image_ready()const noexcept{
    if(!shown_ || review_ || layout_dirty_ || !image_ || !same(rendered_,model_.token()))return false;
    ag_canvas_render_state s{};s.size=sizeof(s);s.version=1;
    return ag_canvas_state(image_->handle(),&s)>=0 && s.ready && !s.pending &&
        s.submitted_snapshot==image_submission_ && s.rendered_snapshot==image_submission_ && s.target_generation==image_target_;
}
void __cdecl OutputIdentificationPanel::pointer(void* user,std::uint32_t kind,ag_point point)noexcept{
    auto& self=*static_cast<OutputIdentificationPanel*>(user);
    try{
        if(kind==AG_CANVAS_PRESS){self.pressed_=self.model_.token();self.pressed_target_=self.image_target_;self.pressed_region_=self.model_.can_select() && self.image_ready()?self.hit(point):OutputAtlasRegion::none;}
        else if(kind==AG_CANVAS_RELEASE){const auto r=self.hit(point);if(r==self.pressed_region_ && r!=OutputAtlasRegion::none && same(self.pressed_,self.model_.token()) && self.pressed_target_==self.image_target_ && self.image_ready()){self.actions_.push_back({0,self.pressed_,r});ag_window_post(self.window_->handle(),1);}self.pressed_region_=OutputAtlasRegion::none;}
        else self.pressed_region_=OutputAtlasRegion::none;
    }catch(...) {self.pressed_region_=OutputAtlasRegion::none;}
}
void OutputIdentificationPanel::create_window(gfx::Context& context){
    context_=&context;
    if(!window_){
        window_=std::make_unique<gfx::Window>(context,L"HWIO — Assign outputs",1100,900,event,this);icons_.emplace(static_cast<HWND>(window_->hwnd()));window_->defer_close(true);window_->timer(1,50);
        ag_control_desc d{};d.kind=AG_PANEL;d.bounds={0,0,1080,860};d.text=L"";pane_=gfx::Control(context,window_->hwnd(),d);
        auto add=[&](gfx::Control& c,unsigned type,unsigned id,const wchar_t* text,void* parent){d.kind=type;d.id=id;d.text=text;d.bounds={0,0,100,30};c=gfx::Control(context,parent,d,event,this);};
        add(status_,AG_LABEL,0,L"",pane_.hwnd());add(message_,AG_LABEL,0,L"",pane_.hwnd());add(navigation_,AG_PANEL,0,L"",pane_.hwnd());
        add(footer_,AG_PANEL,0,L"",pane_.hwnd());add(up_,AG_BUTTON,1,L"Up",navigation_.hwnd());
        const wchar_t* labels[]{L"Undo",L"No output visible",L"Pause",L"Finish"};
        for(unsigned n=0;n<4;++n)add(buttons_[n],AG_BUTTON,n+2,labels[n],footer_.hwnd());
        image_=std::make_unique<gfx::Canvas>(context,pane_.hwnd(),ag_rect{12,90,760,720});image_->input(pointer,this);
    }
}
bool OutputIdentificationPanel::show(gfx::Context& context,Runtime& runtime,const std::string& device,std::string& error){
    context_=&context;runtime_=&runtime;
    if(model_.ownership_pending()){error="The previous output test is still restoring its card";return false;}
    // Every qualified illumination must be placed or explicitly deferred until
    // the human supplies its artwork. Deferrals never become assignable leaves.
    std::vector<std::string> required;for(const auto& [id,d]:active_catalogue().digital_outputs)if(d->qualified && (d->kind=="lamp" || d->kind=="shutter"))required.push_back(id);
    atlas_directory_=std::filesystem::path(executable_directory())/L"output-atlas";
    if(!atlas_.load(atlas_directory_/L"atlas.xml",true,required,qualified,nullptr,error))return false;
    for(const auto& image:atlas_.images())if(!std::filesystem::is_regular_file(atlas_directory_/image.file)){error="Output atlas image is missing: "+utf8(image.file.native());return false;}
    const auto card=runtime.output_identification_card(device,error);if(!card)return false;
    create_window(context);
    original_=runtime.hardware();if(model_.phase()!=IdentificationPhase::idle && model_.phase()!=IdentificationPhase::closed)model_.stop();
    if(!model_.begin(*card)){error=model_.reason();return false;}
    shown_=true;finishing_=false;rendered_={};layout_dirty_=true;region_=0;review_=false;window_->fit();window_->show(true);service();return true;
}
void OutputIdentificationPanel::send_requests(){
    if(!runtime_)return;
    // Several synchronous validation failures must not cause unbounded UI work.
    for(unsigned n=0;n<4;++n){const auto r=model_.take_request();if(!r)break;std::string error;
        if(!runtime_->identify_outputs(*r,model_.card(),error))model_.complete({*r,false,r->kind==IdentificationRequestKind::acquire,std::move(error)});
    }
}
void OutputIdentificationPanel::hide(){
    shown_=false;pressed_region_=OutputAtlasRegion::none;model_.stop();send_requests();if(window_)window_->show(false);
}
void OutputIdentificationPanel::refresh(){if(window_)service();}
void OutputIdentificationPanel::service(){
    if(!runtime_ || !window_ || updating_ || servicing_)return;
    struct ServiceScope {bool& active;explicit ServiceScope(bool& flag):active(flag){active=true;}~ServiceScope(){active=false;}} scope(servicing_);
    if(shown_ && (!IsWindowVisible(static_cast<HWND>(window_->hwnd())) || IsIconic(static_cast<HWND>(window_->hwnd()))))hide();
    for(unsigned n=0;n<8;++n){const auto c=runtime_->identification_completion(model_.token().session);if(!c)break;model_.complete(*c);}
    const auto now=GetTickCount64();
    if(shown_ && now>=next_inventory_ && model_.phase()!=IdentificationPhase::review && model_.phase()!=IdentificationPhase::closed && model_.phase()!=IdentificationPhase::idle){std::string error;next_inventory_=now+250;
        if(const auto card=runtime_->output_identification_card(model_.card().device,error))model_.inventory_changed(card->connection_generation,card->resource_generation);
        else if(model_.phase()!=IdentificationPhase::paused && model_.phase()!=IdentificationPhase::releasing)model_.pause(std::move(error));
    }
    send_requests();if(!shown_)return;
    if(finishing_ && model_.phase()==IdentificationPhase::paused)finishing_=false;
    if(finishing_ && model_.phase()==IdentificationPhase::review){std::string error;const bool saved=runtime_->save_identified_outputs(model_.card(),model_.choices(),original_,error);model_.finish(saved,error);finishing_=false;if(model_.phase()==IdentificationPhase::closed){hide();return;}}
    if(!same(rendered_,model_.token())){const auto found=std::find_if(atlas_.regions().begin(),atlas_.regions().end(),[&](const auto& r){return r.id==model_.region();});if(found!=atlas_.regions().end())region_=static_cast<std::uint32_t>(found-atlas_.regions().begin());}
    const auto bounds=window_->client_bounds();if(width_!=int(bounds.right) || height_!=int(bounds.bottom)){width_=int(bounds.right);height_=int(bounds.bottom);layout_dirty_=true;rendered_={};}
    if(model_.phase()==IdentificationPhase::review || !same(rendered_,model_.token()))render();
    if(layout_dirty_)layout();
    if(!review_ && same(rendered_,model_.token())){
        const auto state=image_->render_state();
        if(state.last_result<0){model_.pause("Output reference image failed to render; draft retained");send_requests();}
        else if(state.target_generation!=image_target_){pressed_region_=OutputAtlasRegion::none;layout_dirty_=true;}
        else if(image_ready())model_.image_ready(rendered_);
    }
    controls();
}
void OutputIdentificationPanel::controls(){
    const auto p=model_.phase();const auto& c=model_.card();const auto index=model_.channel();
    auto status=wide(c.device)+L"\n"+phase(p);
    if(index<c.endpoints.size())status+=L" — "+wide(c.endpoints[index].label)+L" ("+std::to_wstring(index+1)+L" / "+std::to_wstring(c.endpoints.size())+L")";
    if(status!=status_text_){status_text_=status;status_.text(status.c_str());layout_dirty_=true;}
    auto message=wide(model_.reason());if(message.empty()&&!review_&&atlas_.children(region_).empty())message=L"No outputs have been placed in this area yet. Use Up to choose another area.";
    if(message!=message_text_){message_text_=message;message_.text(message.c_str());layout_dirty_=true;}
    const bool selectable=model_.can_select() && image_ready();
    up_.enabled(selectable && region_!=0);ag_visible(up_.handle(),!review_);
    buttons_[0].enabled(!finishing_ && (selectable || p==IdentificationPhase::review));
    ag_visible(buttons_[1].handle(),!review_ && region_==0);buttons_[1].enabled(selectable && region_==0 && !finishing_);
    const auto pause=p==IdentificationPhase::paused?(model_.ownership_pending()?L"Retry restoration":L"Resume"):L"Pause";if(buttons_[2].text()!=pause){buttons_[2].text(pause);layout_dirty_=true;}
    buttons_[2].enabled(!finishing_ && p!=IdentificationPhase::closed && p!=IdentificationPhase::releasing && p!=IdentificationPhase::review);buttons_[3].enabled(!finishing_ && p!=IdentificationPhase::closed);
    for(std::size_t n=0;n<children_.size();++n)children_[n].enabled(review_?model_.choices()[n].kind==IdentificationChoiceKind::skipped:selectable);for(auto& b:breadcrumbs_)b.enabled(selectable);
    displayed_phase_=p;
}
void OutputIdentificationPanel::activate(std::uint32_t index,IdentificationGesture token){
    if(!image_ready() || index>=atlas_.regions().size())return;const auto& r=atlas_.regions()[index];
    if(r.output.empty()){if(model_.navigate(r.id,token)){region_=index;layout_dirty_=true;}}
    else model_.choose(r.output,qualified(nullptr,r.output),token);
}
void OutputIdentificationPanel::dispatch(){
    auto actions=std::move(actions_);actions_.clear();
    for(const auto& action:actions){if(!same(action.token,model_.token()))continue;const auto p=model_.phase();
        if(action.id==0)activate(action.region,action.token);
        else if(action.id==1 && region_)activate(atlas_.regions()[region_].parent,action.token);
        else if(action.id==2)model_.undo(action.token);
        else if(action.id==3 && image_ready())model_.skip(action.token);
        else if(action.id==4){if(p==IdentificationPhase::paused && !model_.ownership_pending()){std::string error;const auto card=runtime_->output_identification_card(model_.card().device,error);if(card)model_.resume(*card);else model_.pause(std::move(error));}else model_.pause("Paused; draft retained");}
        else if(action.id==5){finishing_=true;model_.review();}
        else if(action.id>=100 && action.id<200){const auto path=atlas_.path(region_);const auto n=action.id-100;if(n<path.size())activate(path[n],action.token);}
        else if(action.id>=1000){const auto n=action.id-1000;if(review_){if(n<model_.choices().size())model_.retry_skipped(n,action.token);}else if(n<child_regions_.size())activate(child_regions_[n],action.token);}
    }
    // A channel change starts at the root; navigation never changes the channel.
    const auto r=std::find_if(atlas_.regions().begin(),atlas_.regions().end(),[&](const auto& entry){return entry.id==model_.region();});if(r!=atlas_.regions().end())region_=static_cast<std::uint32_t>(r-atlas_.regions().begin());
    service();
}
void OutputIdentificationPanel::render(){
    updating_=true;
    try{
        auto batch=pane_.update_scope();
        const bool review=model_.phase()==IdentificationPhase::review;
        if(review_!=review || !same(rendered_,model_.token())){
            review_=review;children_.clear();child_regions_.clear();breadcrumbs_.clear();child_captions_.clear();breadcrumb_captions_.clear();
            // Native buttons are single-line. Wrapped native labels retain the
            // complete function name independently of the compact action.
            auto add=[&](std::vector<gfx::Control>& target,std::vector<gfx::Control>& captions,void* parent,unsigned id,const wchar_t* action,const std::wstring& text){ag_control_desc d{};d.kind=AG_BUTTON;d.id=id;d.bounds={0,0,64,30};d.text=action;target.emplace_back(*context_,parent,d,event,this);d.kind=AG_LABEL;d.id=0;d.text=text.c_str();captions.emplace_back(*context_,parent,d);};
            if(review){for(unsigned n=0;n<model_.choices().size();++n){const auto& choice=model_.choices()[n];const auto text=wide(model_.card().endpoints[n].label)+L": "+(choice.kind==IdentificationChoiceKind::assigned?wide(active_catalogue().digital_outputs.at(choice.output)->name):L"Skipped — retry");add(children_,child_captions_,navigation_.hwnd(),1000+n,L"Retry",text);if(choice.kind!=IdentificationChoiceKind::skipped)children_.back().enabled(false);}}
            else {const auto path=atlas_.path(region_);for(unsigned n=0;n<path.size();++n)add(breadcrumbs_,breadcrumb_captions_,pane_.hwnd(),100+n,L"Go",wide(atlas_.regions()[path[n]].label));for(const auto index:atlas_.children(region_)){child_regions_.push_back(index);add(children_,child_captions_,navigation_.hwnd(),1000+static_cast<unsigned>(child_regions_.size()-1),L"Select",wide(atlas_.regions()[index].label));}}
            rendered_=model_.token();layout_dirty_=true;
        }
        if(layout_dirty_)layout();
    }catch(...){updating_=false;throw;}
    updating_=false;
}
void OutputIdentificationPanel::layout(){
    if(!window_ || !shown_)return;
    auto batch=pane_.update_scope();
    const float width=std::max(360.f,float(width_)),height=std::max(400.f,float(height_));pane_.bounds({0,0,width,height});
    const float inner=width-12;const auto status_height=std::max(54.f,status_.text_extent(inner-24).y+8);status_.bounds({12,8,inner,8+status_height});
    float y=12+status_height;
    // Retain a compact path instead of consuming a row for every zoom level.
    for(std::size_t n=0;n<breadcrumbs_.size();++n){const float cell=(inner-12)/float(breadcrumbs_.size()),x=12+float(n)*cell;const auto h=std::max(30.f,breadcrumb_captions_[n].text_extent(std::max(40.f,cell-50)).y+8);breadcrumbs_[n].bounds({x,y,x+40,y+30});breadcrumb_captions_[n].bounds({x+44,y+3,x+cell-4,y+h});}
    float path_height{};for(const auto& c:breadcrumb_captions_)path_height=std::max(path_height,c.text_extent(std::max(40.f,(inner-12)/float(breadcrumbs_.size())-50)).y+8);if(!breadcrumbs_.empty())y+=std::max(30.f,path_height)+6;
    const bool narrow=inner<700;const float list_height=narrow?std::min(180.f,height*.25f):0;
    const float message_height=message_text_.empty()?0:std::max(28.f,message_.text_extent(inner-24).y+8);
    const unsigned footer_columns=inner>=600?4:2;const float footer_rows=float((4+footer_columns-1)/footer_columns);
    const float footer_height=footer_rows*40+4+message_height;
    const float image_height=std::max(80.f,height-y-footer_height-12-(narrow&&!review_?list_height+8:0));
    const float image_width=narrow?inner-12:inner-std::clamp(width*.25f,240.f,360.f)-24;
    image_->bounds({12,y,12+image_width,y+image_height});ag_visible(image_->handle(),!review_);
    navigation_.bounds(review_?ag_rect{12,y,inner,y+image_height}:narrow?ag_rect{12,y+image_height+8,inner,y+image_height+8+list_height}:ag_rect{image_width+24,y,inner,y+image_height});
    const float list_width=std::max(180.f,navigation_.viewport().right-12);up_.bounds({6,6,list_width,36});float cy=review_?6.f:44.f;
    for(std::size_t n=0;n<children_.size();++n){auto& caption=child_captions_[n];const auto h=std::max(32.f,caption.text_extent(list_width-88).y+12);children_[n].bounds({6,cy,70,cy+h});caption.bounds({78,cy+6,list_width,cy+h-6});cy+=h+5;}navigation_.extent(cy+6);
    if(!review_){
        const auto& image=atlas_.images()[atlas_.regions()[region_].image];view_=atlas_.regions()[region_].bounds;
        const auto w=view_.right-view_.left,h=view_.bottom-view_.top;const double scale=std::min(double(image_width)/w,double(image_height)/h);
        const auto left=float((image_width-w*scale)/2),top=float((image_height-h*scale)/2);fitted_={left,top,float(left+w*scale),float(top+h*scale)};
        auto screen=[&](const AtlasRectangle& r){return ag_rect{float(left+(r.left-view_.left)*scale),float(top+(r.top-view_.top)*scale),float(left+(r.right-view_.left)*scale),float(top+(r.bottom-view_.top)*scale)};};
        std::vector<ag_draw> commands;ag_draw clip{};clip.kind=AG_PUSH_CLIP;clip.rect=fitted_;commands.push_back(clip);
        ag_draw draw{};draw.kind=AG_IMAGE;draw.color={1,1,1,1};draw.rect=screen(image.world);const auto file=(atlas_directory_/image.file).native();if(file.size()>=std::size(draw.text))throw std::runtime_error("Output atlas image path exceeds graphics path limit");std::copy(file.begin(),file.end(),draw.text);commands.push_back(draw);
        for(const auto child:atlas_.children(region_)){const auto& r=atlas_.regions()[child].bounds;ag_draw box{};box.kind=AG_RECT;box.width=2;box.color={.2f,.65f,1.f,.9f};box.rect=screen(r);commands.push_back(box);}
        ag_draw pop{};pop.kind=AG_POP_CLIP;commands.push_back(pop);
        image_->input(nullptr);pressed_region_=OutputAtlasRegion::none;
        image_->submit(commands);const auto state=image_->render_state();image_submission_=state.submitted_snapshot;image_target_=state.target_generation;
        image_->input(pointer,this);
    }
    // One retained fixed footer is outside the independently scrolling choices.
    // Its old pixels are invalidated as one batch after a navigation/resize.
    const float footer_y=height-footer_height;footer_.bounds({12,footer_y,inner,footer_y+footer_rows*40});footer_.extent(footer_rows*40);
    const float button_width=(inner-12-float(footer_columns-1)*6)/float(footer_columns);for(unsigned n=0;n<4;++n){const float x=float(n%footer_columns)*(button_width+6),by=4+float(n/footer_columns)*40;buttons_[n].bounds({x,by,x+button_width,by+32});}
    message_.bounds({12,footer_y+footer_rows*40,inner,height});ag_visible(message_.handle(),message_height>0);pane_.extent(height);layout_dirty_=false;
    InvalidateRect(static_cast<HWND>(pane_.hwnd()),nullptr,TRUE);
}
}

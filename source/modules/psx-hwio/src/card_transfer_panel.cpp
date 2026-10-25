#include "hwio/card_transfer_panel.hpp"
#include <algorithm>

namespace hwio {
namespace gfx=ass::graphics;
void CardTransferPanel::attach(gfx::Context& context,gfx::Window& owner,void* parent,Runtime& runtime){
    if(context_)return;context_=&context;owner_=&owner;runtime_=&runtime;
    confirmation_=std::make_unique<gfx::Window>(context,L"HWIO — Confirm card import",600,260,event,this);icons_.emplace(static_cast<HWND>(confirmation_->hwnd()));confirmation_->defer_close(true);
    auto control=[&](gfx::Control& c,void* host,unsigned kind,unsigned id,const wchar_t* text,ag_rect bounds){ag_control_desc d{};d.kind=kind;d.id=id;d.text=text;d.bounds=bounds;c=gfx::Control(context,host,d,event,this);};
    control(export_,parent,AG_BUTTON,47001,L"Export",{0,0,140,30});control(import_,parent,AG_BUTTON,47002,L"Import",{148,0,288,30});control(message_,parent,AG_LABEL,0,L"",{0,36,500,66});
    control(warning_,confirmation_->hwnd(),AG_LABEL,0,L"",{14,14,570,166});control(confirm_,confirmation_->hwnd(),AG_BUTTON,47003,L"Overwrite assignments",{14,184,292,218});control(cancel_,confirmation_->hwnd(),AG_BUTTON,47004,L"Cancel",{306,184,570,218});
}
void CardTransferPanel::device(const std::string& key){if(key_!=key){hide();key_=key;message(L"");}std::string error;const bool eligible=runtime_->card_identity(key_,error).has_value();export_.enabled(eligible);import_.enabled(eligible);ag_tooltip_set(export_.handle(),wide(error).c_str());ag_tooltip_set(import_.handle(),wide(error).c_str());}
float CardTransferPanel::arrange(float x,float y,float width){const auto half=(width-8)/2;export_.bounds({x,y,x+half,y+30});import_.bounds({x+half+8,y,x+width,y+30});const auto height=message_.text().empty()?0.f:std::max(30.f,message_.text_extent(width).y+8);message_.bounds({x,y+36,x+width,y+36+std::max(1.f,height)});layout_dirty_=false;return 38+height;}
void CardTransferPanel::hide(){++epoch_;proposal_.reset();events_.clear();if(confirmation_)confirmation_->show(false);}
void __cdecl CardTransferPanel::event(void* user,std::uint32_t id,std::uint32_t kind,double)noexcept{
    auto& self=*static_cast<CardTransferPanel*>(user);try{if(kind==AG_CLOSE){self.hide();return;}if(kind==AG_DISPATCH){self.dispatch();return;}if(kind==AG_CLICK&&id>=47001&&id<=47004&&self.events_.size()<8){self.events_.push_back(id-47000);ag_window_post(self.confirmation_->handle(),1);}}catch(const std::exception& e){try{if(self.message_.handle())self.message(wide(e.what()));}catch(...){}}catch(...){}
}
void CardTransferPanel::dispatch(){if(dispatching_||events_.empty())return;dispatching_=true;try{const auto id=events_.front();events_.clear();if(id==1||id==2)choose(id==2);else if(id==3)finish(true);else if(id==4)hide();events_.clear();if(layout_dirty_&&reflow_)reflow_();dispatching_=false;}catch(...){dispatching_=false;throw;}}
void CardTransferPanel::choose(bool importing){
    hide();const auto epoch=epoch_;const auto key=key_;std::wstring path;std::uint32_t accepted{};struct Selected {std::wstring& path;bool failed{};}selected{path};
    const auto result=ag_file_dialog(context_->handle(),owner_->hwnd(),importing?0u:1u,importing?L"Import card assignments":L"Export card assignments",L"HWIO card assignments",L"*.xml",L"card-assignments.xml",[](void* user,const wchar_t* value)noexcept{auto& s=*static_cast<Selected*>(user);try{s.path=value;}catch(...){s.failed=true;}},&selected,&accepted);
    if(epoch_!=epoch||key_!=key)return;if(FAILED(result)||selected.failed){message(L"Could not open the file selection");return;}if(!accepted)return;
    std::filesystem::path file(path);auto extension=file.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),[](wchar_t ch){return wchar_t(towlower(ch));});if(extension.empty()&&!importing)file+=L".xml";else if(extension!=L".xml"){message(L"Select an XML file (.xml)");return;}
    std::string error;
    if(!importing){message(runtime_->export_card_assignments(key_,file,error)?L"Card assignments exported":wide(error));return;}
    CardImport proposal;if(!runtime_->prepare_card_import(key_,file,proposal,error)){message(wide("Import not applied: "+error));return;}offer(std::move(proposal));
}
void CardTransferPanel::offer(CardImport proposal,bool visible){
    proposal_.emplace(std::move(proposal));
    if(proposal_->match==CardMatch::exact){finish(false);return;}
    const auto text=L"This XML is for the same card model, but a different serial number.\n\nXML: "+wide(proposal_->source.model)+L" / "+wide(proposal_->source.serial)+L"\nSelected card: "+wide(proposal_->target.model)+L" / "+wide(proposal_->target.serial)+L"\n\nOverwrite digital, encoder and analogue I/O assignments for the selected card? Matching I/O inputs under the XML's old serial will move to this card. Flight axes, motors and card settings will be preserved.";
    warning_.text(text.c_str());const auto height=std::max(160.f,warning_.text_extent(556).y+10);warning_.bounds({14,14,570,14+height});confirm_.bounds({14,height+28,292,height+62});cancel_.bounds({306,height+28,570,height+62});confirmation_->bounds({100,100,700,height+222});confirmation_->fit();confirmation_->show(visible);
}
void CardTransferPanel::finish(bool confirmed){if(!proposal_)return;std::string error;const bool ok=runtime_->import_card_assignments(*proposal_,confirmed,error);hide();message(ok?L"Card assignments imported":wide("Import not applied: "+error));}
}

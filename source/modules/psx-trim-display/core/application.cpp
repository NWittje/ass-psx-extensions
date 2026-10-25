#include "trim/application.hpp"
namespace trim {
using namespace psx::hardware;
Application::Application(Descriptor d,Role r) noexcept:role_(r),link_(d,{this,message,session}){}
bool Application::message(void* p,const Message& m,std::uint64_t now) noexcept {
    auto& app=*static_cast<Application*>(p);Snapshot s;
    if(m.type!=state_type||!decode_snapshot(m.payload,app.role_,s))return false;
    app.observation_={s,now,m.session,m.sequence,true,true};return true;
}
void Application::session(void* p,std::uint32_t id,bool active) noexcept {
    auto& app=*static_cast<Application*>(p);app.observation_={};app.observation_.session=id;app.observation_.active=active;app.observation_.value.role=app.role_;
}
void Application::report(std::uint32_t rendered_session,std::uint32_t rendered,bool has_rendered,bool fault,Mode mode,
                         std::uint32_t heap,std::uint32_t dma,std::uint32_t uart,
                         std::uint32_t comm_stack,std::uint32_t render_stack) noexcept {
    const auto s=link_.status();if(!s.active)return;
    std::array<std::uint8_t,48> b{};
    write32(b,0,s.accepted_sequence);write32(b,4,rendered);
    b[8]=std::uint8_t((s.has_accepted?1:0)|(has_rendered&&rendered_session==s.session?2:0)|(fault?4:0));
    b[9]=std::uint8_t(mode);b[10]=std::uint8_t(role_);
    write32(b,12,s.rejected+s.profile_rejected);write32(b,16,s.oversized);write32(b,20,s.wrong_session);
    write32(b,24,s.tx_busy);write32(b,28,uart);write32(b,32,heap);write32(b,36,dma);
    write32(b,40,comm_stack);write32(b,44,render_stack);
    link_.try_send(std::uint8_t(Type::status),b);
}
}

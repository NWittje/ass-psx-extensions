#include "psx/cdu/blackout.hpp"
#include <algorithm>
#include <cstring>

namespace psx::cdu::blackout
{
namespace
{
void put(std::span<std::uint8_t> b, unsigned p, std::uint64_t v, unsigned n) noexcept
{ for (unsigned i=0;i<n;++i) b[p+i]=std::uint8_t(v>>(8*i)); }
std::uint64_t get(std::span<const std::uint8_t> b, unsigned p, unsigned n) noexcept
{ std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[p+i])<<(8*i);return v; }
bool zero(std::span<const std::uint8_t> b) noexcept
{ return std::all_of(b.begin(),b.end(),[](auto c){return c==0;}); }
template<std::size_t N> bool terminated(const std::array<char,N> &s) noexcept
{ return std::find(s.begin(),s.end(),'\0')!=s.end(); }
bool valid(const Control &c) noexcept
{
 return !zero(c.session) && c.generation && c.sequence && c.revision &&
        !(c.mask & ~all_units) && !(c.flags & ~ready_flag) && ((c.flags & ready_flag)||!c.mask);
}
void message(Packet &p, const char *s) noexcept
{ p.message={};std::memcpy(p.message.data(),s,std::min(std::strlen(s),p.message.size()-1)); }
}
bool encode(const Control &c,std::array<std::uint8_t,control_bytes>&b) noexcept
{
 if(!valid(c))return false;b={};std::memcpy(b.data(),"CBL1",4);put(b,4,1,2);put(b,6,b.size(),2);
 std::copy(c.session.begin(),c.session.end(),b.begin()+8);put(b,24,c.generation,8);put(b,32,c.sequence,8);
 put(b,40,c.revision,8);put(b,48,c.mask,4);put(b,52,c.flags,4);return true;
}
bool decode(std::span<const std::uint8_t>b,Control&out) noexcept
{
 if(b.size()!=control_bytes||std::memcmp(b.data(),"CBL1",4)||get(b,4,2)!=1||get(b,6,2)!=b.size()||!zero(b.subspan(56)))return false;
 Control c;std::copy_n(b.begin()+8,16,c.session.begin());c.generation=get(b,24,8);c.sequence=get(b,32,8);
 c.revision=get(b,40,8);c.mask=std::uint32_t(get(b,48,4));c.flags=std::uint32_t(get(b,52,4));
 if(!valid(c))return false;out=c;return true;
}
bool context(Context&c,std::string_view host,std::uint16_t port,std::string_view adapter,std::uint64_t epoch) noexcept
{
 if(host.empty()||host.size()>=c.host.size()||host.find('\0')!=host.npos||!port||adapter.size()>=c.adapter.size()||adapter.find('\0')!=adapter.npos)return false;
 Context next;std::copy(host.begin(),host.end(),next.host.begin());std::copy(adapter.begin(),adapter.end(),next.adapter.begin());next.port=port;next.binding_epoch=epoch;c=next;return true;
}
bool encode(const Packet&p,std::array<std::uint8_t,pipe_bytes>&b) noexcept
{
 if(unsigned(p.operation)>3||unsigned(p.result)>4||!terminated(p.main.host)||!terminated(p.main.adapter)||!terminated(p.message)||p.enabled>1||(p.selected_mask|p.active_mask)&~all_units||p.flags&~(ready_flag|bound_flag))return false;
 b={};std::memcpy(b.data(),"CDP1",4);put(b,4,1,2);put(b,6,b.size(),2);put(b,8,unsigned(p.operation),4);put(b,12,unsigned(p.result),4);
 put(b,16,p.request,8);put(b,24,p.epoch,8);put(b,32,p.generation,8);put(b,40,p.revision,8);
 put(b,48,p.flags,4);put(b,52,p.selected_mask,4);put(b,56,p.active_mask,4);put(b,60,p.enabled,4);
 put(b,64,p.main.binding_epoch,8);put(b,72,p.main.port,2);std::memcpy(b.data()+76,p.main.host.data(),16);
 std::memcpy(b.data()+92,p.main.adapter.data(),48);std::memcpy(b.data()+140,p.message.data(),96);return true;
}
bool decode(std::span<const std::uint8_t>b,Packet&out) noexcept
{
 if(b.size()!=pipe_bytes||std::memcmp(b.data(),"CDP1",4)||get(b,4,2)!=1||get(b,6,2)!=pipe_bytes||!zero(b.subspan(74,2))||!zero(b.subspan(236)))return false;
 Packet p;p.operation=Operation(get(b,8,4));p.result=Result(get(b,12,4));p.request=get(b,16,8);p.epoch=get(b,24,8);p.generation=get(b,32,8);p.revision=get(b,40,8);
 p.flags=std::uint32_t(get(b,48,4));p.selected_mask=std::uint32_t(get(b,52,4));p.active_mask=std::uint32_t(get(b,56,4));p.enabled=std::uint32_t(get(b,60,4));p.main.binding_epoch=get(b,64,8);p.main.port=std::uint16_t(get(b,72,2));
 std::memcpy(p.main.host.data(),b.data()+76,16);std::memcpy(p.main.adapter.data(),b.data()+92,48);std::memcpy(p.message.data(),b.data()+140,96);
 std::array<std::uint8_t,pipe_bytes> check{};if(!encode(p,check))return false;
 // Canonical strings cannot hide a second endpoint after their terminator.
 auto canonical=[](const auto&s){auto end=std::find(s.begin(),s.end(),'\0');return end!=s.end()&&std::all_of(end,s.end(),[](char c){return !c;});};
 if(!canonical(p.main.host)||!canonical(p.main.adapter)||!canonical(p.message))return false;out=p;return true;
}
State::State(std::uint64_t epoch) noexcept { state_.epoch=epoch?epoch:1; }
void State::revoke() noexcept
{
 state_.mask=0;state_.bound=false;state_.lease_until=0;++state_.revision;
 if(++state_.epoch==0)++state_.epoch;
}
void State::update(const Context&c,std::uint64_t gen,bool ready,std::uint64_t now) noexcept
{
 ready=ready&&gen!=0;
 if(c!=state_.main||gen!=state_.generation||ready!=state_.ready){revoke();state_.main=c;state_.generation=gen;state_.ready=ready;}
 tick(now);
}
void State::tick(std::uint64_t now) noexcept { if(state_.bound && now>=state_.lease_until)revoke(); }
void State::owner_lost() noexcept { if(state_.bound||state_.mask)revoke(); }
Packet State::dispatch(const Packet&q,std::uint64_t now) noexcept
{
 tick(now);Packet p=q;p.result=Result::ok;message(p,"Accepted; client rendering is not observed");
 bool renew=false;
 if(q.operation==Operation::status)
 {
  const bool discovery=!q.epoch&&!q.generation&&!q.revision&&q.main==Context{};
  if(!discovery)
  {
   if(q.epoch!=state_.epoch||q.generation!=state_.generation||q.main!=state_.main||q.revision!=state_.revision)
   {p.result=Result::stale;message(p,"Control keepalive context changed; explicit action required");}
   else if(!state_.bound){p.result=Result::not_bound;message(p,"No current control owner");}
   else renew=true;
  }
 }
 if(q.operation!=Operation::status)
 {
  if(!state_.ready){p.result=Result::not_ready;message(p,"Current Main source is not initialized");}
  else if(q.epoch!=state_.epoch||q.generation!=state_.generation||q.main!=state_.main||q.revision!=state_.revision){p.result=Result::stale;message(p,"Context or state revision changed; do not replay");}
  else if(q.operation==Operation::bind){state_.bound=true;}
  else if(!state_.bound){p.result=Result::not_bound;message(p,"Bind this control owner first");}
  else if(q.operation==Operation::set && (!q.selected_mask || (q.selected_mask&~all_units)||q.enabled>1)){p.result=Result::invalid;message(p,"Invalid CDU mask or boolean");}
  else if(q.operation==Operation::set || q.operation==Operation::clear_all)
  {
   auto mask=q.operation==Operation::clear_all?0u:q.enabled?(state_.mask|q.selected_mask):(state_.mask&~q.selected_mask);
   // A new explicit idempotent action also revokes delayed copies and lets a
   // receiver recover after its independent control lease expired.
   state_.mask=mask;++state_.revision;
  }
  else {p.result=Result::invalid;message(p,"Unsupported operation");}
  renew=p.result==Result::ok&&state_.bound;
 }
 if(renew)state_.lease_until=now+owner_lease_ns;
 p.epoch=state_.epoch;p.generation=state_.generation;p.revision=state_.revision;p.main=state_.main;
 p.flags=(state_.ready?ready_flag:0)|(state_.bound?bound_flag:0);p.active_mask=state_.mask;return p;
}
void Reception::clear() noexcept { session_={};generation_=blocked_revision_=0;pinned_=source_healthy_=false;value_={};observed_mask_=observed_flags_=0; }
void Reception::source(bool pinned,const SessionId&s,std::uint64_t gen,bool healthy,std::uint64_t now) noexcept
{
 if(!pinned||!pinned_||s!=session_||gen!=generation_){clear();pinned_=pinned;session_=s;generation_=gen;}
 source_healthy_=healthy;
 if(!healthy){blocked_revision_=std::max(blocked_revision_,value_.revision);value_.valid=false;value_.mask=0;}
 tick(now);
}
void Reception::tick(std::uint64_t now) noexcept
{
 if(value_.valid && now>=value_.last_advance && now-value_.last_advance>=receive_lease_ns)
 {blocked_revision_=std::max(blocked_revision_,value_.revision);value_.valid=false;value_.mask=0;}
}
bool Reception::accept(const Control&c,std::uint64_t now) noexcept
{
 tick(now);
 if(!pinned_||!source_healthy_||c.session!=session_||c.generation!=generation_||!valid(c)||c.sequence<=value_.sequence||c.revision<value_.revision)return false;
 if(c.revision==value_.revision&&(c.mask!=observed_mask_||c.flags!=observed_flags_))return false;
 // An expired active revision cannot be resurrected by delayed repair or repeats.
 if(c.mask && c.revision<=blocked_revision_)return false;
 value_={true,c.mask,c.revision,c.sequence,now};observed_mask_=c.mask;observed_flags_=c.flags;return true;
}
} // namespace psx::cdu::blackout

#pragma once
#include <psx/sync/protocol.hpp>
#include <array>
#include <cstdint>
#include <span>
namespace psx::sync {
// One owner. The endpoint's worker owns this object; producer synchronization
// remains outside it. Different groups sharing a lane retain independent slots.
class StateWatermarks {
 struct Entry {Family family{};std::uint64_t key{},sequence{};bool active{};};
 std::array<Entry,64> entries_{};
public:
 bool newer(Family f,std::uint64_t key,std::uint64_t sequence)const noexcept {
  if(!sequence)return false;for(const auto& e:entries_)if(e.active&&e.family==f&&e.key==key)return sequence>e.sequence;return true;
 }
 bool commit(Family f,std::uint64_t key,std::uint64_t sequence)noexcept {
  if(!newer(f,key,sequence))return false;Entry* vacancy{};
  for(auto& e:entries_){if(e.active&&e.family==f&&e.key==key){e.sequence=sequence;return true;}if(!e.active&&!vacancy)vacancy=&e;}
  if(!vacancy)return false;*vacancy={f,key,sequence,true};return true;
 }
 void clear()noexcept{entries_={};}
 void clear(Family f)noexcept{for(auto& e:entries_)if(e.family==f)e={};}
};
// These are cheap connection-scoped incarnation numbers, not credentials.
// A peer restart invalidates only this service; the local incarnation stays
// stable when its worker restarts in response, avoiding reset ping-pong.
struct ServiceBinding {
 std::uint32_t local{},remote{},echo{};std::uint64_t generation{};
 bool active{},remote_open{},dirty{};
 bool ready()const noexcept{return active&&remote_open&&local&&remote&&echo==local;}
};
class ServiceBindings {
 std::array<ServiceBinding,8> services_{};std::uint32_t next_{};
public:
 ServiceBinding& at(Family f)noexcept{return services_[unsigned(f)];}
 const ServiceBinding& at(Family f)const noexcept{return services_[unsigned(f)];}
 std::uint64_t open(Family f)noexcept{auto& s=at(f);if(!s.active){if(next_==UINT32_MAX)return 0;s.local=++next_;s.echo=0;s.active=s.dirty=true;++s.generation;}return s.generation;}
 void close(Family f,std::uint64_t generation)noexcept{auto& s=at(f);if(s.active&&s.generation==generation){s.active=false;s.dirty=true;++s.generation;}}
 bool observe(Family f,std::uint32_t source,std::uint32_t target,bool open)noexcept{
  auto& s=at(f);if(!source||source<s.remote||(source==s.remote&&!s.remote_open&&open))return false;
  if(source!=s.remote||open!=s.remote_open){if(s.remote)++s.generation;s.dirty=true;s.remote=source;s.remote_open=open;}
  s.echo=target;return true;
 }
 void clear()noexcept{services_={};next_=0;}
};
}

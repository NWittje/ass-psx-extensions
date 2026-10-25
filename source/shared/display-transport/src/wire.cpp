#include <psx/display/transport.hpp>
#include <psx/display/dpf_protocol.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace psx::du::transport {
namespace {
std::atomic_size_t payload_bytes{},payload_peak{};
template<class T> T get(std::span<const uint8_t>b,size_t p) { if(p>b.size()||sizeof(T)>b.size()-p)throw std::runtime_error("DUN1 bounds");T x{};std::memcpy(&x,b.data()+p,sizeof x);return x; }
template<class T> void put(Bytes&b,size_t p,T x) { std::memcpy(b.data()+p,&x,sizeof x); }
void check(const Envelope&e) {
 if(e.unit>=6||!e.generation||!e.sequence||std::all_of(e.session.begin(),e.session.end(),[](auto x){return x==0;})||e.source.identity.size()>envelope_limit-128||uint32_t(e.state)<1||uint32_t(e.state)>6)throw std::runtime_error("DUN1 identity");
 if(e.state==State::frame) { auto d=psx::recorder::decode(e.frame); if(d.unit!=e.unit||d.frequency!=e.source.clock||!e.source.pid||!e.source.created||!identity_json(e.source.identity))throw std::runtime_error("DUN1 source/frame mismatch"); }
 else if(!e.frame.empty())throw std::runtime_error("DUN1 status payload");
}
}
Owned encode(const Envelope&e) {
 check(e); auto n=128+e.source.identity.size(),size=n+e.frame.size();auto old=payload_bytes.load();
 // Reserve 48 MiB for the six pipe assembly buffers within the 256 MiB host budget.
 do{if(size>208*1024*1024-old)throw std::runtime_error("DU host payload budget");}while(!payload_bytes.compare_exchange_weak(old,old+size));
 auto peak=payload_peak.load();while(peak<old+size&&!payload_peak.compare_exchange_weak(peak,old+size)){}
 // A shared_ptr constructor invokes its deleter if control-block allocation fails.
 // Only vector allocation belongs to the manual rollback below.
 Bytes* raw;try{raw=new Bytes(size);}catch(...){payload_bytes-=size;throw;}
 std::shared_ptr<Bytes> b(raw,[size](auto*p){delete p;payload_bytes-=size;});
 put(*b,0,uint32_t(0x314e5544));put(*b,4,uint32_t(1));put(*b,8,uint32_t(n));put(*b,12,uint32_t(e.frame.size()));
 std::copy(e.session.begin(),e.session.end(),b->begin()+16);
 put(*b,32,e.source.pid);put(*b,36,e.unit);put(*b,40,e.source.created);put(*b,48,e.source.clock);
 put(*b,56,e.generation);put(*b,64,e.sequence);put(*b,72,e.offered);put(*b,80,uint32_t(e.state));put(*b,84,uint32_t(e.source.identity.size()));
 std::copy(e.source.identity.begin(),e.source.identity.end(),b->begin()+128);std::copy(e.frame.begin(),e.frame.end(),b->begin()+n);return b;
}
size_t owned_bytes(){return payload_bytes.load();}size_t owned_peak(){return payload_peak.load();}
Envelope decode(std::span<const uint8_t>b) {
 if(b.size()<128||b.size()>frame_limit+envelope_limit||get<uint32_t>(b,0)!=0x314e5544||get<uint32_t>(b,4)!=1)throw std::runtime_error("DUN1 header");
 auto h=get<uint32_t>(b,8),n=get<uint32_t>(b,12),s=get<uint32_t>(b,84);
 if(h<128||h>envelope_limit||h!=128+s||n>frame_limit||size_t(h)+n!=b.size())throw std::runtime_error("DUN1 length");
 for(size_t p=88;p<128;++p)if(b[p])throw std::runtime_error("DUN1 reserved");
 Envelope e;std::copy_n(b.begin()+16,16,e.session.begin());e.source.pid=get<uint32_t>(b,32);e.unit=get<uint32_t>(b,36);e.source.created=get<uint64_t>(b,40);e.source.clock=get<uint64_t>(b,48);e.generation=get<uint64_t>(b,56);e.sequence=get<uint64_t>(b,64);e.offered=get<uint64_t>(b,72);e.state=State(get<uint32_t>(b,80));e.source.identity.assign(reinterpret_cast<const char*>(b.data()+128),s);e.frame=b.subspan(h,n);check(e);return e;
}
std::string group(unsigned u){if(u>=6)throw std::runtime_error("DU unit");return "239.255.74."+std::to_string(80+u);}
uint16_t port(unsigned u){if(u>=6)throw std::runtime_error("DU unit");return uint16_t(47480+u);}
void Admission::reset(){*this=Admission{};}
bool Admission::accept(const Envelope&e){
 if(active_&&e.session!=current_){for(size_t i=0;i<count_;++i)if(retired_[i]==e.session)return false;if(count_==retired_.size())return false;retired_[count_++]=current_;active_=false;}
 if(!active_){current_=e.session;generation_=0;sequence_=0;source_=e.source;active_=true;}
 if(e.generation<generation_||(e.generation==generation_&&e.sequence<=sequence_))return false;
 if(e.generation==generation_&&e.state==State::frame&&(e.source.pid!=source_.pid||e.source.created!=source_.created||e.source.identity!=source_.identity))return false;
 generation_=e.generation;sequence_=e.sequence;source_=e.source;return true;
}
}

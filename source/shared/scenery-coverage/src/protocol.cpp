#include <psx/scenery/protocol.hpp>
#include <bit>
#include <cmath>
namespace psx::scenery {
namespace {
struct Writer {std::uint8_t* p;std::size_t n{};void u(std::uint64_t v,unsigned count){for(unsigned i=0;i<count;++i)p[n++]=static_cast<std::uint8_t>(v>>(8*i));}void real(double v){u(std::bit_cast<std::uint64_t>(v),8);}};
struct Reader {const std::uint8_t* p;std::size_t n{};std::uint64_t u(unsigned count){std::uint64_t v{};for(unsigned i=0;i<count;++i)v|=std::uint64_t(p[n++])<<(8*i);return v;}double real(){return std::bit_cast<double>(u(8));}};
void put(Writer& w,const Query& q){w.u(q.operation,8);w.u(q.psx_pid,4);w.u(q.psx_birth,8);w.real(q.latitude_deg);w.real(q.longitude_deg);}
void get(Reader& r,Query& q){q.operation=r.u(8);q.psx_pid=static_cast<std::uint32_t>(r.u(4));q.psx_birth=r.u(8);q.latitude_deg=r.real();q.longitude_deg=r.real();}
void header(Writer& w,unsigned kind,std::size_t bytes){w.u(0x31435850,4);w.u(version,2);w.u(kind,2);w.u(bytes,4);w.u(1,4);}
bool header(Reader& r,unsigned kind,std::size_t bytes){return r.u(4)==0x31435850&&r.u(2)==version&&r.u(2)==kind&&r.u(4)==bytes&&r.u(4)==1;}
}
bool tile(double lat,double lon,int& south,int& west) noexcept {
 if(!std::isfinite(lat)||!std::isfinite(lon)||lat < -90||lat>90||lon < -180||lon>180)return false;
 south=lat==90?89:static_cast<int>(std::floor(lat));west=lon==180?-180:static_cast<int>(std::floor(lon));return true;
}
bool valid(const Query& q) noexcept {int s{},w{};return q.operation&&tile(q.latitude_deg,q.longitude_deg,s,w);}
bool valid(const Result& r) noexcept {
 if(!valid(r.query)||r.verdict>Verdict::missing||r.reason>Reason::busy)return false;
 if(r.verdict==Verdict::unavailable)return r.reason!=Reason::none;
 int s{},w{};return r.reason==Reason::none&&tile(r.query.latitude_deg,r.query.longitude_deg,s,w)&&r.south==s&&r.west==w&&r.plugin_load&&r.plugin_enable&&r.inventory_generation&&r.observed_qpc>0&&r.source_qpc>0&&r.source_frequency;
}
bool encode(const Query& q,Frame& f) noexcept {f={};if(!valid(q))return false;Writer w{f.bytes.data()};header(w,1,query_bytes);put(w,q);f.size=w.n;return true;}
bool encode(const Result& v,Frame& f) noexcept {
 f={};if(!valid(v))return false;Writer w{f.bytes.data()};header(w,2,result_bytes);put(w,v.query);w.u(static_cast<unsigned>(v.verdict),4);w.u(static_cast<unsigned>(v.reason),4);w.u(static_cast<std::uint32_t>(v.south),4);w.u(static_cast<std::uint32_t>(v.west),4);w.u(v.plugin_load,8);w.u(v.plugin_enable,8);w.u(v.inventory_generation,8);w.u(static_cast<std::uint64_t>(v.observed_qpc),8);w.u(static_cast<std::uint64_t>(v.source_qpc),8);w.u(v.source_frequency,8);w.u(0,8);w.u(0,4);f.size=w.n;return true;
}
bool decode(const std::uint8_t* p,std::size_t n,Query& q) noexcept {if(!p||n!=16+query_bytes)return false;Reader r{p};Query v;if(!header(r,1,query_bytes))return false;get(r,v);if(!valid(v))return false;q=v;return true;}
bool decode(const std::uint8_t* p,std::size_t n,Result& out) noexcept {
 if(!p||n!=16+result_bytes)return false;Reader r{p};if(!header(r,2,result_bytes))return false;Result v;get(r,v.query);v.verdict=static_cast<Verdict>(r.u(4));v.reason=static_cast<Reason>(r.u(4));v.south=static_cast<std::int32_t>(r.u(4));v.west=static_cast<std::int32_t>(r.u(4));v.plugin_load=r.u(8);v.plugin_enable=r.u(8);v.inventory_generation=r.u(8);v.observed_qpc=static_cast<std::int64_t>(r.u(8));v.source_qpc=static_cast<std::int64_t>(r.u(8));v.source_frequency=r.u(8);if(r.u(8)||r.u(4)||!valid(v))return false;out=v;return true;
}
}

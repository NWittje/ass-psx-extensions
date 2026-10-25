#include <psx/situation/fmc_protocol.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>

namespace psx::fmc::wire {namespace {
namespace s=psx::situation;
void put(std::uint8_t* p,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=static_cast<std::uint8_t>(v>>(8*i));}
std::uint64_t get(const std::uint8_t* p,unsigned n){std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
void head(s::Frame& f,unsigned kind,std::size_t size,std::uint32_t seq,std::uint16_t version){f={};std::memcpy(f.bytes.data(),"PXQ1",4);put(f.bytes.data()+4,version,2);put(f.bytes.data()+6,kind,2);put(f.bytes.data()+8,size,4);put(f.bytes.data()+12,seq,4);f.size=16+size;}
bool header(std::span<const std::uint8_t> b,unsigned kind,std::size_t prefix,std::uint32_t& sequence,std::uint16_t version){
 if((version!=revision&&version!=native_revision)||b.size()<16+prefix||b.size()>s::frame_bytes||std::memcmp(b.data(),"PXQ1",4)||get(b.data()+4,2)!=version||get(b.data()+6,2)!=kind||get(b.data()+8,4)!=b.size()-16)return false;
 sequence=static_cast<std::uint32_t>(get(b.data()+12,4));return sequence!=0;
}
bool bounds(std::uint32_t offset,std::uint32_t total,std::size_t size){return size<=chunk_bytes&&total<=s::max_artifact&&offset<=total&&size<=total-offset;}
}
bool encode(const Request& r,std::uint32_t sequence,s::Frame& frame)noexcept{return encode(r,sequence,frame,revision);}
bool encode(const Result& r,std::uint32_t sequence,s::Frame& frame)noexcept{return encode(r,sequence,frame,revision);}
bool decode(std::span<const std::uint8_t> bytes,Request& r,std::uint32_t& sequence)noexcept{return decode(bytes,r,sequence,revision);}
bool decode(std::span<const std::uint8_t> bytes,Result& r,std::uint32_t& sequence)noexcept{return decode(bytes,r,sequence,revision);}
bool encode(const Request& r,std::uint32_t sequence,s::Frame& f,std::uint16_t version)noexcept{try{
 if((version!=revision&&version!=native_revision)||!sequence||static_cast<unsigned>(r.command)>9||!bounds(r.offset,r.total,r.data.size()))return false;
 head(f,1,request_prefix+r.data.size(),sequence,version);auto p=f.bytes.data()+16;
 put(p,static_cast<unsigned>(r.command),4);put(p+4,r.client.pid,4);put(p+8,r.target.pid,4);put(p+16,r.client.creation_filetime,8);put(p+24,r.target.creation_filetime,8);put(p+32,r.session,8);put(p+40,r.operation,8);put(p+48,r.lifecycle,8);put(p+56,r.frequency,8);put(p+64,std::bit_cast<std::uint64_t>(r.issued_qpc),8);put(p+72,std::bit_cast<std::uint64_t>(r.deadline_qpc),8);put(p+80,r.offset,4);put(p+84,r.total,4);put(p+88,r.data.size(),4);put(p+96,r.generation,8);std::copy(r.data.begin(),r.data.end(),p+request_prefix);return true;
}catch(...){return false;}}
bool decode(std::span<const std::uint8_t> b,Request& r,std::uint32_t& sequence,std::uint16_t version)noexcept{try{
 if(!header(b,1,request_prefix,sequence,version))return false;auto p=b.data()+16;auto count=get(p+88,4);
 if(count!=b.size()-16-request_prefix||get(p,4)>9||get(p+12,4)||get(p+92,4)||get(p+104,8))return false;
 Request result;result.command=static_cast<Command>(get(p,4));result.client={static_cast<std::uint32_t>(get(p+4,4)),get(p+16,8)};result.target={static_cast<std::uint32_t>(get(p+8,4)),get(p+24,8)};result.session=get(p+32,8);result.operation=get(p+40,8);result.lifecycle=get(p+48,8);result.frequency=get(p+56,8);result.issued_qpc=std::bit_cast<std::int64_t>(get(p+64,8));result.deadline_qpc=std::bit_cast<std::int64_t>(get(p+72,8));result.offset=static_cast<std::uint32_t>(get(p+80,4));result.total=static_cast<std::uint32_t>(get(p+84,4));result.generation=get(p+96,8);
 if(!result.client.pid||!result.target.pid||!result.client.creation_filetime||!result.target.creation_filetime||!result.session||!result.operation||!result.frequency||!bounds(result.offset,result.total,static_cast<std::size_t>(count)))return false;
 result.data.assign(p+request_prefix,p+request_prefix+count);r=std::move(result);return true;
}catch(...){return false;}}
bool encode(const Result& r,std::uint32_t sequence,s::Frame& f,std::uint16_t version)noexcept{try{
 const auto& a=r.common;const bool native=version==native_revision;if((version!=revision&&!native)||!sequence||static_cast<unsigned>(a.phase)>(native?6u:7u)||static_cast<unsigned>(a.reason)>15||(a.capabilities&~(native?255u:127u))||(a.evidence&~(native?511u:255u))||r.entry_pause_mask>3||r.model>2||(r.validity&~(native?15u:7u))||!bounds(a.offset,a.total,a.data.size()))return false;
 head(f,2,result_prefix+a.data.size(),sequence,version);auto p=f.bytes.data()+16;
 put(p,static_cast<unsigned>(a.phase),4);put(p+4,static_cast<unsigned>(a.reason),4);put(p+8,a.capabilities,4);put(p+12,a.evidence,4);put(p+16,a.server.pid,4);put(p+20,r.model,4);put(p+24,a.server.creation_filetime,8);put(p+32,a.session,8);put(p+40,a.operation,8);put(p+48,a.lifecycle,8);put(p+56,std::bit_cast<std::uint64_t>(a.observation_qpc),8);put(p+64,a.offset,4);put(p+68,a.total,4);put(p+72,a.data.size(),4);std::copy(a.artifact_hash.begin(),a.artifact_hash.end(),p+80);std::copy(a.source_jar_hash.begin(),a.source_jar_hash.end(),p+112);
 for(unsigned i=0;i<12;++i){if(!std::isfinite(a.readback[i]))return false;put(p+144+8*i,std::bit_cast<std::uint64_t>(a.readback[i]),8);}
 put(p+240,r.generation,8);put(p+248,r.entry_pause_mask,4);put(p+252,r.validity,4);unsigned offset=256;for(auto hash:{r.profile,r.navigation_database,r.model_configuration,r.restored_state,r.preserved_state}){std::copy(hash.begin(),hash.end(),p+offset);offset+=32;}
 std::copy(a.data.begin(),a.data.end(),p+result_prefix);return true;
}catch(...){return false;}}
bool decode(std::span<const std::uint8_t> b,Result& r,std::uint32_t& sequence,std::uint16_t version)noexcept{try{
 if(!header(b,2,result_prefix,sequence,version))return false;const bool native=version==native_revision;auto p=b.data()+16;auto count=get(p+72,4);
 if(count!=b.size()-16-result_prefix||get(p,4)>(native?6u:7u)||get(p+4,4)>15||(get(p+8,4)&~(native?255ULL:127ULL))||(get(p+12,4)&~(native?511ULL:255ULL))||get(p+20,4)>2||get(p+76,4)||get(p+248,4)>3||(get(p+252,4)&~(native?15ULL:7ULL)))return false;
 Result value;auto& a=value.common;a.phase=static_cast<s::Phase>(get(p,4));a.reason=static_cast<s::Reason>(get(p+4,4));a.capabilities=static_cast<std::uint32_t>(get(p+8,4));a.evidence=static_cast<std::uint32_t>(get(p+12,4));a.server={static_cast<std::uint32_t>(get(p+16,4)),get(p+24,8)};a.session=get(p+32,8);a.operation=get(p+40,8);a.lifecycle=get(p+48,8);a.observation_qpc=std::bit_cast<std::int64_t>(get(p+56,8));a.offset=static_cast<std::uint32_t>(get(p+64,4));a.total=static_cast<std::uint32_t>(get(p+68,4));
 if(!a.server.pid||!a.server.creation_filetime||!a.session||!a.operation||!bounds(a.offset,a.total,static_cast<std::size_t>(count)))return false;
 std::copy_n(p+80,32,a.artifact_hash.data());std::copy_n(p+112,32,a.source_jar_hash.data());
 for(unsigned i=0;i<12;++i){a.readback[i]=std::bit_cast<double>(get(p+144+8*i,8));if(!std::isfinite(a.readback[i]))return false;}
 value.model=static_cast<std::uint32_t>(get(p+20,4));value.generation=get(p+240,8);value.entry_pause_mask=static_cast<std::uint32_t>(get(p+248,4));value.validity=static_cast<std::uint32_t>(get(p+252,4));unsigned offset=256;for(auto hash:{&value.profile,&value.navigation_database,&value.model_configuration,&value.restored_state,&value.preserved_state}){std::copy_n(p+offset,32,hash->data());offset+=32;}
 a.data.assign(p+result_prefix,p+result_prefix+count);r=std::move(value);return true;
}catch(...){return false;}}
}

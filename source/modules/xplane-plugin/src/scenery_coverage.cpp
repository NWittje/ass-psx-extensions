#include "psx_plugin/scenery_coverage.hpp"
#include <psx/non_normal/pipe.hpp>
#include <Windows.h>
#include <ShObjIdl.h>
#include <bcrypt.h>
#include <7z.h>
#include <7zFile.h>
#include <7zCrc.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <climits>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>
namespace psx::xp {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t max_header=65536,max_dsf=256u*1024*1024,max_alloc=384u*1024*1024;
// DSF's mandatory MD5 footer detects truncated/modified bytes. This is file
// format integrity, not authentication and not qualification of referenced assets.
struct DsfHash {
 BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};std::array<UCHAR,1024> object{};
 DsfHash(){
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_MD5_ALGORITHM,nullptr,0)<0)throw scenery::Reason::io;
  ULONG size{},got{};if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0)<0||size>object.size()||BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;throw scenery::Reason::io;}
 }
 ~DsfHash(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
 void update(const std::uint8_t* p,std::size_t n){if(n>ULONG_MAX||BCryptHashData(hash,const_cast<PUCHAR>(p),static_cast<ULONG>(n),0)<0)throw scenery::Reason::io;}
 void finish(const std::uint8_t* expected){std::array<UCHAR,16> digest{};if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)throw scenery::Reason::io;if(std::memcmp(digest.data(),expected,digest.size()))throw scenery::Reason::format;}
};
struct Budget {
 ISzAlloc api{allocate,release};std::size_t used{};
 struct alignas(std::max_align_t) Block {std::size_t bytes;};
 static void* allocate(ISzAllocPtr p,size_t n){auto& a=*reinterpret_cast<Budget*>(const_cast<ISzAlloc*>(p));if(n>max_alloc-a.used||n>SIZE_MAX-sizeof(Block))return nullptr;auto b=static_cast<Block*>(std::malloc(n+sizeof(Block)));if(!b)return nullptr;b->bytes=n;a.used+=n;return b+1;}
 static void release(ISzAllocPtr p,void* v){if(!v)return;auto& a=*reinterpret_cast<Budget*>(const_cast<ISzAlloc*>(p));auto b=static_cast<Block*>(v)-1;a.used-=b->bytes;std::free(b);}
};
std::vector<std::uint8_t> prefix(const fs::path& path){
 std::ifstream in(path,std::ios::binary);if(!in)throw scenery::Reason::io;
 std::vector<std::uint8_t> bytes(max_header);in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));bytes.resize(static_cast<std::size_t>(in.gcount()));
 if(bytes.size()<6||std::memcmp(bytes.data(),"7z\xBC\xAF\x27\x1C",6)){
  in.clear();in.seekg(0,std::ios::end);const auto end=in.tellg();if(end<28||end>static_cast<std::streamoff>(max_dsf))throw scenery::Reason::format;
  in.seekg(0);std::array<std::uint8_t,max_header> block{};auto remaining=static_cast<std::uint64_t>(end)-16;DsfHash hash;
  while(remaining){const auto n=static_cast<std::size_t>((std::min)(remaining,static_cast<std::uint64_t>(block.size())));in.read(reinterpret_cast<char*>(block.data()),static_cast<std::streamsize>(n));if(!in)throw scenery::Reason::io;hash.update(block.data(),n);remaining-=n;}
  std::array<std::uint8_t,16> footer{};in.read(reinterpret_cast<char*>(footer.data()),16);if(!in)throw scenery::Reason::io;hash.finish(footer.data());bytes.resize((std::min)(bytes.size(),static_cast<std::size_t>(end)-16));return bytes;
 }
 static std::once_flag crc;std::call_once(crc,CrcGenerateTable);
 CFileInStream file{};File_Construct(&file.file);FileInStream_CreateVTable(&file);
 if(InFile_OpenW(&file.file,path.c_str()))throw scenery::Reason::io;
 struct Close {CFileInStream& f;~Close(){File_Close(&f.file);}} close{file};
 Budget budget;CLookToRead2 stream{};LookToRead2_CreateVTable(&stream,0);stream.realStream=&file.vt;stream.buf=bytes.data();stream.bufSize=bytes.size();LookToRead2_INIT(&stream);
 CSzArEx archive;SzArEx_Init(&archive);
 struct Free {CSzArEx& a;Budget& b;Byte* output{};~Free(){ISzAlloc_Free(&b.api,output);SzArEx_Free(&a,&b.api);}} free{archive,budget};
 if(SzArEx_Open(&archive,&stream.vt,&budget.api,&budget.api)!=SZ_OK||archive.NumFiles!=1||SzArEx_IsDir(&archive,0)||SzArEx_GetFileSize(&archive,0)>max_dsf)throw scenery::Reason::format;
 UInt32 block=0xffffffff;size_t capacity{},offset{},processed{};
 if(SzArEx_Extract(&archive,&stream.vt,0,&block,&free.output,&capacity,&offset,&processed,&budget.api,&budget.api)!=SZ_OK||offset>capacity||processed>capacity-offset)throw scenery::Reason::format;
 if(processed<28)throw scenery::Reason::format;DsfHash hash;hash.update(free.output+offset,processed-16);hash.finish(free.output+offset+processed-16);
 return {free.output+offset,free.output+offset+(std::min)(processed-16,max_header)};
}
std::uint32_t u32(const std::vector<std::uint8_t>& b,std::size_t p){return std::uint32_t(b[p])|(std::uint32_t(b[p+1])<<8)|(std::uint32_t(b[p+2])<<16)|(std::uint32_t(b[p+3])<<24);}
bool overlay(const fs::path& path,int south,int west){
 const auto b=prefix(path);if(b.size()<28||std::memcmp(b.data(),"XPLNEDSF",8)||u32(b,8)!=1||std::memcmp(b.data()+12,"DAEH",4))throw scenery::Reason::format;
 const auto head=u32(b,16);if(head<16||head>b.size()-12)throw scenery::Reason::format;
 std::map<std::string,std::string> props;bool found=false;
 for(std::size_t p=20;p<12+head;){if(12+head-p<8)throw scenery::Reason::format;const auto n=u32(b,p+4);if(n<8||n>12+head-p)throw scenery::Reason::format;
  if(!std::memcmp(b.data()+p,"PORP",4)){
   if(found)throw scenery::Reason::format;found=true;std::size_t q=p+8;
   while(q<p+n){const auto read=[&](){const auto start=q;while(q<p+n&&b[q])++q;if(q==p+n)throw scenery::Reason::format;std::string s(reinterpret_cast<const char*>(b.data()+start),q-start);++q;return s;};const auto key=read();const auto value=read();if(!props.emplace(key,value).second&&(key=="sim/west"||key=="sim/east"||key=="sim/south"||key=="sim/north"||key=="sim/planet"||key=="sim/overlay"))throw scenery::Reason::format;}
  }p+=n;
 }
 const auto coordinate=[&](const char* name,int expected){const auto it=props.find(name);if(it==props.end())return false;int v{};auto r=std::from_chars(it->second.data(),it->second.data()+it->second.size(),v);return r.ec==std::errc{}&&r.ptr==it->second.data()+it->second.size()&&v==expected;};
 if(!found||!coordinate("sim/south",south)||!coordinate("sim/north",south+1)||!coordinate("sim/west",west)||!coordinate("sim/east",west+1)||(props.contains("sim/planet")&&props.at("sim/planet")!="earth"))throw scenery::Reason::format;
 if(!props.contains("sim/overlay"))return false;if(props.at("sim/overlay")!="1")throw scenery::Reason::format;return true;
}
std::string trim(std::string s){const auto first=s.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};const auto last=s.find_last_not_of(" \t\r\n");return s.substr(first,last-first+1);}
fs::path linked(fs::path p){
 while(p!=p.root_path()&&p.filename().empty())p=p.parent_path();
 if(p.extension()!=L".lnk")return p;
 const auto init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(init))throw scenery::Reason::io;
 struct Com {~Com(){CoUninitialize();}} com;IShellLinkW* link{};
 if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))))throw scenery::Reason::io;
 struct Release {IShellLinkW* p;~Release(){p->Release();}} release{link};IPersistFile* file{};
 if(FAILED(link->QueryInterface(IID_PPV_ARGS(&file))))throw scenery::Reason::io;
 const auto loaded=file->Load(p.c_str(),STGM_READ);file->Release();if(FAILED(loaded))throw scenery::Reason::io;
 std::array<wchar_t,32768> target{};if(FAILED(link->GetPath(target.data(),static_cast<int>(target.size()),nullptr,SLGP_RAWPATH))||!target[0])throw scenery::Reason::io;
 return fs::path(target.data());
}
std::string tile_name(int south,int west){char b[16]{};std::snprintf(b,sizeof(b),"%+03d%+04d",south,west);return b;}
std::string metadata(const fs::path& p){std::error_code e;const bool exists=fs::exists(p,e);if(e)throw scenery::Reason::io;if(!exists)return "absent";if(!fs::is_regular_file(p,e)||e)throw scenery::Reason::io;const auto size=fs::file_size(p,e);if(e)throw scenery::Reason::io;const auto time=fs::last_write_time(p,e);if(e)throw scenery::Reason::io;return std::to_string(size)+":"+std::to_string(time.time_since_epoch().count());}
}
struct SceneryCoverage::Impl {
 fs::path root;std::uint64_t generation{};std::string last;std::map<fs::path,std::string> known;
 explicit Impl(fs::path p):root(std::move(p)){}
 struct Inventory {std::vector<fs::path> packs;std::string identity;};
 Inventory inventory(){
  Inventory v;const auto ini=root/"Custom Scenery/scenery_packs.ini";if(fs::file_size(ini)>4*1024*1024)throw scenery::Reason::format;std::ifstream f(ini,std::ios::binary);if(!f)throw scenery::Reason::io;
  std::string text((std::istreambuf_iterator<char>(f)),{});if(f.bad()||text.size()>4*1024*1024)throw scenery::Reason::io;v.identity=text;
  if(text.starts_with("\xEF\xBB\xBF"))text.erase(0,3);
  std::istringstream lines(text);std::string line;
  while(std::getline(lines,line)){line=trim(line);if(line.starts_with("SCENERY_PACK ")){auto value=trim(line.substr(13));if(value=="*GLOBAL_AIRPORTS*")continue;if(value.empty())throw scenery::Reason::format;auto p=fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));if(p.is_relative())p=root/p;p=linked(p);std::error_code e;if(!fs::is_directory(p,e)||e)throw scenery::Reason::io;v.packs.push_back(p);if(v.packs.size()>4096)throw scenery::Reason::busy;}}
  const auto global=root/"Global Scenery";std::error_code e;if(!fs::is_directory(global,e)||e)throw scenery::Reason::io;
  std::vector<fs::path> stock;for(const auto& item:fs::directory_iterator(global)){if(item.is_directory())stock.push_back(item.path());}
  const auto defaults=root/"Resources/default scenery";
  e.clear();if(fs::exists(defaults,e)){if(e||!fs::is_directory(defaults))throw scenery::Reason::io;for(const auto& item:fs::directory_iterator(defaults))if(item.is_directory())stock.push_back(item.path());}else if(e)throw scenery::Reason::io;
  if(stock.size()+v.packs.size()>4096)throw scenery::Reason::busy;
  std::sort(stock.begin(),stock.end());for(const auto& p:stock){v.packs.push_back(p);const auto encoded=p.generic_u8string();v.identity.append(reinterpret_cast<const char*>(encoded.data()),encoded.size());}
  return v;
 }
 scenery::Result inspect(const scenery::Query& query){
  scenery::Result result;result.query=query;result.reason=scenery::Reason::invalid;int south{},west{};if(!scenery::valid(query)||!scenery::tile(query.latitude_deg,query.longitude_deg,south,west))return result;
  auto before=inventory();const auto folder=tile_name(static_cast<int>(std::floor(south/10.0))*10,static_cast<int>(std::floor(west/10.0))*10);
  bool available=false;std::vector<std::pair<fs::path,std::string>> inspected;
  for(const auto& pack:before.packs){const auto path=pack/"Earth nav data"/folder/(tile_name(south,west)+".dsf");const auto m=metadata(path);inspected.emplace_back(path,m);if(m=="absent")continue;if(!overlay(path,south,west)){available=true;break;}}
  const auto after=inventory();if(before.identity!=after.identity||before.packs!=after.packs)throw scenery::Reason::changed;
  for(const auto& item:inspected)if(metadata(item.first)!=item.second)throw scenery::Reason::changed;
  bool changed=before.identity!=last;
  for(const auto& item:inspected){const auto it=known.find(item.first);if(it!=known.end()&&it->second!=item.second)changed=true;if(known.size()>=8192){known.clear();changed=true;}known[item.first]=item.second;}
  if(changed||!generation){++generation;last=std::move(before.identity);}result.south=south;result.west=west;result.inventory_generation=generation;result.verdict=available?scenery::Verdict::available:scenery::Verdict::missing;result.reason=scenery::Reason::none;return result;
 }
};
SceneryCoverage::SceneryCoverage(std::filesystem::path root):impl_(std::make_unique<Impl>(std::move(root))){}
SceneryCoverage::~SceneryCoverage()=default;
scenery::Result SceneryCoverage::inspect(const scenery::Query& q) noexcept {try{return impl_->inspect(q);}catch(scenery::Reason reason){scenery::Result r;r.query=q;r.reason=reason;return r;}catch(...){scenery::Result r;r.query=q;r.reason=scenery::Reason::io;return r;}}
}

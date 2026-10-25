#include <psx/instructor/maximum_speed.hpp>
#include <psx/instructor/communicator.hpp>
#include "metadata_json.hpp"
#include <array>
#include <map>
#include <variant>

namespace psx::instructor {
namespace {
[[noreturn]] void unavailable(const char *why) {throw std::runtime_error(std::string("PSX maximum speed unavailable: ")+why);}
double number(metadata::Json &parser) {
  const auto begin=parser.at;const double value=parser.real();
  const auto token=parser.s.substr(begin,parser.at-begin);std::size_t at{};
  if(at<token.size()&&token[at]=='-')++at;
  auto digits=[&]{const auto start=at;while(at<token.size()&&token[at]>='0'&&token[at]<='9')++at;return at>start;};
  if(!digits())unavailable("invalid JSON number");
  if(at<token.size()&&token[at]=='.'){++at;if(!digits())unavailable("invalid JSON fraction");}
  if(at<token.size()&&(token[at]=='e'||token[at]=='E')){
    ++at;if(at<token.size()&&(token[at]=='+'||token[at]=='-'))++at;
    if(!digits())unavailable("invalid JSON exponent");
  }
  if(at!=token.size())unavailable("invalid JSON number");return value;
}
}
MaximumSpeedSelection::MaximumSpeedSelection(std::filesystem::path directory):directory_(std::move(directory)){}
MaximumSpeed MaximumSpeedSelection::parse(std::string_view bytes,std::uint32_t pid,std::uint64_t birth,std::uint64_t now) {
  if(bytes.empty()||bytes.size()>4096)unavailable("metadata bounds");
  metadata::Json parser{bytes};using Value=std::variant<std::monostate,std::string,double>;std::map<std::string,Value> fields;
  parser.expect('{');if(!parser.take('}'))for(;;) {
    auto name=parser.str();parser.expect(':');parser.ws();Value value;
    if(parser.at<bytes.size()&&bytes[parser.at]=='"')value=parser.str();
    else if(bytes.substr(parser.at,4)=="null")parser.at+=4;
    else value=number(parser);
    if(!fields.emplace(name,std::move(value)).second||fields.size()>8)unavailable("duplicate or unknown field");
    if(parser.take('}'))break;parser.expect(',');
  }
  parser.ws();if(parser.at!=bytes.size()||fields.size()!=8)unavailable("incomplete metadata");
  auto whole=[&](const char *name) {
    const auto it=fields.find(name);if(it==fields.end())unavailable("missing identity field");
    const auto value=std::get_if<double>(&it->second);
    if(!value||*value<=0||*value>9007199254740991.||std::trunc(*value)!=*value)unavailable("invalid identity field");
    return std::uint64_t(*value);
  };
  if(whole("schema")!=1)unavailable("unsupported schema");
  if(!pid||!birth||whole("pid")!=pid||whole("process_start_unix_ms")!=birth)unavailable("process does not own Main");
  MaximumSpeed result{pid,birth,whole("sequence"),whole("sampled_unix_ms"),whole("lifecycle"),{}, {}};
  if(result.sampled_unix_ms>now||now-result.sampled_unix_ms>=1000)unavailable("stale or future source");
  auto error=fields.find("error"),limit=fields.find("max_speed_kt");
  if(error==fields.end()||limit==fields.end()||!std::holds_alternative<std::string>(error->second))unavailable("missing source status");
  result.notice=std::get<std::string>(error->second);
  if(result.notice.empty()) {
    const auto value=std::get_if<double>(&limit->second);
    if(!value||*value<=0||*value>450)unavailable("invalid indicated limit");
    result.max_speed_kt=*value;
  }else if(!std::holds_alternative<std::monostate>(limit->second))unavailable("inconsistent unavailable evidence");
  return result;
}
MaximumSpeed MaximumSpeedSelection::read(const NavigationSelection &selection,std::uint64_t now) {
  if(!selection.pid||!selection.process_start_unix_ms)unavailable("local Main identity unavailable");
  const auto directory=directory_.empty()?(selection.metadata_directory.empty()?NavigationDatabaseSelection::default_directory():selection.metadata_directory).parent_path()/L"maximum-speed":directory_;
  const auto path=directory/(std::to_wstring(selection.pid)+L".json");
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)unavailable("exporter metadata not active");
  std::array<char,4097> bytes{};DWORD count{};
  const bool okay=ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=FALSE;CloseHandle(file);
  if(!okay)unavailable("cannot read metadata");
  const std::string_view view(bytes.data(),count);auto result=parse(view,selection.pid,selection.process_start_unix_ms,now);
  if(last_&&last_->pid==result.pid&&last_->process_start_unix_ms==result.process_start_unix_ms) {
    if(result.sequence==last_->sequence&&last_bytes_==view)return *last_;
    if(result.sequence<=last_->sequence||result.lifecycle<last_->lifecycle)unavailable("regressed or changed same-sequence evidence");
  }
  last_=result;last_bytes_=view;return result;
}
}

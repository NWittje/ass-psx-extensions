#include "hwio/output_atlas.hpp"
#include "hwio/core.hpp"
#include <Windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
namespace hwio {
namespace {
bool rectangle(const AtlasRectangle& r){return std::isfinite(r.left)&&std::isfinite(r.top)&&std::isfinite(r.right)&&std::isfinite(r.bottom)&&r.left<r.right&&r.top<r.bottom&&std::max({std::abs(r.left),std::abs(r.top),std::abs(r.right),std::abs(r.bottom)})<1e7;}
bool includes(const AtlasRectangle& a,const AtlasRectangle& b){return a.left<=b.left&&a.top<=b.top&&a.right>=b.right&&a.bottom>=b.bottom;}
bool relative_image(const std::filesystem::path& p){if(p.empty()||p.is_absolute()||p.has_root_name()||p.native().size()>200)return false;for(const auto& part:p)if(part==L".."||part==L".")return false;return p.extension()==L".png";}
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
void check(HRESULT r){if(FAILED(r))throw std::runtime_error("Output atlas XML read failed (HRESULT "+std::to_string(r)+")");}
template<class T>struct Com {T* p{};~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->()const{return p;}};
using Attributes=std::map<std::string,std::string>;
Attributes attributes(IXmlReader* reader){Attributes a;auto status=reader->MoveToFirstAttribute();while(status==S_OK){const wchar_t *name{},*value{};UINT n{},v{};check(reader->GetLocalName(&name,&n));check(reader->GetValue(&value,&v));require(n<80&&v<2048&&a.size()<12,"Output atlas attribute bounds exceeded");require(a.emplace(utf8(std::wstring(name,n)),utf8(std::wstring(value,v))).second,"Duplicate output atlas attribute");status=reader->MoveToNextAttribute();}check(status);check(reader->MoveToElement());return a;}
std::string take(Attributes& a,const char* name){const auto it=a.find(name);require(it!=a.end(),"Missing output atlas attribute");auto value=std::move(it->second);a.erase(it);return value;}
double coordinate(Attributes& a,const char* name){auto s=take(a,name);double n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);require(r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&std::isfinite(n),"Invalid output atlas coordinate");return n;}
AtlasRectangle bounds(Attributes& a){return {coordinate(a,"left"),coordinate(a,"top"),coordinate(a,"right"),coordinate(a,"bottom")};}
}
bool AtlasRectangle::contains(double x,double y)const noexcept{return x>=left&&x<right&&y>=top&&y<bottom;}
bool OutputAtlas::replace(std::vector<OutputAtlasImage> images,std::vector<OutputAtlasRegion> regions,bool complete,bool require_complete,std::span<const std::string> required_outputs,Qualify qualify,void* user,std::string& error,std::vector<OutputAtlasDeferred> deferred){
    try{
        require(!require_complete||complete||!deferred.empty(),"The output atlas is incomplete; production identification is unavailable");
        require(deferred.size()<=4096&&(!complete||deferred.empty()),"A complete output atlas cannot defer illumination");
        require(!images.empty()&&images.size()<=64&&!regions.empty()&&regions.size()<=4096,"Output atlas inventory bounds exceeded");
        std::set<std::string> image_ids,region_ids,outputs;
        for(const auto& image:images)require(!image.id.empty()&&image.id.size()<=128&&image_ids.insert(image.id).second&&relative_image(image.file)&&rectangle(image.world),"Invalid output atlas image");
        require(regions[0].parent==OutputAtlasRegion::none&&regions[0].id=="flightdeck","Output atlas must begin at the complete flightdeck");
        std::vector<std::uint32_t> offsets(regions.size()+1),children(regions.size()-1);
        for(std::size_t n=0;n<regions.size();++n){const auto& region=regions[n];require(!region.id.empty()&&region.id.size()<=128&&region.label.size()<=240&&region_ids.insert(region.id).second&&rectangle(region.bounds),"Invalid output atlas region");require(region.image<images.size()&&includes(images[region.image].world,region.bounds),"Output atlas region lies outside its image");if(n){require(region.parent<regions.size()&&region.parent!=n&&includes(regions[region.parent].bounds,region.bounds),"Invalid output atlas parent");++offsets[region.parent+1];}if(!region.output.empty()){require(region.output.size()<=128&&region.output.starts_with("out.")&&qualify&&qualify(user,region.output),"Output atlas references unavailable illumination");outputs.insert(region.output);}}
        for(std::size_t n=1;n<offsets.size();++n)offsets[n]+=offsets[n-1];auto cursor=offsets;
        for(std::uint32_t n=1;n<regions.size();++n)children[cursor[regions[n].parent]++]=n;
        std::vector<std::uint32_t> visits{0};visits.reserve(regions.size());
        for(std::size_t n=0;n<visits.size();++n){const auto index=visits[n];require(regions[index].output.empty()||offsets[index]==offsets[index+1],"An assignable lamp cannot contain navigation regions");for(auto k=offsets[index];k<offsets[index+1];++k)visits.push_back(children[k]);}
        require(visits.size()==regions.size(),"Output atlas contains an unreachable or cyclic region");
        std::set<std::string> excluded;
        for(const auto& d:deferred)require(!d.output.empty()&&d.output.size()<=128&&!d.reason.empty()&&d.reason.size()<=240&&qualify&&qualify(user,d.output)&&excluded.insert(d.output).second&&!outputs.contains(d.output),"Invalid, duplicate or already placed deferred illumination");
        // Partial human artwork is usable only with explicit qualified omissions.
        // A completeness flag alone never exempts a missing catalogue entry.
        if(require_complete){require(!required_outputs.empty(),"Production atlas requires an independent catalogue coverage list");for(const auto& output:required_outputs)require(outputs.contains(output)||excluded.contains(output),"Output atlas does not account for every required illumination definition");}
        images_=std::move(images);regions_=std::move(regions);deferred_=std::move(deferred);children_=std::move(children);offsets_=std::move(offsets);complete_=complete;error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
std::span<const std::uint32_t> OutputAtlas::children(std::uint32_t index)const noexcept{if(index>=regions_.size())return {};return std::span<const std::uint32_t>(children_).subspan(offsets_[index],offsets_[index+1]-offsets_[index]);}
std::uint32_t OutputAtlas::hit(std::uint32_t index,double x,double y)const noexcept{auto found=OutputAtlasRegion::none;double area=1e300;bool ambiguous=false;for(auto child:children(index)){const auto& r=regions_[child].bounds;if(r.contains(x,y)){const auto candidate=(r.right-r.left)*(r.bottom-r.top);if(candidate<area){found=child;area=candidate;ambiguous=false;}else if(candidate==area)ambiguous=true;}}return ambiguous?OutputAtlasRegion::none:found;}
std::vector<std::uint32_t> OutputAtlas::path(std::uint32_t index)const{std::vector<std::uint32_t> result;if(index>=regions_.size())return result;while(index!=OutputAtlasRegion::none){result.push_back(index);index=regions_[index].parent;}std::reverse(result.begin(),result.end());return result;}
bool OutputAtlas::load(const std::filesystem::path& file,bool require_complete,std::span<const std::string> required_outputs,Qualify qualify,void* user,std::string& error){
    try{
        require(std::filesystem::file_size(file)<=1024*1024,"Output atlas exceeds one MiB");
        Com<IStream> stream;check(SHCreateStreamOnFileEx(file.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,stream.out()));
        Com<IXmlReader> xml;check(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(xml.out()),nullptr));check(xml->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit));check(xml->SetProperty(XmlReaderProperty_MaxElementDepth,4));check(xml->SetInput(stream.p));
        std::vector<OutputAtlasImage> images;std::vector<OutputAtlasRegion> regions;std::vector<OutputAtlasDeferred> deferred;std::vector<std::pair<std::string,std::string>> links;
        std::map<std::string,std::uint32_t> image_index,region_index;bool root{},closed{},complete{},scoped{};XmlNodeType node{};HRESULT status{};
        while((status=xml->Read(&node))==S_OK){
            if(node==XmlNodeType_Element){UINT depth{};check(xml->GetDepth(&depth));const wchar_t* tag{};check(xml->GetLocalName(&tag,nullptr));const std::wstring name(tag);auto a=attributes(xml.p);
                if(depth==0){require(!root&&!closed&&std::wstring_view(name)==L"output-atlas","Invalid output atlas root");const auto version=take(a,"version");require(version=="1"||version=="2","Unsupported output atlas version");scoped=version=="2";const auto value=take(a,"complete");require(value=="true"||value=="false","Invalid output atlas completeness");complete=value=="true";root=true;require(!xml->IsEmptyElement(),"Empty output atlas");}
                else {require(root&&!closed&&depth==1&&xml->IsEmptyElement(),"Unexpected output atlas nesting");
                    if(std::wstring_view(name)==L"image"){require(images.size()<64,"Too many output atlas images");OutputAtlasImage image;image.id=take(a,"id");image.file=std::filesystem::path(wide(take(a,"file")));image.world=bounds(a);require(image_index.emplace(image.id,static_cast<std::uint32_t>(images.size())).second,"Duplicate output atlas image");images.push_back(std::move(image));}
                    else if(std::wstring_view(name)==L"region"){require(regions.size()<4096,"Too many output atlas regions");OutputAtlasRegion region;region.id=take(a,"id");region.label=take(a,"label");const auto parent=take(a,"parent"),image=take(a,"image");region.output=take(a,"output");region.bounds=bounds(a);require(region_index.emplace(region.id,static_cast<std::uint32_t>(regions.size())).second,"Duplicate output atlas region");regions.push_back(std::move(region));links.push_back({parent,image});}
                    else if(std::wstring_view(name)==L"deferred"){require(scoped&&deferred.size()<4096,"Unexpected deferred illumination");deferred.push_back({take(a,"output"),take(a,"reason")});}
                    else throw std::runtime_error("Unknown output atlas element");
                }require(a.empty(),"Unknown output atlas attribute");
            }else if(node==XmlNodeType_EndElement){
                // Native XmlLite retains the open element in GetDepth until
                // advancing past its closing token (root EndElement depth=1).
                const wchar_t* name{};check(xml->GetLocalName(&name,nullptr));require(root&&!closed&&std::wstring_view(name)==L"output-atlas","Unexpected output atlas closing element");closed=true;
            }
            else require(node==XmlNodeType_Whitespace||node==XmlNodeType_XmlDeclaration||node==XmlNodeType_Comment,"Unexpected output atlas content");
        }check(status);require(root&&closed,"Incomplete output atlas document");
        for(std::size_t n=0;n<regions.size();++n){auto& region=regions[n];const auto& [parent,image]=links[n];if(!parent.empty()){require(region_index.contains(parent),"Unknown output atlas parent");region.parent=region_index.at(parent);}require(image_index.contains(image),"Unknown output atlas image");region.image=image_index.at(image);}
        return replace(std::move(images),std::move(regions),complete,require_complete,required_outputs,qualify,user,error,std::move(deferred));
    }catch(const std::exception& e){error=e.what();return false;}
}
}

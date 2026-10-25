#include <ios/situations.hpp>
#include <shobjidl.h>
#include <shellapi.h>
#include <atomic>
#include <memory>

namespace ios {
bool situation_recycle_allowed(const SituationRecycleItem& item,std::uint32_t flags,std::filesystem::path source){
  if(!(flags&TSF_DELETE_RECYCLE_IF_POSSIBLE))return false;
  try{source=source.lexically_normal();source.make_preferred();auto expected=item.path.lexically_normal();expected.make_preferred();return _wcsicmp(source.c_str(),expected.c_str())==0&&SituationStore::file_hash(item.path)==item.hash;}catch(...){return false;}
}
namespace {
template<class T>struct Release {void operator()(T* value)const{if(value)value->Release();}};
template<class T>using Com=std::unique_ptr<T,Release<T>>;
class RecycleSink final:public IFileOperationProgressSink {
  std::atomic<ULONG> references_{1};
  const SituationRecycleItem& item_;
public:
  bool recycled{};
  explicit RecycleSink(const SituationRecycleItem& item):item_(item){}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
    if(!out)return E_POINTER;*out=nullptr;
    if(id==__uuidof(IUnknown)||id==__uuidof(IFileOperationProgressSink)){*out=static_cast<IFileOperationProgressSink*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
  ULONG STDMETHODCALLTYPE Release()override{const auto remaining=--references_;if(!remaining)delete this;return remaining;}
  HRESULT STDMETHODCALLTYPE StartOperations()override{return S_OK;}
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT)override{return S_OK;}
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD,IShellItem*,LPCWSTR)override{return E_ABORT;}
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR)override{return E_ABORT;}
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR)override{return E_ABORT;}
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags,IShellItem* item)override{
    // Confirm covers recycling only. Reject a non-recycle delete notification
    // and changed bytes instead of silently accepting permanent destruction.
    if(!item)return E_ABORT;
    PWSTR path=nullptr;if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))||!path){CoTaskMemFree(path);return E_ABORT;}
    bool matches=false;try{matches=situation_recycle_allowed(item_,flags,std::filesystem::path(path));}catch(...){}CoTaskMemFree(path);return matches?S_OK:E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,IShellItem*,HRESULT result,IShellItem* recycled_item)override{
    recycled=SUCCEEDED(result)&&recycled_item!=nullptr;return recycled?S_OK:E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD,IShellItem*,LPCWSTR)override{return E_ABORT;}
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,IShellItem*,LPCWSTR,LPCWSTR,DWORD,HRESULT,IShellItem*)override{return S_OK;}
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT,UINT)override{return S_OK;}
  HRESULT STDMETHODCALLTYPE ResetTimer()override{return S_OK;}
  HRESULT STDMETHODCALLTYPE PauseTimer()override{return S_OK;}
  HRESULT STDMETHODCALLTYPE ResumeTimer()override{return S_OK;}
};
}
SituationRecycleResult recycle_situations(const std::vector<SituationRecycleItem>& items){
  SituationRecycleResult result;
  if(items.empty()){result.error="Select at least one situation";return result;}
  const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  if(FAILED(initialized)){result.error="Windows Recycle Bin unavailable";return result;}
  struct Apartment{~Apartment(){CoUninitialize();}} apartment;
  IFileOperation* raw=nullptr;
  if(FAILED(CoCreateInstance(__uuidof(FileOperation),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)))){result.error="Windows Recycle Bin unavailable";return result;}
  Com<IFileOperation> operation(raw);
  // This is the application's user-requested Recycle Bin action, not a general
  // file transfer or work-folder cleanup. There is no permanent-delete fallback.
  if(FAILED(operation->SetOperationFlags(FOF_SILENT|FOF_NOCONFIRMATION|FOF_NOERRORUI|FOFX_EARLYFAILURE|FOFX_RECYCLEONDELETE|FOFX_ADDUNDORECORD))){result.error="Windows Recycle Bin unavailable";return result;}
  std::vector<Com<RecycleSink>> sinks;sinks.reserve(items.size());
  for(const auto& item:items){
    IShellItem* shell=nullptr;
    if(FAILED(SHCreateItemFromParsingName(item.path.c_str(),nullptr,IID_PPV_ARGS(&shell)))){result.error="Situation file unavailable";return result;}
    Com<IShellItem> source(shell);Com<RecycleSink> sink(new RecycleSink(item));
    if(FAILED(operation->DeleteItem(source.get(),sink.get()))){result.error="Situation could not be moved to the Recycle Bin";return result;}
    sinks.push_back(std::move(sink));
  }
  const auto completed=operation->PerformOperations();BOOL aborted=FALSE;const auto observed=operation->GetAnyOperationsAborted(&aborted);
  for(std::size_t i=0;i<sinks.size();++i)if(sinks[i]->recycled)result.recycled.push_back(i);
  if(FAILED(completed)||FAILED(observed)||aborted||result.recycled.size()!=items.size())result.error="Not all situations could be moved to the Recycle Bin";
  return result;
}
}

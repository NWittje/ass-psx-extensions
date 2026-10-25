#include <psx/sync/worker_io.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace psx::sync {
namespace {
thread_local WorkerIo* current;
void complete(OVERLAPPED* op,DWORD code,DWORD bytes)noexcept{op->Internal=code;op->InternalHigh=bytes;SetEvent(op->hEvent);}
}
WorkerIo::WorkerIo(Family f):port_(f),previous_(current){port_.attach();current=this;}
WorkerIo::~WorkerIo(){io_cancel(nullptr,nullptr);current=previous_;}
bool WorkerIo::read_now(void* data,DWORD size,DWORD& read){Frame f;if(!port_.receive(f))return false;if(f.size>size)throw std::runtime_error("Synchronization frame exceeds service buffer");std::memcpy(data,f.bytes.data(),f.size);read=DWORD(f.size);return true;}
bool WorkerIo::write_now(const void* data,DWORD size,DWORD& wrote){if(!port_.send({static_cast<const std::uint8_t*>(data),size}))return false;wrote=size;return true;}
bool WorkerIo::poll()noexcept{try{if(port_.changed())return false;DWORD n{};
 if(read_.op&&read_now(read_.data,read_.size,n)){complete(read_.op,ERROR_SUCCESS,n);read_={};}
 if(write_.op&&write_now(write_.data,write_.size,n)){complete(write_.op,ERROR_SUCCESS,n);write_={};}
 return true;}catch(...){return false;}}
BOOL io_read(HANDLE h,LPVOID b,DWORD n,LPDWORD got,LPOVERLAPPED op)noexcept{
 if(h)return ::ReadFile(h,b,n,got,op);if(!current||!op){SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
 try{DWORD count{};if(current->read_now(b,n,count)){if(got)*got=count;complete(op,ERROR_SUCCESS,count);return TRUE;}
  if(current->read_.op){SetLastError(ERROR_INVALID_FUNCTION);return FALSE;}current->read_={op,b,n};complete(op,ERROR_IO_PENDING,0);ResetEvent(op->hEvent);SetLastError(ERROR_IO_PENDING);return FALSE;
 }catch(...){SetLastError(ERROR_INVALID_DATA);return FALSE;}
}
BOOL io_write(HANDLE h,LPCVOID b,DWORD n,LPDWORD wrote,LPOVERLAPPED op)noexcept{
 if(h)return ::WriteFile(h,b,n,wrote,op);if(!current||!op){SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
 try{DWORD count{};if(current->write_now(b,n,count)){if(wrote)*wrote=count;complete(op,ERROR_SUCCESS,count);return TRUE;}
  if(current->write_.op){SetLastError(ERROR_INVALID_FUNCTION);return FALSE;}current->write_={op,const_cast<void*>(b),n};complete(op,ERROR_IO_PENDING,0);ResetEvent(op->hEvent);SetLastError(ERROR_IO_PENDING);return FALSE;
 }catch(...){SetLastError(ERROR_INVALID_DATA);return FALSE;}
}
BOOL io_result(HANDLE h,LPOVERLAPPED op,LPDWORD count,BOOL wait)noexcept{
 if(h)return ::GetOverlappedResult(h,op,count,wait);if(!current||!op){SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
 current->poll();if(op->Internal==ERROR_IO_PENDING){SetLastError(ERROR_IO_INCOMPLETE);return FALSE;}
 if(op->Internal!=ERROR_SUCCESS){SetLastError(DWORD(op->Internal));return FALSE;}if(count)*count=DWORD(op->InternalHigh);return TRUE;
}
BOOL io_cancel(HANDLE h,LPOVERLAPPED op)noexcept{
 if(h)return ::CancelIoEx(h,op);if(!current){SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
 for(auto* p:{&current->read_,&current->write_})if(p->op&&(!op||p->op==op)){complete(p->op,ERROR_OPERATION_ABORTED,0);*p={};}return TRUE;
}
bool worker_poll(HANDLE transport)noexcept{return transport?local_io_selected():current&&current->poll();}
DWORD wait_local_connection(HANDLE stop,HANDLE completion)noexcept{HANDLE handles[]{stop,completion};while(local_io_selected()){const auto result=WaitForMultipleObjects(2,handles,FALSE,20);if(result!=WAIT_TIMEOUT)return result;}return WAIT_TIMEOUT;}
bool worker_receive(Frame& frame)noexcept{try{return current&&current->port_.receive(frame);}catch(...){return false;}}
bool worker_send(std::span<const std::uint8_t> frame)noexcept{try{return current&&current->port_.send(frame);}catch(...){return false;}}
}

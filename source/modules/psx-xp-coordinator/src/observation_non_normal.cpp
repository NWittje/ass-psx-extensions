#include "observation_internal.hpp"
namespace coordinator {
bool ObservationController::start_non_normal(){
 const auto endpoint=impl_->test_bridge_name.empty()?std::wstring{}:impl_->test_bridge_name+L".coverage";
 impl_->coverage_server.start([](void* p,const psx::scenery::Query& q,std::uint32_t ms) noexcept{return static_cast<Impl*>(p)->check_coverage(q,ms);},impl_.get(),endpoint);
 return impl_->non_normal.start();
}
void ObservationController::stop_non_normal()noexcept{impl_->coverage_server.request_stop();impl_->non_normal.request_stop();}
void ObservationController::join_non_normal(){impl_->coverage_server.stop();impl_->non_normal.join();}
bool ObservationController::non_normal_done()const noexcept{return impl_->non_normal.done();}
NonNormalView ObservationController::non_normal_status()const{return impl_->non_normal.snapshot();}
void ObservationController::Impl::drain_non_normal(){
 ProcessIdentity selected,psx;
 {std::lock_guard<std::mutex> lock(mutex);if(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0){psx=traffic.view().selected;if(view.bridge==State::connected&&view.bridge_peer_process_id)selected=terrain_process_identity(view.bridge_peer_process_id);}}
 non_normal.associate_psx({psx.pid,psx.creation_filetime});
 non_normal.select({selected.pid,selected.creation_filetime});
}
}

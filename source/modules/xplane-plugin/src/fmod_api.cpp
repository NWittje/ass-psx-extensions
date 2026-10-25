#include "psx_plugin/audio_renderer.hpp"
#include "psx_plugin/audio_assets.hpp"
#include <Windows.h>
#include <cstring>
namespace psx::xp {
namespace {
template<class F> bool symbol(HMODULE module,const char* name,F& out) noexcept {
    const auto address=GetProcAddress(module,name);static_assert(sizeof(out)==sizeof(address));
    std::memcpy(&out,&address,sizeof(out));return out!=nullptr;
}
}
bool FmodApi::resolve(void* borrowed) noexcept {
    *this={};if(!borrowed||!qualify_audio_host())return false;
    const auto dll=GetModuleHandleW(L"fmodstudio.dll"),core=GetModuleHandleW(L"fmod.dll");if(!dll||!core)return false;
    int (*get_core)(void*,void**){};int (*version)(void*,unsigned*){};void* system{};unsigned v{};
    if(!symbol(dll,"FMOD_Studio_System_GetCoreSystem",get_core)||!symbol(core,"FMOD_System_GetVersion",version)||get_core(borrowed,&system)||version(system,&v)||v!=0x00020229)return false;
    bool ok=true;
#define FMOD_RESOLVE(member,name) ok=symbol(dll,name,member)&&ok
    FMOD_RESOLVE(event,"FMOD_Studio_System_GetEventByID");
    FMOD_RESOLVE(event_id,"FMOD_Studio_EventDescription_GetID");
    FMOD_RESOLVE(is3d,"FMOD_Studio_EventDescription_Is3D");
    FMOD_RESOLVE(parameter_count,"FMOD_Studio_EventDescription_GetParameterDescriptionCount");
    FMOD_RESOLVE(parameter,"FMOD_Studio_EventDescription_GetParameterDescriptionByIndex");
    FMOD_RESOLVE(create,"FMOD_Studio_EventDescription_CreateInstance");
    FMOD_RESOLVE(load_samples,"FMOD_Studio_EventDescription_LoadSampleData");
    FMOD_RESOLVE(unload_samples,"FMOD_Studio_EventDescription_UnloadSampleData");
    FMOD_RESOLVE(sample_state,"FMOD_Studio_EventDescription_GetSampleLoadingState");
    FMOD_RESOLVE(oneshot,"FMOD_Studio_EventDescription_IsOneshot");
    FMOD_RESOLVE(set_parameter,"FMOD_Studio_EventInstance_SetParameterByID");
    FMOD_RESOLVE(set_attributes,"FMOD_Studio_EventInstance_Set3DAttributes");
    FMOD_RESOLVE(set_volume,"FMOD_Studio_EventInstance_SetVolume");
    FMOD_RESOLVE(start,"FMOD_Studio_EventInstance_Start");
    FMOD_RESOLVE(stop,"FMOD_Studio_EventInstance_Stop");
    FMOD_RESOLVE(release,"FMOD_Studio_EventInstance_Release");
    FMOD_RESOLVE(playback,"FMOD_Studio_EventInstance_GetPlaybackState");
    FMOD_RESOLVE(listener,"FMOD_Studio_System_GetListenerAttributes");
    FMOD_RESOLVE(channel_group,"FMOD_Studio_EventInstance_GetChannelGroup");
#undef FMOD_RESOLVE
    ok=symbol(core,"FMOD_ChannelGroup_SetMute",group_mute)&&ok;
    ok=symbol(core,"FMOD_ChannelGroup_GetMute",group_get_mute)&&ok;
    if(!ok){*this={};return false;}studio=borrowed;return true;
}
}

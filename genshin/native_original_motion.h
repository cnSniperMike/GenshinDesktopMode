#pragma once
#include "native_original_motion_state.h"

namespace NativeOriginalMotion {
using SetRTPC=int (*)(uint32_t,float,uint64_t,int,int,bool);
struct Key {const void *bus;uint64_t listener;};
using Resolve=void *(*)(const Key *);
static std::atomic<SetRTPC> originalSet{nullptr};
static std::atomic<Resolve> originalResolve{nullptr};
static NativeMotion::OriginalCapabilities capabilities;
static NativeMotion::OriginalMotionActionRelay actionRelay;
static void **managerCell;
static bool installed=false,attempted=false;
static thread_local unsigned ownWrites=0;
static std::atomic<uint64_t> redirects{0},preservedRoutes{0},observedGameWrites{0};
static constexpr uint32_t observedEvents[]={
    0x0c2baa48,0x26860fe2,0x6697ee02,0x6e94fdad,0x916bd377,0xf8481152
};
static std::atomic<uint64_t> eventCounts[6]{};

template<class T>static T ReadAt(const void *object,size_t offset){
    T value{};memcpy(&value,(const char *)object+offset,sizeof(value));return value;
}
static int Set(uint32_t id,float value,uint64_t owner,int time,int curve,bool bypass){
    float actual=ownWrites?value:capabilities.effective(id,value,owner);
    int result=originalSet.load(std::memory_order_acquire)(id,actual,owner,time,curve,bypass);
    if(!ownWrites&&result==1){
        capabilities.observe(id,value,owner);
        if(owner==UINT64_MAX&&(id==NativeMotion::OriginalCapabilities::motion||
           id==NativeMotion::OriginalCapabilities::dualSense))++observedGameWrites;
    }
    return result;
}
static int ExecuteAction(void *action,void *pending){return actionRelay.execute(action,pending);}
static void *Output(const Key *key){
    void *result=originalResolve.load(std::memory_order_acquire)(key);
    void *device=PrivateOutputProbe::nativeDevice.load(std::memory_order_acquire);
    if(!capabilities.active()||!device||!key||!key->bus)return result;
    bool motion=NativeMotion::HasOriginalMotionRoot(key->bus,
        [](const void *bus){return ReadAt<uint32_t>(bus,0x10);},
        [](const void *bus){return ReadAt<const void *>(bus,0x90);});
    if(!motion){++preservedRoutes;return result;}
    ++redirects;return device;
}
static void CaptureOnRender(){
    if(!installed||capabilities.ready()||!managerCell)return;
    // The render callback is the RTPC manager's owning thread. Capture only
    // existing entries, before overriding either global value.
    const void *manager=*managerCell;if(!manager)return;
    uint32_t buckets=ReadAt<uint32_t>(manager,8);
    const void *const *table=ReadAt<const void *const *>(manager,0);
    if(!buckets||buckets>1048576||!table)return;
    for(uint32_t id:{NativeMotion::OriginalCapabilities::motion,NativeMotion::OriginalCapabilities::dualSense}){
        const void *entry=table[id%buckets];
        for(unsigned n=0;entry&&n<256;++n,entry=ReadAt<const void *>(entry,8)){
            if(ReadAt<uint32_t>(entry,0)!=id)continue;
            bool global=ReadAt<uint8_t>(entry,0x2c)!=0;
            // STMG's verified 7.2 defaults are 1 for Motion and 0 for DualSense.
            float value=global?ReadAt<float>(entry,0x28):(id==NativeMotion::OriginalCapabilities::motion?1.f:0.f);
            capabilities.capture(id,value);break;
        }
    }
}
static bool Install(){
    if(attempted)return installed;attempted=true;
    const uint32_t *setter=(const uint32_t *)RuntimeImageRange(0x1108df6bc,80,false);
    uintptr_t cell=setter?NativeMotion::RTPCManagerCell((uintptr_t)F<void *>(0x1108df6bc),setter[12],setter[17]):0;
    uintptr_t slide=_dyld_get_image_vmaddr_slide(0);
    if(!cell||cell<slide||!RuntimeImageRange(cell-slide,sizeof(void *),true)){
        W(@"original_motion installed=0 reason=rtpc_reference");return false;
    }
    managerCell=(void **)cell;
    const uint32_t parentPrefix[]={0xaa0903ea,0xf940494a,0xb400012a,0xb9417948};
    const void *parent=RuntimeImageRange(0x1109226b4,16,false);
    if(!parent||memcmp(parent,parentPrefix,16)){
        W(@"original_motion installed=0 reason=bus_parent_profile");return false;
    }
    const uint32_t setPrefix[]={0x6dbb23e9,0xa9015ff8,0xa90257f6,0xa9034ff4};
    const uint32_t resolvePrefix[]={0xa9be4ff4,0xa9017bfd,0x910043fd,0xf9400009};
    const uint32_t actionPrefix[]={0xd10783ff,0xa91b57f6,0xa91c4ff4,0xa91d7bfd};
    if(!NativeCodePatch::Install(F<void *>(0x1108d9684),(void *)ExecuteAction,actionPrefix,actionRelay.original)||
       !NativeCodePatch::Install(F<void *>(0x1108df6bc),(void *)Set,setPrefix,originalSet)||
       !NativeCodePatch::Install(F<void *>(0x1109224b8),(void *)Output,resolvePrefix,originalResolve)){
        W(@"original_motion installed=0 reason=hook_profile");return false;
    }
    installed=true;PrivateOutputProbe::observeRender=CaptureOnRender;
    W(@"original_motion installed=1 existing_ios_vorbis_aux=1 master_bus_route=1 bow_duplicate_action_guard=1 bow_guard_entries=2 primary_audio_route_unchanged=1 capture=existing_private_output");
    return true;
}
static void Request(bool enabled){capabilities.request(installed&&enabled);}
static void Sync(){
    if(!installed||!F<bool (*)()>(0x1108de04c)())return;
    auto result=capabilities.sync([](uint32_t id,float value){
        ++ownWrites;
        int status=originalSet.load(std::memory_order_acquire)(id,value,UINT64_MAX,0,4,false);
        --ownWrites;return status;
    });
    if(result.changed)W(@"original_motion gates active=%d motion_status=%d dualsense_status=%d game_requests_restored_on_disable=1",
        result.active,result.motionStatus,result.dualSenseStatus);
}
static void Prime(){
    if(installed&&capabilities.ready()&&!capabilities.active())Sync();
}
static void ObserveEvent(uint32_t event){
    for(unsigned i=0;i<6;++i)if(observedEvents[i]==event){++eventCounts[i];break;}
}
static void Status(){
    if(!installed)return;
    W(@"original_motion status snapshot_ready=%d active=%d native_bus_resolves=%llu preserved_output_resolves=%llu bow_actions_suppressed=%llu bow_primary_suppressed=%llu bow_alternate_suppressed=%llu game_gate_writes=%llu",
        capabilities.ready(),capabilities.active(),redirects.load(),preservedRoutes.load(),actionRelay.suppressed.load(),
        actionRelay.suppressedPrimary.load(),actionRelay.suppressedAlternate.load(),observedGameWrites.load());
    static uint64_t last[6]{};
    for(unsigned i=0;i<6;++i){
        uint64_t count=eventCounts[i].load();if(count==last[i])continue;last[i]=count;
        W(@"original_motion event=%u seen=%llu private_event_copy=0",observedEvents[i],count);
    }
}
}

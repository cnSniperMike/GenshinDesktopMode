#pragma once
#include "native_dispatch.h"
#include "native_immediate_dispatch.h"
#include "native_capability_gate.h"
#include "native_sdk_relay.h"
#include "native_engine_relay.h"
#include "native_signal.h"
#include "live_event_map.h"
#include "native_code_patch.h"
#include "native_original_motion.h"

namespace NativeLive {
using Dispatch=NativeMotion::ImmediateDispatcher<>;
static NativeMotion::SDKRelay relay;
static NativeMotion::EngineRelay engineRelay;
static NativeMotion::Signal signal;
static void *loader;static uint32_t package;
static unsigned phase=0;static CFAbsoluteTime began=0;
static constexpr uint32_t bank=0xfd4d0001;
static std::atomic<uint64_t> submitted{0},posted{0},failed{0},stopObserved{0},endCallbacks{0},pcmFrames{0},pcmRejected{0};
static std::atomic<bool> ready{false},allowed{false};
static NativeMotion::CapabilityGate capabilities;
static std::atomic<uint64_t> mappedSeen{0},mappedOriginalFailed{0},mappedWhileDisabled{0};
static constexpr size_t EventCount=sizeof(LiveEventMap)/sizeof(*LiveEventMap);
struct EventStat {
    std::atomic<uint64_t> seen{0},posted{0},failed{0},disabled{0},originalZero{0};
};
static EventStat eventStats[EventCount];
static size_t EventIndex(uint32_t event){
    size_t lo=0,hi=EventCount;
    while(lo<hi){size_t m=(lo+hi)/2;if(LiveEventMap[m].event<event)lo=m+1;else hi=m;}
    return lo<EventCount&&LiveEventMap[lo].event==event?lo:EventCount;
}
static void End(uint32_t type,void *info){
    if((type&1)&&info){auto *token=*(Dispatch::Token **)info;Dispatch::ended(token);++endCallbacks;}
}
static uint32_t Post(void *,const NativeMotion::Command &c,Dispatch::Token *token){
    NativeMotion::EngineRelay::PrivateScope privatePost;
    uint32_t id=F<uint32_t (*)(uint32_t,uint64_t,uint32_t,void *,void *,uint32_t,void *,uint32_t,uint32_t,uint64_t)>(0x1108dfba8)
        (c.privateEvent,c.owner,1,(void *)End,token,0,nullptr,0,c.customType,c.customValue);
    size_t index=EventIndex(c.event);
    if(id){++posted;if(index<EventCount)++eventStats[index].posted;}
    else {++failed;if(index<EventCount)++eventStats[index].failed;}
    return id;
}
static void Stop(void *,uint32_t id,int transition,int curve){
    relay.originalStop.load()(id,transition,curve); // Never relay our own stop again.
}
static Dispatch dispatcher(LiveEventMap,sizeof(LiveEventMap)/sizeof(*LiveEventMap),{nullptr,Post,Stop});
static void SetAllowed(bool value){
    allowed=value;bool next=value&&ready.load();capabilities.request(next);
    #if GDM_NATIVE_PRODUCT
    NativeOriginalMotion::Request(next);
    #endif
    // Revoke immediately on a disconnect, focus/mode change or user disable.
    // Enabling waits until the isolated capability commands were submitted.
    if(!next){signal.setEnabled(false);dispatcher.setEnabled(false);}
}
static void SyncCapabilities(){
    static uint64_t failedEpoch=UINT64_MAX;
    static double retryAt;
    const double now=CACurrentMediaTime();
    if(failedEpoch==capabilities.generation()&&now<retryAt)return;
    auto result=capabilities.sync([](uint32_t id,float value){
        return F<int (*)(uint32_t,float,uint64_t,int,int,bool)>(0x1108df6bc)(id,value,UINT64_MAX,0,4,true);
    });
    const bool failed=result.motionStatus!=1||result.dualSenseStatus!=1;
    if(failed){failedEpoch=result.epoch;retryAt=now+1;}
    else failedEpoch=UINT64_MAX;
    bool next=result.active&&ready.load();
    signal.setEnabled(next);dispatcher.setEnabled(next);
    if(capabilities.generation()!=result.epoch||!ready.load()){
        signal.setEnabled(false);dispatcher.setEnabled(false);
    }
    if(result.changed)W(@"native_live private_capability motion=%d dualsense=%d active=%d original_game_parameters_unchanged=1 physical_feedback_verified=0",result.motionStatus,result.dualSenseStatus,next);
}
static bool Read(PSAudioPair &pair){return signal.read(pair);}
static void Observe(uint32_t event,uint64_t owner,uint32_t playing,uint32_t type,uint64_t value){
    #if GDM_NATIVE_PRODUCT
    NativeOriginalMotion::ObserveEvent(event);
    #endif
    size_t lo=EventIndex(event);
    if(lo<EventCount){
        ++mappedSeen;if(!playing)++mappedOriginalFailed;if(!allowed.load())++mappedWhileDisabled;
        auto &s=eventStats[lo];++s.seen;if(!playing)++s.originalZero;if(!allowed.load())++s.disabled;
    }
    if(dispatcher.submit(event,owner,playing,type,value))++submitted;
}
static void ObserveStop(uint32_t id,int transition,int curve){
    if(dispatcher.stopOriginal(id,transition,curve))++stopObserved;
}
static uint32_t EnginePost(uint32_t e,uint64_t o,uint32_t flags,void *callback,void *cookie,void *params,uint32_t requested){
    #if GDM_NATIVE_PRODUCT
    if(!NativeMotion::EngineRelay::privateDepth)NativeOriginalMotion::Prime();
    #endif
    auto pending=dispatcher.begin(NativeMotion::EngineRelay::privateDepth?0:e);
    return engineRelay.post(e,o,flags,callback,cookie,params,requested);
}
static void SDKStop(uint32_t id,int time,int curve){relay.stop(id,time,curve);}
static void PCM(const float *p,unsigned maximum,unsigned valid,unsigned channels,unsigned rate,float first,float last){
    if(signal.push(p,maximum,valid,channels,rate,first,last))pcmFrames.fetch_add(valid);
    else ++pcmRejected;
}
static bool InstallRelay(){
    const uint32_t postPrefix[]={0xa9ba6ffc,0xa90167fa,0xa9025ff8,0xa90357f6};
    const uint32_t stopPrefix[]={0xa9bd57f6,0xa9014ff4,0xa9027bfd,0x910083fd};
    auto p=NativeCodePatch::Install(F<void *>(0x1108dfe00),(void *)EnginePost,postPrefix,engineRelay.original);
    if(!p)return false;
    auto s=NativeCodePatch::Install(F<void *>(0x1108e1254),(void *)SDKStop,stopPrefix,relay.originalStop);
    if(!s)return false;
    engineRelay.observe=Observe;relay.observeStop=ObserveStop;
    #if GDM_NATIVE_PRODUCT
    NativeOriginalMotion::actionRelay.replacementReady=[](){
        return ready.load()&&allowed.load()&&capabilities.active()&&NativeOriginalMotion::capabilities.active();
    };
    #endif
    W(@"native_live event_entry=shared_native_postevent sdk_and_direct_callers=1 private_posts_excluded=1 preserve_emitter_command_order=1 timer_starts_no_events=1");
    return true;
}
static void Status(){
    #if GDM_NATIVE_PRODUCT
    NativeOriginalMotion::Status();
    #endif
    W(@"native_live status phase=%u game_calls=%llu submitted=%llu native_posts=%llu failed=%llu stop_seen=%llu ended=%llu active=%zu drops=%llu pcm_frames=%llu pcm_rejected=%llu output_allowed=%d physical_feedback_verified=0 original_failed=%llu external_calls=%llu mapped_seen=%llu mapped_original_failed=%llu mapped_disabled=%llu",
      phase,engineRelay.calls.load(),submitted.load(),posted.load(),failed.load(),stopObserved.load(),endCallbacks.load(),
      dispatcher.active(),dispatcher.dropped(),pcmFrames.load(),pcmRejected.load(),allowed.load(),engineRelay.failed.load(),engineRelay.externalCalls.load(),mappedSeen.load(),mappedOriginalFailed.load(),mappedWhileDisabled.load());
    // Bounded diagnostics on the control timer. Native event and audio threads
    // only increment counters; they never allocate or write per-event logs.
    static uint64_t last[EventCount][5]{};static size_t cursor=0;unsigned lines=0;
    for(size_t n=0;n<EventCount&&lines<8;++n){
        size_t i=cursor;cursor=(cursor+1)%EventCount;auto &s=eventStats[i];
        uint64_t values[]={s.seen.load(),s.posted.load(),s.failed.load(),s.disabled.load(),s.originalZero.load()};
        if(!memcmp(last[i],values,sizeof(values)))continue;
        memcpy(last[i],values,sizeof(values));++lines;
        W(@"native_live event=%u seen=%llu private_posts=%llu post_failures=%llu disabled=%llu original_zero=%llu",LiveEventMap[i].event,values[0],values[1],values[2],values[3],values[4]);
    }
}
static bool Begin(){
    if(!F<bool (*)()>(0x1108de04c)())return false;
    #if GDM_NATIVE_PRODUCT
    if(!NativeMotionSource::Register())return false;
    #endif
    auto engine=F<void *(*)()>(0x110858298)();loader=engine?F<void *(*)(void *)>(0x110858328)(engine):nullptr;
    if(!loader)return false;
    #if GDM_NATIVE_PRODUCT
    const char *filename=NativePackFilename();
    #else
    const char *filename="GDMNativeMotionResearch.pck";
    #endif
    int status=F<int (*)(void *,const char *,uint32_t *,int)>(0x110853fc0)(loader,filename,&package,-1);
    W(@"native_live package_status=%d",status);
    if(status!=1&&status!=22){phase=9;return true;}
    status=F<int (*)(void *,uint32_t)>(0x1108545a4)(loader,bank);
    W(@"native_live bank_status=%d",status);
    if(status!=1){F<int (*)(void *,uint32_t)>(0x110854330)(loader,package);phase=9;return true;}
    if(!PrivateOutputProbe::Register()){
        F<int (*)(void *,uint32_t)>(0x1108545c8)(loader,bank);F<int (*)(void *,uint32_t)>(0x110854330)(loader,package);phase=9;return true;
    }
    phase=1;began=CFAbsoluteTimeGetCurrent();return true;
}
static void Tick(){
    CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();
    if(phase==1&&PrivateOutputProbe::phase.load()==1){
        if(PrivateOutputProbe::result!=1||!InstallRelay()){phase=8;W(@"native_live relay_or_output_failed=1");return;}
        if(!SinkObserver::captureSink.load()){phase=8;W(@"native_live private_sink_missing=1");return;}
        SinkObserver::privatePCM=PCM;ready=true;
        #if GDM_NATIVE_PRODUCT
        SetAllowed(allowed.load());
        W(@"native_live route_ready=1 requires_physical_dualsense_audio_backend=1 source_generator=1 original_audio_untouched=1");
        #else
        SetAllowed(true);
        W(@"native_live mirror_enabled=1 capture_only=1 original_audio_untouched=1 hardware_opened=0");
        #endif
        phase=2;began=now;
    }
    if(phase==1&&now-began>5){phase=8;W(@"native_live private_output_timed_out=1");}
    if(phase==8){
        ready=false;SetAllowed(false);dispatcher.drain();
        if(!dispatcher.active()){PrivateOutputProbe::request=2;phase=4;}
    }
    if(phase==2){
        SyncCapabilities();
        dispatcher.drain();
        #if !GDM_NATIVE_PRODUCT
        // Silent software consumer; never open a hardware device in this probe.
        float silent[8192*4];signal.render(silent,8192);
        if(now-began>480){dispatcher.setEnabled(false);signal.setEnabled(false);phase=3;began=now;}
        #endif
    }
    if(phase==3){
        dispatcher.drain();
        if(!dispatcher.active()){
            SinkObserver::privatePCM=nullptr;PrivateOutputProbe::request=2;phase=4;
        }else if(now-began>5){W(@"native_live cleanup_retained_active_callbacks=1");phase=8;}
    }
    if(phase==4&&PrivateOutputProbe::phase.load()==2){
        PrivateOutputProbe::Unregister();SinkObserver::SaveCapture();
        int b=F<int (*)(void *,uint32_t)>(0x1108545c8)(loader,bank),p=F<int (*)(void *,uint32_t)>(0x110854330)(loader,package);
        W(@"native_live cleanup bank=%d package=%d",b,p);phase=5;Status();
    }
    #if GDM_NATIVE_PRODUCT
    NativeOriginalMotion::Sync();
    #endif
}
}

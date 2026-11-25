#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace NativeMotion {
// These are the game's original bus gates, separate from the two private-bank
// parameters. Remember actual game requests so relinquishing the output does
// not leave a forced device capability behind.
class OriginalCapabilities {
    static constexpr uint64_t known=uint64_t(1)<<32;
    std::atomic<uint64_t> motionValue{0x3f800000},dualSenseValue{0};
    std::atomic<uint64_t> wanted{0},applied{UINT64_MAX};
    std::atomic<bool> everRequested{false};
    static uint32_t bits(float value){uint32_t result;memcpy(&result,&value,4);return result;}
    static float value(uint64_t bits){uint32_t b=uint32_t(bits);float result;memcpy(&result,&b,4);return result;}
    std::atomic<uint64_t> *slot(uint32_t id){
        return id==motion?&motionValue:id==dualSense?&dualSenseValue:nullptr;
    }
public:
    static constexpr uint32_t motion=0x09f8cc94,dualSense=0xb108057d,rootBus=0x78eb87f0;
    struct Result {bool changed,active;int motionStatus,dualSenseStatus;};
    bool ready()const{return (motionValue.load(std::memory_order_acquire)&known)&&
        (dualSenseValue.load(std::memory_order_acquire)&known);}
    void assumeDefaults(){
        uint64_t m=motionValue.load(std::memory_order_acquire),d=dualSenseValue.load(std::memory_order_acquire);
        if(!(m&known))motionValue.store(known|bits(1.f),std::memory_order_release);
        if(!(d&known))dualSenseValue.store(known|bits(0.f),std::memory_order_release);
    }
    void capture(uint32_t id,float current){
        auto s=slot(id);if(!s||!std::isfinite(current))return;
        uint64_t old=s->load(std::memory_order_acquire);
        // A concurrent SDK request is newer than this render-thread snapshot.
        if(!(old&known))s->compare_exchange_strong(old,known|bits(current),std::memory_order_acq_rel);
    }
    void observe(uint32_t id,float requested,uint64_t owner){
        auto s=slot(id);if(s&&owner==UINT64_MAX&&std::isfinite(requested))
            s->store(known|bits(requested),std::memory_order_release);
    }
    void request(bool enabled){
        if(enabled)everRequested=true;
        uint64_t old=wanted.load(std::memory_order_acquire);
        while(bool(old&1)!=enabled&&!wanted.compare_exchange_weak(old,((old>>1)+1)*2+enabled,std::memory_order_acq_rel)){}
    }
    bool active()const{
        uint64_t next=wanted.load(std::memory_order_acquire);
        return (next&1)&&ready()&&applied.load(std::memory_order_acquire)==next&&
            wanted.load(std::memory_order_acquire)==next;
    }
    float effective(uint32_t id,float requested,uint64_t owner)const{
        if(owner==UINT64_MAX&&(id==motion||id==dualSense)&&std::isfinite(requested)&&
           ready()&&(wanted.load(std::memory_order_acquire)&1))return 1;
        return requested;
    }
    template<class Writer>Result sync(Writer write){
        uint64_t next=wanted.load(std::memory_order_acquire);
        assumeDefaults();
        if(!ready())return {false,false,0,0};
        if(!(next&1)&&!everRequested.load())return {false,false,1,1};
        if(applied.load(std::memory_order_acquire)==next)return {false,bool(next&1),1,1};
        uint64_t m=motionValue.load(std::memory_order_acquire),d=dualSenseValue.load(std::memory_order_acquire);
        int a=write(motion,(next&1)?1.f:value(m)),b=write(dualSense,(next&1)?1.f:value(d));
        bool stable=wanted.load(std::memory_order_acquire)==next&&
            ((next&1)||(motionValue.load(std::memory_order_acquire)==m&&dualSenseValue.load(std::memory_order_acquire)==d));
        if(a==1&&b==1&&stable){
            applied.store(next,std::memory_order_release);
        }
        return {true,a==1&&b==1&&stable&&bool(next&1),a,b};
    }
};

// The renderer resolves the master bus after combining auxiliary sends.
// Filtering this lookup by child IDs discards those sends as well.
template<class Bus,class ReadID,class ReadParent>
bool HasOriginalMotionRoot(const Bus *bus,ReadID id,ReadParent parent){
    for(unsigned depth=0;bus&&depth<64;++depth){
        if(id(bus)==OriginalCapabilities::rootBus)return true;
        const Bus *next=parent(bus);if(next==bus)return false;bus=next;
    }
    return false;
}

// Suppress the two redundant iOS bow-charge actions before they enter the
// shared mixer. Private events 0xed000d2a and 0xed000bad both play 0xed00086d;
// their native counterparts both play 0x310b53b2 with identical action data.
// Match the action ID, target and type together so ordinary sound, auxiliary
// sends, stop actions and remapped private actions retain their native path.
struct OriginalMotionActionRelay {
    using Execute=int (*)(void *,void *);
    using Ready=bool (*)();
    std::atomic<Execute> original{nullptr};
    std::atomic<Ready> replacementReady{nullptr};
    std::atomic<uint64_t> suppressed{0};
    std::atomic<uint64_t> suppressedPrimary{0},suppressedAlternate{0};
    int execute(void *action,void *pending){
        if(action){
            uint32_t id,target;uint16_t type;
            memcpy(&id,(const char *)action+0x10,4);
            memcpy(&target,(const char *)action+0x30,4);
            memcpy(&type,(const char *)action+0x34,2);
            auto ready=replacementReady.load(std::memory_order_acquire);
            if((id==0x2ce9920d||id==0x2948cffc)&&target==0x310b53b2&&type==0x0403&&ready&&ready()){
                ++suppressed;
                if(id==0x2ce9920d)++suppressedPrimary;else ++suppressedAlternate;
                // AK_Success, also used by the native action's chance-to-play
                // rejection path. No voice was started, so no stop is needed.
                return 1;
            }
        }
        return original.load(std::memory_order_acquire)(action,pending);
    }
};

// SetRTPCValue's checked ARM64 body loads the audio queue through ADRP x24 at
// +0x30 and LDR x0,[x24,#imm] at +0x44. The SDK's RTPC-manager cell is +0x38
// within that same global block in both supported clients.
inline uintptr_t RTPCManagerCell(uintptr_t setter,uint32_t adrp,uint32_t ldr){
    if((adrp&0x9f00001f)!=0x90000018||(ldr&0xffc003ff)!=0xf9400300)return 0;
    int64_t imm=((adrp>>5&0x7ffff)<<2)|(adrp>>29&3);
    if(imm&(1<<20))imm-=1<<21;
    return ((setter+0x30)&~uintptr_t(4095))+imm*4096+((ldr>>10)&4095)*8+0x38;
}
}

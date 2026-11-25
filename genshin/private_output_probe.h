namespace PrivateOutputProbe {
struct OutputSettings {uint32_t shareset,device,panning,channels;};
struct EmptySet {alignas(16) uint8_t storage[64]{};};
static std::atomic<unsigned> request{0},phase{0};
static std::atomic<int> result{0};
static std::atomic<void *> nativeDevice{nullptr};
static void (*observeRender)();
static uint64_t outputId,silentOutputId;
static bool motionCreated,silentCreated;
static constexpr uint32_t shareset=0xfd4d0061,silentShareset=0xfd4d0062;
static void Remove(){
    nativeDevice=nullptr;
    if(silentCreated){F<void (*)(uint64_t,bool)>(0x1109220e4)(silentOutputId,false);silentCreated=false;}
    if(motionCreated){F<void (*)(uint64_t,bool)>(0x1109220e4)(outputId,false);motionCreated=false;}
}
static void Callback(void *,unsigned location,void *){
    if(location!=4)return;
    if(observeRender)observeRender();
    unsigned command=request.exchange(0);
    if(command==1){
        outputId=F<uint64_t (*)(uint32_t,uint32_t)>(0x1108e12bc)(shareset,0);
        silentOutputId=F<uint64_t (*)(uint32_t,uint32_t)>(0x1108e12bc)(silentShareset,0);
        if(F<void *(*)(uint64_t)>(0x110922358)(outputId)||F<void *(*)(uint64_t)>(0x110922358)(silentOutputId)){
            result=-1;phase=1;return;
        }
        OutputSettings settings{shareset,0,0,0x3102};EmptySet listeners;
        SinkObserver::selectPrivate=true;
        int motion=F<int (*)(OutputSettings *,void *)>(0x1109222ac)(&settings,&listeners);
        SinkObserver::selectPrivate=false;motionCreated=motion==1;
        if(motionCreated)nativeDevice=F<void *(*)(uint64_t)>(0x110922358)(outputId);
        int silent=-1;
        if(motionCreated){
            // Ordinary sibling PCM is consumed by an independent native Dummy
            // sink. It is never selected by the actuator signal observer.
            settings.shareset=silentShareset;
            silent=F<int (*)(OutputSettings *,void *)>(0x1109222ac)(&settings,&listeners);
            silentCreated=silent==1;
        }
        result=motionCreated&&silentCreated?1:-1;
        if(result.load()!=1)Remove();
        phase=1;
    }else if(command==2){Remove();phase=2;}
}
static bool Register(){
    int status=F<int (*)(void *,unsigned,void *,int,unsigned,unsigned)>(0x1108e0eb0)((void *)Callback,4,nullptr,0,0,0);
    W(@"private_output_probe callback_status=%d physical_output=0 primary_output_changed=0 isolated_sibling_dummy=1",status);
    if(status!=1)return false;request=1;return true;
}
static void Unregister(){
    int status=F<int (*)(void *,unsigned)>(0x1108e10b8)((void *)Callback,4);
    W(@"private_output_probe unregister_status=%d",status);
}
}

#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mutex>
#include <atomic>
#include <array>
#include "keyboard_state.h"
#include "primary_button.h"
#include "hardware_state.h"
#include "scroll_state.h"
#include "screen_metrics.h"
#include "wheel_zoom_policy.h"

namespace NativeBridge {
static std::atomic<bool> nativeDesktopMouseUI{false};
static std::atomic<bool> controllerInputActive{false};
static std::atomic<bool> nativeNoticeEscapeActive{false};
static std::atomic<bool> nativeOverlayActive{false};
static std::atomic<bool> accountSheetActive{false};
static std::atomic<bool> mapPageVisible{false};
static std::atomic<bool> mapDragActive{false};
static std::atomic<bool> nativePointerGestures{false};
static int (*mapContextReader)(void);
static std::atomic<bool> paimonPageVisible{false};
static bool (*paimonScrollContext)(void);
static std::atomic<bool> settingsPageVisible{false};
static bool (*settingsScrollContext)(void);
static std::atomic<bool> friendPageVisible{false};
static bool (*friendScrollContext)(void);
static std::atomic<uint64_t> mapFocusRevision{0};
static std::atomic<uint32_t> physicalMouseSnapshot{0};
// A device-menu click belongs to the old mode until its real button-up.
static std::atomic<uint32_t> handoffMouseBlocked{0};
static void (*stopControllerFeedback)(void);
// Read-only device activity is sampled after the native update has returned;
// mode rebuilding is queued outside this engine call.
// Sample the latest OS position once before Unity consumes this frame's input.
// Observe the original AppKit dispatch before local monitors may consume Esc.
// The observer queues mode changes; it never consumes or modifies the event.
static void (*observeDesktopEvent)(id event);
// Only stable keyboard ownership may bypass per-packet device discovery.
// A pending handoff or saved controller preference keeps the original route.
static bool (*canBatchMapMotion)(void);
static void (*observeBatchedMapMotion)(double x,double y);
// Optional native map provider. Its ownership query never sends input.
static bool (*mapTouchOwnership)(bool refresh);
static std::mutex mutex;
static KeyboardState keyboard;
static auto &heldKeys=keyboard.held;
static std::array<bool,7> mouseButtons;
static DesktopPrimaryButton primaryButton;
static bool primaryUsesEvents,mouseReleasePending;
static float deltaX,deltaY;
static DesktopWheelRouter scroll;
static std::atomic<bool> systemScrollActive{false};
static uint32_t (*scrollContextReader)(void);
static uint32_t (*scrollZoomReader)(void);
// Read only the live interaction list, on pending world-wheel input. The
// callback is outside the input mutex and never selects or activates a row.
static bool (*interactionWheelContext)(void);
static bool packetUsesInteractionWheel=false; // protected by mutex
static float normalizedX,normalizedY;
static bool hasPointerPosition;
static bool IsKeyHeld(int key) {
    std::lock_guard<std::mutex> guard(mutex);
    return key>0 && key<323 && heldKeys[key];
}
static int (*screenWidth)(void *),(*screenHeight)(void *);
static void *screenWidthMethod,*screenHeightMethod;
static DesktopScreenMetrics screenMetrics;
static void RefreshScreenMetrics() {
    if (screenWidth && screenHeight)
        screenMetrics.publish(screenWidth(screenWidthMethod),screenHeight(screenHeightMethod));
}
static std::atomic<uint64_t> frames{0}, forwardedKeys{0}, forwardedMouseFrames{0};
static std::atomic<uint64_t> gameActionFrames{0};
static std::atomic<uint64_t> repairedKeyStates{0};
static void (*originalUpdate)(char *);
static bool enabled;

static void QueueKey(int key,bool pressed) {
    if (key<=0 || key>=350) return;
    std::lock_guard<std::mutex> guard(mutex);
    if(key==27 && pressed && nativeNoticeEscapeActive) {keyboard.blockUntilRelease(key);return;}
    keyboard.event(key,pressed);
}
static void SystemKey(unsigned short macKey,bool pressed) {
    if(!IsModernRuntime() || !IsForeground()) return;
    for(int hid=4;hid<=231;++hid) {
        if(MacKeyForHID(hid)!=(int)macKey) continue;
        int key=UnityKeyForHID(hid);if(!key) return;
        keyboardSources.event(key,pressed);
        QueueKey(key,pressed);
        return;
    }
}
static void SuppressNoticeEscape(void) {
    std::lock_guard<std::mutex> guard(mutex);keyboard.blockUntilRelease(27);
}
static void CancelMouseLocked(void) {
    primaryButton.cancel();
    mouseButtons.fill(false);
    mouseReleasePending=true;
    physicalMouseSnapshot=0;
    ++mapFocusRevision;
    mapDragActive=false;
}
static void CancelDesktopPointer(void) {
    std::lock_guard<std::mutex> guard(mutex);
    CancelMouseLocked();
}
static void SetAccountSheetActive(bool active) {
    std::lock_guard<std::mutex> guard(mutex);
    accountSheetActive=active;
    if (active) keyboard.focus(false);
    CancelMouseLocked();
    deltaX=deltaY=0;
    scroll.cancel();
    // A held SDK press must finish in the SDK even when it dismisses the sheet.
    uint32_t held=(uint32_t)((NSUInteger (*)(id,SEL))objc_msgSend)(
        NSClassFromString(@"NSEvent"),sel_registerName("pressedMouseButtons"));
    handoffMouseBlocked.fetch_or(held);
}
static void SetDesktopMouseUI(bool active) {
    if(nativeDesktopMouseUI.load()==active) return;
    std::lock_guard<std::mutex> guard(mutex);
    if(nativeDesktopMouseUI.exchange(active)!=active) CancelMouseLocked();
}
static void ReleaseKeys(void) {
    std::lock_guard<std::mutex> guard(mutex);
    keyboard.release();
    CancelMouseLocked();deltaX=deltaY=0;scroll.cancel();
}
static void Deactivate(void) {
    if(stopControllerFeedback) stopControllerFeedback();
    keyboardSources.reset();
    paimonPageVisible=false;settingsPageVisible=false;friendPageVisible=false;
    std::lock_guard<std::mutex> guard(mutex);
    keyboard.focus(false);
    CancelMouseLocked();deltaX=deltaY=0;scroll.cancel();
}
static void Move(float x,float y) {
    std::lock_guard<std::mutex> guard(mutex);if(keyboard.active){deltaX+=x;deltaY+=y;}
}
static bool PacketUsesInteractionWheel(uint32_t target) {
    return target==1 && interactionWheelContext && interactionWheelContext();
}
static void Scroll(float x,float y) {
    if(!x && !y) return;
    if(!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(),^{Scroll(x,y);});
        return;
    }
    bool discrete=PacketUsesInteractionWheel(scrollContextReader?scrollContextReader():1);
    std::lock_guard<std::mutex> guard(mutex);
    if(keyboard.active && !nativeOverlayActive) {
        double now=CACurrentMediaTime();
        packetUsesInteractionWheel=discrete;
        scroll.hardware(x,y,now);
    }
}
static void SystemScroll(const DesktopScrollDispatchScope::Key &event,uint32_t target,bool mosMarked,double linesX=0,double linesY=0) {
    bool discrete=PacketUsesInteractionWheel(target);
    std::lock_guard<std::mutex> guard(mutex);
    if(keyboard.active && !nativeOverlayActive) {
        packetUsesInteractionWheel=discrete;
        scroll.system(event.stamp,CACurrentMediaTime(),event.x,event.y,event.precise,event.phase,event.momentum,target,mosMarked,linesX,linesY);
    }
}
static void CancelCoarseWheel(void) {
    std::lock_guard<std::mutex> guard(mutex);scroll.cancelCoarse();
}
static void Position(float x,float y) {
    std::lock_guard<std::mutex> guard(mutex);
    normalizedX=x;normalizedY=y;hasPointerPosition=true;
}
static void CenterPointer(void) {
    std::lock_guard<std::mutex> guard(mutex);
    normalizedX=normalizedY=0.5f;hasPointerPosition=true;
    deltaX=deltaY=0;
}
static void MouseButton(unsigned button,bool pressed) {
    if (button>=7) return;
    uint32_t mask=1u<<button;
    std::lock_guard<std::mutex> guard(mutex);
    if(button==0 && primaryUsesEvents) return;
    if(!pressed) handoffMouseBlocked.fetch_and(~mask);
    mouseButtons[button]=keyboard.active && !controllerInputActive && pressed && !(handoffMouseBlocked.load()&mask);
}
static void EnablePrimaryEvents(void) {
    std::lock_guard<std::mutex> guard(mutex);
    if(!primaryUsesEvents) {CancelMouseLocked();primaryUsesEvents=true;}
}
static void PrimaryEvent(bool down,bool eligible,DesktopPrimaryButton::Point point) {
    std::lock_guard<std::mutex> guard(mutex);
    if(!down) handoffMouseBlocked.fetch_and(~1u);
    if(!primaryUsesEvents || controllerInputActive || nativeOverlayActive || !keyboard.active ||
       (down && (handoffMouseBlocked.load() & 1u))) return;
    // A real new AppKit down is authoritative even if a trackpad never
    // exposed its previous tap in the physical button mask.
    primaryButton.event(down,eligible,point);
}
static void SetControllerOwnership(bool controller) {
    if(controllerInputActive.load()==controller) return;
    std::lock_guard<std::mutex> guard(mutex);
    if(controllerInputActive.load()==controller) return;
    controllerInputActive=controller;
    if(controller) keyboard.release(); else keyboard.handoffToKeyboard();
    handoffMouseBlocked=(uint32_t)((NSUInteger (*)(id,SEL))objc_msgSend)(
        NSClassFromString(@"NSEvent"),sel_registerName("pressedMouseButtons"));
    CancelMouseLocked();deltaX=deltaY=0;scroll.cancel();
}
static void Update(char *manager) {
    bool mouseOwned;
    auto setter=EngineFunction<void (*)(void *,int,bool)>(0x10eec31b8);
    bool foreground=IsForeground() && !accountSheetActive.load();
    uint32_t wheelContext=foreground && !nativeOverlayActive?
        (scrollContextReader?scrollContextReader():1):0;
    uint32_t zoomContext=wheelContext && scrollZoomReader?scrollZoomReader():0;
    bool interactionWheelDiscrete;
    auto physical=KeyboardSnapshot(foreground);
    NSUInteger rawMouse=((NSUInteger (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSEvent"),sel_registerName("pressedMouseButtons"));
    std::vector<std::pair<int,bool>> events;
    std::array<bool,7> buttons;
    float x,y,sx,sy,px,py,zoomGain;bool positioned,mosScroll;unsigned movement=0;
    uint64_t pointerRevision;
    {
        std::lock_guard<std::mutex> guard(mutex);
        foreground=foreground && !accountSheetActive.load() && !macWindowSuspended.load();
        if(keyboard.active!=foreground) {
            keyboard.focus(foreground);
            CancelMouseLocked();deltaX=deltaY=0;scroll.cancel();
            Log(@"input_focus active=%d released_all=1",foreground);
        }
        if(nativeNoticeEscapeActive && physical[27]) keyboard.blockUntilRelease(27);
        repairedKeyStates+=keyboard.reconcile(physical);
        mouseOwned=!controllerInputActive;
        pointerRevision=mapFocusRevision.load();
        uint32_t blocked=handoffMouseBlocked.fetch_and((uint32_t)rawMouse);
        NSUInteger physicalMouse=foreground?(rawMouse&~(NSUInteger)blocked):0;
        if(nativeDesktopMouseUI.load()) {
            for(unsigned b=primaryUsesEvents?1:0;b<7;++b)
                mouseButtons[b]=!nativeOverlayActive && (physicalMouse&(1UL<<b))!=0;
        }
        events.swap(keyboard.pending);
        movement=(heldKeys[97]?1:0)|(heldKeys[100]?2:0)|(heldKeys[115]?4:0)|(heldKeys[119]?8:0);
        x=deltaX;y=deltaY;
        // A map/list/world transition cancels an ordinary wheel's short tail
        // before the newly opened view can consume it. Mos keeps its route.
        interactionWheelDiscrete=wheelContext==1 && packetUsesInteractionWheel;
        auto wheel=scroll.frame(CACurrentMediaTime(),wheelContext,wheelContext|((uint64_t)zoomContext<<32),interactionWheelDiscrete);
        mosScroll=scroll.usingMos();
        sx=(float)wheel.x;sy=(float)wheel.y;
        px=normalizedX;py=normalizedY;positioned=hasPointerPosition;
        if(primaryUsesEvents && mouseOwned && foreground && !nativeOverlayActive) {
            auto frame=primaryButton.frame((rawMouse&1u)!=0);
            mouseButtons[0]=frame.down;
            if(frame.position.valid) {px=frame.position.x;py=frame.position.y;positioned=true;}
        }
        buttons=mouseButtons;
        if(mouseReleasePending || !mouseOwned || !foreground || nativeOverlayActive) buttons.fill(false);
        uint32_t mask=0;
        for(unsigned b=0;b<buttons.size();++b) if(buttons[b]) mask|=1u<<b;
        physicalMouseSnapshot=mask;
        zoomGain=interactionWheelDiscrete?1.0f:DesktopWheelZoomPolicy::gain(mosScroll,zoomContext);
        sx*=zoomGain;sy*=zoomGain;
        if(!mouseOwned || mouseReleasePending) x=y=sx=sy=0;
        // The input frame is the only writer. A boundary between frames
        // guarantees a neutral native update even if the new owner is a pad.
        if(mouseOwned || mouseReleasePending) {
            for(unsigned b=0;b<buttons.size();++b) setter(manager,323+b,buttons[b]);
            *(float *)(manager+0xc0)=x;*(float *)(manager+0xc4)=y;
            *(float *)(manager+0xc8)=sx;*(float *)(manager+0xcc)=sy;
        }
        mouseReleasePending=false;
        deltaX=deltaY=0;
    }
    for (auto event : events) {
        setter(manager,event.first,event.second);
        auto count=++forwardedKeys;
        if (count<=8) Log(@"keyboard_frame_forwarded pressed=%d engine_readback=%d",
            event.second,EngineFunction<bool (*)(int)>(0x10e864028)(event.first));
    }
    originalUpdate(manager);
    if(IsModernRuntime()) {
        unsigned engine=0,held=0;unsigned mask=1;
        auto keys=*(const uint32_t **)(manager+0x68);
        for(int key:{97,100,115,119}) {
            if(EngineFunction<bool (*)(int)>(0x10e864028)(key)) engine|=mask;
            if(keys && (keys[key/32]&(1u<<(key%32)))) held|=mask;
            mask<<=1;
        }
        static unsigned previous=~0u;unsigned state=movement|(held<<4)|(engine<<8);
        if(state!=previous) {
            previous=state;
            Log(@"keyboard_movement_snapshot desired=%u native_held=%u engine=%u gc=%u hardware=%u",movement,held,engine,
                gcMovementSnapshot.load(),hardwareMovementSnapshot.load());
        }
    }
    static int previousWheelSource=-1;
    int wheelSource=interactionWheelDiscrete?2:mosScroll?1:0;
    if(previousWheelSource!=wheelSource) {
        previousWheelSource=wheelSource;
        Log(@"scroll_source mode=%s",interactionWheelDiscrete?"interaction_packet_direct":mosScroll?"mos_precise":"hardware_discrete_smooth precise_passthrough=1");
    }
    if(sx || sy) {
        static uint64_t wheelFrames[3]={};
        auto count=++wheelFrames[wheelSource];
        if(count<=6 || (diagnosticsEnabled && count%240==0))
            Log(@"scroll_frame source=%s context=%u zoom=%d gain=%.2f delta=(%.5f,%.5f)",interactionWheelDiscrete?"interaction_direct":mosScroll?"mos":"r33_hardware",wheelContext,zoomContext,zoomGain,sx,sy);
    }
    if(foreground && buttons[0] && (x || y)) {
        static uint64_t heldMotionFrames=0;
        if(++heldMotionFrames<=4 || heldMotionFrames%240==0)
            Log(@"held_button_motion frame=%llu delta=(%.3f,%.3f)",heldMotionFrames,x,y);
    }
    if(nativeDesktopMouseUI.load()) {
        static bool previousLeft=false;
        if(buttons[0]!=previousLeft) {
            Log(@"native_pc_button physical=%d key=%d mouse=%d down=%d up=%d",
                buttons[0],EngineFunction<bool (*)(int)>(0x10e864028)(323),
                EngineFunction<bool (*)(int)>(0x10e8640f0)(0),
                EngineFunction<bool (*)(int)>(0x10e8640f4)(0),EngineFunction<bool (*)(int)>(0x10e8640f8)(0));
            previousLeft=buttons[0];
        }
    }
    const auto pixels=screenMetrics.read();
    if (mouseOwned && pointerRevision==mapFocusRevision.load() && foreground && positioned && pixels.width && pixels.height) {
        // One position drives both the visible cursor and Unity hit testing.
        // Camera motion uses the axis accumulators, never an unbounded second
        // absolute cursor that could reappear at a different location.
        *(float *)(manager+0xd0)=px*pixels.width;
        *(float *)(manager+0xd4)=py*pixels.height;
    }
    ++frames;
    if (x || y || sx || sy) {
        auto frame=++forwardedMouseFrames;
        if (frame<=3 || frame%240==0) {
            char *axes=*(char **)(manager+0x40);uint64_t count=*(uint64_t *)(manager+0x50);
            unsigned mouseAxes=0,nonzero=0;
            if(count<=2000) for(uint64_t i=0;i<count;++i) {
                char *axis=axes+i*0xb8;
                if(*(int *)(axis+0x94)!=1) continue;
                ++mouseAxes;if(*(float *)(axis+0x9c)!=0) ++nonzero;
            }
            Log(@"mouse_native_frame delta=(%.2f,%.2f) configured_axes=%u nonzero_axes=%u",x,y,mouseAxes,nonzero);
        }
    }
}
static bool MousePresent(void) { return true; }
static void (*originalGameTick)(void *,void *);
static void *gameInput;
static void GameTick(void *object,void *method) {
    if (gameInput!=object) {
        gameInput=object;
        Log(@"game_input_tick instance=%p rewired_player=%p rewired_manager=%p",object,
            *(void **)((char *)object+GenshinOffsets().inputPlayer),*(void **)((char *)object+GenshinOffsets().inputManager));
    }
    originalGameTick(object,method);
    if(!diagnosticsEnabled) return;
    unsigned held=0,engineHeld=0,rewiredHeld=0;
    {
        std::lock_guard<std::mutex> guard(mutex);
        for (int i=1;i<323;++i) {
            held+=heldKeys[i];
            engineHeld+=EngineFunction<bool (*)(int)>(0x10e864028)(i);
            if (rewiredKeyReader && rewiredKeyboard) rewiredHeld+=rewiredKeyReader(rewiredKeyboard,i,rewiredKeyMethod);
        }
    }
    static uint64_t heldFrames;
    static unsigned previousHeld=0;
    if (held!=previousHeld || (held && (++heldFrames<=3 || heldFrames%120==0)))
        Log(@"input_layer_readback held=%u engine=%u rewired=%u",held,engineHeld,rewiredHeld);
    previousHeld=held;
    void *player=*(void **)((char *)object+GenshinOffsets().inputPlayer);
    if (gameAxisReader && player) {
        float mx=gameAxisReader(player,0,gameAxisMethod),my=gameAxisReader(player,1,gameAxisMethod);
        float vx=gameAxisReader(player,30,gameAxisMethod),vy=gameAxisReader(player,31,gameAxisMethod);
        static uint64_t viewFrames=0;
        if ((vx || vy) && (++viewFrames<=8 || viewFrames%120==0))
            Log(@"game_view_readback view=(%.3f,%.3f)",vx,vy);
        if (mx || my || vx || vy) {
            auto count=++gameActionFrames;
            if (count<=12 || count%90==0) Log(@"game_action_readback move=(%.3f,%.3f) view=(%.3f,%.3f)",mx,my,vx,vy);
        }
        static bool previouslyMoving=false;
        bool moving=mx || my;
        if (previouslyMoving && !moving) Log(@"game_movement_stopped move=(0,0)");
        previouslyMoving=moving;
    }
}

static void AbsoluteJump(void *destination,uintptr_t target) {
    uint32_t code[2]={0x58000050,0xd61f0200}; // ldr x16, #8; br x16
    memcpy(destination,code,8);memcpy((char *)destination+8,&target,8);
}
static bool PatchCode(void *target,const void *code,size_t length) {
    vm_size_t pageSize=vm_page_size;
    mach_vm_address_t page=(uintptr_t)target&~((uintptr_t)pageSize-1);
    mach_vm_size_t size=(((uintptr_t)target+length-page+pageSize-1)/pageSize)*pageSize;
    auto result=mach_vm_protect(mach_task_self(),page,size,false,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if (result!=KERN_SUCCESS) {Log(@"native_bridge patch_write_failed=%d",result);return false;}
    memcpy(target,code,length);sys_icache_invalidate(target,length);
    result=mach_vm_protect(mach_task_self(),page,size,false,VM_PROT_READ|VM_PROT_EXECUTE);
    Log(@"native_bridge patch_execute_result=%d",result);
    return result==KERN_SUCCESS;
}
static bool Install(void) {
    if (!IsCompatibleRuntime() || !nativeInputManager) return false;
    void *target=EngineFunction<void *>(0x10eec3098);
    const uint32_t expected[4]={0xa9bc5ff8,0xa90157f6,0xa9024ff4,0xa9037bfd};
    if (!RuntimePrologueMatches(0x10eec3098,target)) { Log(@"native_bridge refused_unexpected_update_code");return false; }
    void *trampoline=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if (trampoline==MAP_FAILED) {Log(@"native_bridge trampoline_allocate_failed");return false;}
    memcpy(trampoline,target,16);AbsoluteJump((char *)trampoline+16,(uintptr_t)target+16);
    sys_icache_invalidate(trampoline,32);
    if (mprotect(trampoline,4096,PROT_READ|PROT_EXEC)) {Log(@"native_bridge trampoline_execute_failed");return false;}
    originalUpdate=(void (*)(char *))trampoline;
    uint8_t jump[16];AbsoluteJump(jump,(uintptr_t)Update);
    if (!PatchCode(target,jump,16)) return false;
    AbsoluteJump(jump,(uintptr_t)MousePresent);
    bool mousePatched=PatchCode(EngineFunction<void *>(0x10f354bd4),jump,16);
    nativeKeyConsumer=QueueKey;enabled=true;
    Log(@"native_bridge installed mouse_present=%d",mousePatched);
    return true;
}
static void InstallGameObserver(void) {
    if (!IsCompatibleRuntime()) return;
    void *target=EngineFunction<void *>(0x108a31778);
    const uint32_t expected[4]={0xa9be4ff4,0xa9017bfd,0x910043fd,0xaa0003f3};
    if (!RuntimePrologueMatches(0x108a31778,target)) {Log(@"game_observer unexpected_prologue");return;}
    void *trampoline=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if (trampoline==MAP_FAILED) return;
    memcpy(trampoline,target,16);AbsoluteJump((char *)trampoline+16,(uintptr_t)target+16);
    sys_icache_invalidate(trampoline,32);
    if (mprotect(trampoline,4096,PROT_READ|PROT_EXEC)) return;
    originalGameTick=(void (*)(void *,void *))trampoline;
    uint8_t jump[16];AbsoluteJump(jump,(uintptr_t)GameTick);
    Log(@"game_observer installed=%d",PatchCode(target,jump,16));
}
}

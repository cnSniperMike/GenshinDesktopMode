#pragma once
#include "control_hotswap_state.h"
#include "controller_identity.h"

@interface GDMControllerActivity : NSObject {
@public
    DesktopGamepadIntent intent;
    bool chatPressed,playStation;
}
@property(nonatomic,strong) GCController *controller;
@property(nonatomic,strong) NSArray<GCControllerButtonInput *> *buttons;
@end
@implementation GDMControllerActivity
@end

namespace DesktopControlHotSwap {
using namespace SceneUIProbe;
static NSArray<GDMControllerActivity *> *tracked=@[];
static DesktopControlHotSwapState pending;
static DesktopPointerIntent pointerIntent;
static id eventMonitor;
static bool installed,scheduled,focused;
static bool disconnectFallbackPending;
static NSTimer *pollTimer;
static double mainCheckTime=-100;
static bool mainReady;
static const char *reason="none";
static uint64_t committed;
static void (*chatEvent)(id);
static void (*chatControllerConfirm)(GCController *);
static void QueueAttempt(void);
static void SchedulePoll(void);

static DesktopGamepadIntent::Sample Read(GDMControllerActivity *device) {
    GCExtendedGamepad *pad=device.controller.extendedGamepad;
    DesktopGamepadIntent::Sample sample;
    unsigned index=0;
    for(GCControllerButtonInput *button in device.buttons) {
        if(index>=32)break;
        if(button.pressed)sample.buttons|=1u<<index;
        ++index;
    }
    sample.axes={pad.leftThumbstick.xAxis.value,pad.leftThumbstick.yAxis.value,
        pad.rightThumbstick.xAxis.value,pad.rightThumbstick.yAxis.value,
        pad.leftTrigger.value,pad.rightTrigger.value};
    return sample;
}
static void Seed(void) {
    for(GDMControllerActivity *device in tracked) {device->intent.reset();device->intent.sample(Read(device));device->chatPressed=DesktopControllerIdentity::ChatButton(device.controller,device->playStation).pressed;}
}
static bool InLevelPageVisible(bool includeMenus) {
    // Scene lookups traverse Unity's managed heap and must stay on the main
    // run loop, even if this helper gains another caller.
    if(!NSThread.isMainThread||!findClass||!lookup||!invoke)return false;
    void *go=findClass("UnityEngine.CoreModule.dll","UnityEngine","GameObject");
    void *find=go?lookup(go,"Find",1,0):nullptr;
    auto createString=EngineFunction<void *(*)(const char *)>(0x10e9bb794);
    if(!find||!createString)return false;
    void *name=createString("InLevelMainPage");
    if(!name)return false;
    void *args[]={name};
    if(Call(find,nullptr,args))return true;
    if(!includeMenus)return false;
    // These verified page types also own in-level input while the main HUD is
    // hidden. Searches are bounded and run only during a pending mode change.
    static void *finder,*types[6];
    if(!finder)finder=TeleportCursorBridge::OneTypeArgument(
        findClass("UnityEngine.CoreModule.dll","UnityEngine","Object"),"FindObjectsOfType");
    if(!finder)return false;
    const char *names[]={"MonoInLevelMapPage","MonoCharacterPage",
        "MonoInLevelPlayerProfilePage","MonoInLevelPlayerProfilePageV3","MonoInLevelChatPage","MonoInLevelClockPage"};
    for(unsigned i=0;i<6;++i) {
        if(!types[i])types[i]=TeleportCursorBridge::TypeObject("Assembly-CSharp.dll","MoleMole",names[i]);
        if(!types[i])continue;
        void *typeArgs[]={types[i]},*array=Call(finder,nullptr,typeArgs);
        uint64_t count=array?*(uint64_t *)((char *)array+24):0;
        if(count>16)continue;
        for(uint64_t j=0;j<count;++j)
            if(TeleportCursorBridge::Active(((void **)((char *)array+32))[j]))return true;
    }
    return false;
}
static bool NativeReady(int mode,double now) {
    DesktopHotSwapContext context;
    context.initialized=DesktopControlPreference::bootstrapReady.load()&&DesktopUI::beforeInputSwitch&&
        DesktopUI::afterInputSwitch&&NativeBridge::frames.load();
    context.foreground=NativeBridge::IsForeground();context.web=NativeBridge::nativeOverlayActive.load();
    if(!context.initialized||!context.foreground||context.web)return false;
    char *owner=VerifiedInputOwner(),*ui=VerifiedUIOwner();
    context.owners=owner&&ui;if(!context.owners)return false;
    if(mode==3) {
        context.physicalController=tracked.count!=0;
        if(!context.physicalController)return false;
        if(now-mainCheckTime>=.2) {mainCheckTime=now;mainReady=InLevelPageVisible(true);}
        context.gameplayPage=mainReady;
        char *game=*(char **)(owner+GenshinOffsets().ownerGameInput);
        void *player=game?*(void **)(game+GenshinOffsets().inputPlayer):nullptr;
        void *controllers=player?*(void **)((char *)player+GenshinOffsets().playerControllers):nullptr;
        void *count=controllers?Property(controllers,"get_joystickCount"):nullptr;
        context.nativeJoystick=count&&*(int *)((char *)count+16)>0;
    }
    return context.ready(mode);
}
static void Attempt(void) {
    scheduled=false;
    double now=CACurrentMediaTime();int mode=pending.due(now);
    if(!mode)return;
    if(NativeBridge::nativeOverlayActive || !NativeBridge::IsForeground()) {pending.cancel();return;}
    uint64_t revision=pending.revision;
    bool remember=pending.persist,ready=NativeReady(mode,now),accepted=false;
    if(ready) {
        char *owner=VerifiedInputOwner(),*ui=VerifiedUIOwner();
        int layout=DesktopControllerLayout::Select(mode,1);
        accepted=owner&&ui&&*(int *)(owner+GenshinOffsets().ownerInputMode)==mode&&
            *(int *)(ui+GenshinOffsets().uiLayout)==layout;
        if(!accepted)accepted=DesktopUI::SwitchInputMode(mode);
        else DesktopUI::CompleteInputSwitch(owner);
    }
    // A native callback can make a newer explicit selection during a rebuild.
    if(revision!=pending.revision)return;
    unsigned attempt=pending.attempts+1;
    pending.finish(accepted,now);
    if(accepted) {
        if(remember)DesktopControlPreference::RememberSelection(mode,ControlDevice::CurrentMode(),"hardware_hot_swap");
        ++committed;
        Log(@"control_hot_swap committed=%llu mode=%d source=%s saved=%d",committed,mode,reason,remember);
        ControllerHaptics::ReportStatus();
    } else if(attempt==1||!pending.pending)
        Log(@"control_hot_swap deferred=%d mode=%d source=%s native_ready=%d attempt=%u",pending.pending!=0,mode,reason,ready,attempt);
}
static void QueueAttempt(void) {
    if(scheduled||!pending.due(CACurrentMediaTime()))return;
    scheduled=true;
    // Do not rebuild native pages from inside an input update or OS event.
    dispatch_async(dispatch_get_main_queue(),^{Attempt();});
}
static bool Request(int mode,const char *source,bool remember=true,bool fallback=false) {
    if(!DesktopControlPreference::bootstrapReady.load() || !DesktopUI::afterInputSwitch ||
       !NativeBridge::IsForeground() || NativeBridge::nativeOverlayActive)return false;
    bool current=NativeBridge::controllerInputActive.load()==(mode==3);
    if(current&&!pending.pending&&(!remember||DesktopControlPreference::SavedMode()==mode))return true;
    if(!pending.request(mode,CACurrentMediaTime(),remember,fallback))return false;
    reason=source;QueueAttempt();
    return true;
}
static void RefreshControllers(void) {
    bool hadController=tracked.count!=0;
    NSMutableArray *next=[NSMutableArray array];
    for(GCController *controller in GCController.controllers) {
        if(!controller.extendedGamepad||controller.isSnapshot)continue;
        GDMControllerActivity *existing=nil;
        for(GDMControllerActivity *device in tracked)if(device.controller==controller) {existing=device;break;}
        if(existing) {[next addObject:existing];continue;}
        GDMControllerActivity *device=[GDMControllerActivity new];device.controller=controller;
        // Cache once; stable bit positions without per-frame enumeration,
        // allocation, snapshots, or replacing any game's input handlers.
        device.buttons=controller.extendedGamepad.allButtons.allObjects;
        device->playStation=DesktopControllerIdentity::IsPlayStation(controller);
        device->intent.sample(Read(device));device->chatPressed=DesktopControllerIdentity::ChatButton(controller,device->playStation).pressed;[next addObject:device];
    }
    tracked=[next copy];
    if(!pollTimer || hadController!=(tracked.count!=0)) SchedulePoll();
    DesktopControllerIdentity::Refresh();
    if(hadController&&!tracked.count) {
        pending.cancel();ControllerHaptics::Stop();
        disconnectFallbackPending=true;
        if(Request(2,"controller_disconnected",false,true))disconnectFallbackPending=false;
    }
    Log(@"control_hot_swap connected=%lu snapshots_ignored=1 native_handlers_preserved=1",(unsigned long)tracked.count);
}
static void ExplicitSelection(int mode) {
    if(!NSThread.isMainThread) {dispatch_async(dispatch_get_main_queue(),^{ExplicitSelection(mode);});return;}
    pending.manual(CACurrentMediaTime());pointerIntent.reset();Seed();
}
static void PollOnMain(bool active) {
    if(focused!=active) {
        focused=active;if(!active)pending.cancel();pointerIntent.reset();Seed();
        mainCheckTime=-100;mainReady=false;
    }
    if(active&&DesktopControlPreference::bootstrapReady.load()&&!DesktopControlPreference::gameplayReady.load()) {
        double now=CACurrentMediaTime();
        if(now-mainCheckTime>=.2){mainCheckTime=now;mainReady=InLevelPageVisible(false);
            if(mainReady){DesktopControlPreference::gameplayReady=true;
                dispatch_async(dispatch_get_main_queue(),^{char *owner=VerifiedInputOwner();if(owner)DesktopUIGeometry::After(*(int *)(owner+GenshinOffsets().ownerInputMode));});
                Log(@"desktop_controller_startup gameplay_ready=1 login_completed=1 saved_mode=%d",DesktopControlPreference::SavedMode());}}
    }
    if(active&&disconnectFallbackPending)
        if(Request(2,"controller_disconnected",false,true))disconnectFallbackPending=false;
    if(!active || (!tracked.count&&!pending.pending))return;
    for(GDMControllerActivity *device in tracked) {
        bool pressed=DesktopControllerIdentity::ChatButton(device.controller,device->playStation).pressed;
        bool freshChat=pressed&&!device->chatPressed;device->chatPressed=pressed;
        bool intentional=device->intent.sample(Read(device));
        // Capture the currently open chat before a queued mode handoff
        // rebuilds it. The focus callback itself runs on the main queue.
        if(freshChat&&!NativeBridge::nativeOverlayActive&&chatControllerConfirm)
            chatControllerConfirm(device.controller);
        if(intentional&&!NativeBridge::nativeOverlayActive) {
            DesktopControllerIdentity::Select(device.controller,device->playStation);
            ControllerHaptics::RefreshController(device.controller);
            Request(3,"gamepad");
        }
    }
    QueueAttempt();
}
// AppKit delivers device events and owns the game UI on this run loop. Unity
// may run its input callback on a job thread while this UI is being rebuilt.
static void SchedulePoll(void) {
    [pollTimer invalidate];
    NSTimeInterval interval=tracked.count ? 1.0/120.0 : .2;
    pollTimer=[NSTimer timerWithTimeInterval:interval repeats:YES block:^(NSTimer *) {
        bool active=NativeBridge::IsForeground();
        ControllerHaptics::ObserveForeground(active);
        PollOnMain(active);
    }];
    [NSRunLoop.mainRunLoop addTimer:pollTimer forMode:NSRunLoopCommonModes];
}
static id ObserveEvent(id event) {
    NSUInteger type=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("type"));
    bool motion=type==5 || type==6 || type==7 || type==27;
    if (motion && !tracked.count && !pending.pending && !NativeBridge::controllerInputActive) return event;
    if(!NativeBridge::IsForeground()||!NativeGameWindow()||NativeBridge::nativeOverlayActive)return event;
    id window=((id (*)(id,SEL))objc_msgSend)(event,sel_registerName("window"));
    if(!window&&(type==10||type==12)) {
        id game=NativeGameWindow();
        if(game&&((BOOL (*)(id,SEL))objc_msgSend)(game,sel_registerName("isKeyWindow")))window=game;
    }
    if(window!=NativeGameWindow())return event;
    NSUInteger flags=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("modifierFlags"));
    if(flags&(1ULL<<20))return event; // Command shortcuts belong to macOS.
    if(type==10) {
        bool repeat=((BOOL (*)(id,SEL))objc_msgSend)(event,sel_registerName("isARepeat"));
        if(!repeat)Request(2,"keyboard");
    } else if(type==12) {
        unsigned short code=((unsigned short (*)(id,SEL))objc_msgSend)(event,sel_registerName("keyCode"));
        uint64_t modifier=code==59||code==62?1ULL<<18:code==58||code==61?1ULL<<19:code==56||code==60?1ULL<<17:0;
        if(modifier&&(flags&modifier))Request(2,"keyboard_modifier");
    } else {
        CGRect bounds;
        if(!GameContentBounds(bounds)||!CGRectContainsPoint(bounds,DesktopMouseLocation()))return event;
        double now=CACurrentMediaTime();
        if(type==1||type==3||type==25)Request(2,"mouse_button");
        else if(type==5||type==6||type==7||type==27) {
            double x=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("deltaX"));
            double y=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("deltaY"));
            if(pointerIntent.motion(x,y,now))Request(2,"mouse_motion");
        } else if(type==22) {
            double x=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("scrollingDeltaX"));
            double y=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("scrollingDeltaY"));
            NSUInteger momentum=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("momentumPhase"));
            if(pointerIntent.wheel(x,y,momentum!=0,now))Request(2,"mouse_wheel");
        }
    }
    if(chatEvent)chatEvent(event);
    return event;
}
static void ObserveDispatch(id event) {ObserveEvent(event);}
static bool CanBatchMapMotion(void) {
    return DesktopControlPreference::bootstrapReady.load() &&
        DesktopControlPreference::gameplayReady.load() &&
        !NativeBridge::controllerInputActive.load() && !pending.pending &&
        DesktopControlPreference::SavedMode()==2;
}
static void ObserveBatchedMapMotion(double x,double y) {
    // Preserve the continuous-motion intent latch while keyboard already owns
    // the map, so a held drag cannot masquerade as a new gesture after a pad
    // button requests a handoff. No window/scene lookup or mode write here.
    pointerIntent.motion(x,y,CACurrentMediaTime());
}
static void Install(void) {
    if(installed||!IsCompatibleRuntime())return;
    installed=true;
    NativeBridge::observeDesktopEvent=ObserveDispatch;
    NativeBridge::canBatchMapMotion=CanBatchMapMotion;
    NativeBridge::observeBatchedMapMotion=ObserveBatchedMapMotion;
    DesktopControlPreference::explicitSelection=ExplicitSelection;
    RefreshControllers();
    NSNotificationCenter *nc=NSNotificationCenter.defaultCenter;
    for(NSString *name in @[GCControllerDidConnectNotification,GCControllerDidDisconnectNotification])
        [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {RefreshControllers();}];
    for(NSString *name in @[UIApplicationWillResignActiveNotification,@"NSApplicationWillResignActiveNotification"])
        [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
            pending.cancel();pointerIntent.reset();Seed();focused=false;
        }];
    uint64_t mask=(1ULL<<1)|(1ULL<<3)|(1ULL<<25)|(1ULL<<5)|(1ULL<<6)|(1ULL<<7)|(1ULL<<27)|(1ULL<<10)|(1ULL<<12)|(1ULL<<22);
    // The existing application dispatch sees keys even when the Esc guard
    // consumes them later. Use a local monitor only if that route is absent.
    if(!originalApplicationSendEvent) {
        id (^handler)(id)=^id(id event){return originalApplicationSendEvent?event:ObserveEvent(event);};
        eventMonitor=((id (*)(id,SEL,uint64_t,id))objc_msgSend)(NSClassFromString(@"NSEvent"),
            sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"),mask,handler);
    }
    Log(@"control_hot_swap installed=1 application_dispatch=%d fallback_monitor=%d input_handlers_replaced=0 default=last_used controller_ingame_gate=1 keyboard_menu_return=1 poll_owner=main_runloop",
        originalApplicationSendEvent!=nullptr,eventMonitor!=nil);
}
}

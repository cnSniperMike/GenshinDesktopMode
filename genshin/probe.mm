#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <mach-o/dyld.h>
#import <dlfcn.h>
#include <stdarg.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <algorithm>
#include <vector>
#include "game_identity.h"

static NSFileHandle *probeLog;
static unsigned keyboardEvents, mouseEvents;
static const bool diagnosticsEnabled=getenv("GENSHIN_DESKTOP_DIAGNOSTICS")!=nullptr;

static void Log(NSString *format, ...) {
    va_list args;
    va_start(args, format);
    NSString *line = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    NSData *data = [[NSString stringWithFormat:@"%@ %@\n", [NSDate date], line]
                   dataUsingEncoding:NSUTF8StringEncoding];
    @synchronized(probeLog) { [probeLog writeData:data]; }
}

#include "control_device_preference.h"
#include "presentation_policy.h"
#include "runtime_probe.h"
#include "input_bridge.h"
#include "pointer_bridge.h"
#include "desktop_ui.h"
#include "scene_ui_probe.h"
#include "wheel_probe.h"
#include "wheel_bridge.h"
#include "native_pc.h"
#include "runtime_ready.h"
#include "teleport_bridge.h"
#include "native_camera_pointer.h"
#include "native_codex_pointer.h"
#include "native_character_pointer.h"
#include "native_map_drag.h"
#include "control_device.h"
#include "title_quit.h"
#include "desktop_ui_geometry.h"

#define GDM_NATIVE_PRODUCT 1
#define GDM_NATIVE_SETTINGS 1
#include "native_product_backend.h"
#include "ps_audio_haptics.h"
#include "native_adaptive_trigger.h"
#include "controller_haptics.h"
#include "control_hotswap.h"
#include "native_chat_focus.h"
#include "interaction_wheel.h"
#include "native_controller_sprites.h"
#include "native_clock_pointer.h"
#include "native_team_pointer.h"

static void ConfigureGamePresentation(void) {
    if (!IsModernRuntime() || NativeBridge::frames.load() == 0 ||
        !NativeBridge::IsForeground() || NativeBridge::nativeOverlayActive.load()) return;

    static __weak UIView *gameView;
    static __weak CAMetalLayer *configuredLayer;
    UIView *view = gameView;
    if (!view.window || view.window.hidden || view.window.alpha < 0.02) {
        view = nil;
        for (UIWindow *window in UIApplication.sharedApplication.windows) {
            if (window.hidden || window.alpha < 0.02) continue;
            UIView *candidate = FindUnityView(window);
            if (![candidate.layer isKindOfClass:CAMetalLayer.class]) continue;
            if (view) return;
            view = candidate;
        }
        gameView = view;
    }
    if (![view.layer isKindOfClass:CAMetalLayer.class]) return;

    CAMetalLayer *layer = (CAMetalLayer *)view.layer;
    if (layer == configuredLayer) return;
    configuredLayer = layer;
    NSUInteger current = layer.maximumDrawableCount;

    if (!ReducedDrawableQueueRequested()) {
        // Leave the game and system default (typically three) untouched.
        Log(@"game_presentation buffers=%lu opt_in_reduced=0 unchanged=1",
            (unsigned long)current);
        return;
    }
    if (current != 3) {
        Log(@"game_presentation buffers=%lu requested=2 skipped=1 reason=not_default",
            (unsigned long)current);
        return;
    }

    // A shorter drawable queue reduces the time a rendered response waits for
    // display. Documented tradeoff: it can lock the frame rate to half the
    // refresh rate when a frame does not fit one interval.
    layer.maximumDrawableCount = 2;
    if (layer.maximumDrawableCount == 2)
        Log(@"game_presentation buffers=2 source=unity_view opt_in=1");
}

// Sample Unity's own counters on the main run loop.
static void LogGameFrameRate(void) {
    using namespace SceneUIProbe;
    static void *targetRate,*renderedFrames,*vsync;
    static double lastTime;
    static int lastCount;
    id window=NativeGameWindow();
    if(!window || !NativeBridge::frames.load() || !NativeBridge::IsForeground()) {
        lastTime=0;
        return;
    }
    auto getter=[](const char *type,const char *name) {
        void *cls=findClass("UnityEngine.CoreModule.dll","UnityEngine",type);
        return cls?lookup(cls,name,0,0):nullptr;
    };
    if(!renderedFrames) {
        renderedFrames=getter("Time","get_renderedFrameCount");
        targetRate=getter("Application","get_targetFrameRate");
        vsync=getter("QualitySettings","get_vSyncCount");
    }
    auto value=[](void *method) {
        void *box=Call(method,nullptr);
        return box?*(int *)((char *)box+16):-1;
    };
    id screen=((id (*)(id,SEL))objc_msgSend)(window,sel_registerName("screen"));
    if(!screen)return;
    auto displayHz=((NSInteger (*)(id,SEL))objc_msgSend)(screen,sel_registerName("maximumFramesPerSecond"));
    int count=value(renderedFrames);
    double now=CACurrentMediaTime();
    if(lastTime && count>=lastCount && count>=0)
        Log(@"game_frame_rate target=%d rendered_fps=%.1f display_max_hz=%ld vsync_count=%d low_power=%d",
            value(targetRate),(count-lastCount)/(now-lastTime),(long)displayHz,value(vsync),
            NSProcessInfo.processInfo.lowPowerModeEnabled);
    lastCount=count;lastTime=now;
}

static void ScanInputNames(void) {
    const char *terms[] = {"MoleMole", "ReInput", "IsPCMode", "IsWindowsPlatform",
                          "StandaloneInputModule", "UnityEngine.InputSystem", "Rewired"};
    unsigned counts[7] = {};
    mach_vm_address_t address = 0;
    uint64_t scanned = 0;
    std::vector<char> buffer(1024 * 1024);
    while (scanned < 1024ULL * 1024 * 1024) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = mach_vm_region(mach_task_self(), &address, &size,
                VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || !size) break;
        if ((info.protection & VM_PROT_READ) && (info.protection & VM_PROT_WRITE) &&
            !(info.protection & VM_PROT_EXECUTE) && size <= 256ULL * 1024 * 1024) {
            for (uint64_t offset = 0; offset < size; offset += buffer.size()) {
                mach_vm_size_t got = 0;
                auto length = std::min<uint64_t>(buffer.size(), size - offset);
                if (mach_vm_read_overwrite(mach_task_self(), address + offset, length,
                        (mach_vm_address_t)buffer.data(), &got) != KERN_SUCCESS) continue;
                scanned += got;
                for (unsigned t = 0; t < 7; ++t) {
                    auto begin = buffer.data();
                    auto end = begin + got;
                    const size_t termLength = strlen(terms[t]);
                    while ((begin = std::search(begin, end, terms[t], terms[t]+termLength)) != end) {
                        if (counts[t]++ < 8) Log(@"runtime_name %s address=0x%llx", terms[t],
                            address + offset + (begin - buffer.data()));
                        begin += termLength;
                    }
                }
                if (scanned >= 1024ULL * 1024 * 1024) break;
            }
        }
        if (address + size < address) break;
        address += size;
    }
    for (unsigned t=0;t<7;++t) Log(@"runtime_name_total %s=%u",terms[t],counts[t]);
    Log(@"runtime_scan_finished bytes=%llu",scanned);
}

static void EnableKeyboardPreferences(void) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSString *text = [defaults stringForKey:@"GENERAL_DATA"];
    if (!text) { Log(@"keyboard_preferences no_existing_config"); return; }
    NSMutableDictionary *settings = [NSJSONSerialization JSONObjectWithData:
        [text dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingMutableContainers error:nil];
    if (![settings isKindOfClass:NSMutableDictionary.class]) return;
    settings[@"rewiredEnableKeyboard"] = @YES;
    settings[@"rewiredDisableKeyboard"] = @NO;
    settings[@"enableInputHotSwap"] = @YES;
    NSData *json = [NSJSONSerialization dataWithJSONObject:settings options:0 error:nil];
    [defaults setObject:[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] forKey:@"GENERAL_DATA"];
    [defaults synchronize];
    Log(@"keyboard_preferences enabled_in_isolated_config");
}

static void InspectClass(const char *name) {
    Class cls = objc_getClass(name);
    Log(@"class %s present=%d", name, cls != Nil);
    if (!cls) return;
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count; ++i) {
        SEL sel = method_getName(methods[i]);
        const char *s = sel_getName(sel);
        if (strstr(s, "touch") || strstr(s, "key") || strstr(s, "Key") ||
            strstr(s, "press") || strstr(s, "Command") || strstr(s, "Pointer")) {
            Log(@"method %s %s imp=%p", name, s, method_getImplementation(methods[i]));
        }
    }
    free(methods);
}

static void AttachKeyboard(GCKeyboard *keyboard) {
    if (!keyboard) return;
    keyboard.keyboardInput.keyChangedHandler = ^(GCKeyboardInput *, GCControllerButtonInput *, GCKeyCode key, BOOL pressed) {
        ++keyboardEvents;
        // Record counts only, never typed characters or credentials.
        if (keyboardEvents <= 8) Log(@"keyboard_event count=%u", keyboardEvents);
        // Queue immediately: an extra async hop can replay a stale press after focus loss.
        int unityKey=UnityKeyForHID((int)key);
        if(unityKey) NativeBridge::QueueKey(unityKey,pressed);
        if(unityKey==9) SetDesktopTabHeld(pressed);
    };
    Log(@"keyboard_handler_installed");
}

static void AttachMouse(GCMouse *mouse) {
    if (!mouse) return;
    // Movement comes solely from AppKit's system-scaled events. GC's raw
    // movement must not also be forwarded or applied as an extra multiplier.
    mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput *, float x, float y) {
        ++mouseEvents;
        if(diagnosticsEnabled) {rawMotionX+=llabs(llround(x*1000));rawMotionY+=llabs(llround(y*1000));}
    };
    mouse.mouseInput.scroll.valueChangedHandler=^(GCControllerDirectionPad *,float x,float y) {NativeBridge::Scroll(x,y);};
    mouse.mouseInput.leftButton.pressedChangedHandler=^(GCControllerButtonInput *,float,BOOL down) {NativeBridge::MouseButton(0,down);};
    mouse.mouseInput.rightButton.pressedChangedHandler=^(GCControllerButtonInput *,float,BOOL down) {NativeBridge::MouseButton(1,down);};
    mouse.mouseInput.middleButton.pressedChangedHandler=^(GCControllerButtonInput *,float,BOOL down) {NativeBridge::MouseButton(2,down);};
    Log(@"mouse_handler_installed");
}

static bool ClaimDesktopBridgeInstance(void *initializer) {
    Dl_info own={};
    if(!dladdr(initializer,&own) || !own.dli_fname) return false;
    for(uint32_t i=0;i<_dyld_image_count();++i) {
        const char *path=_dyld_get_image_name(i);
        if(!path || (const void *)_dyld_get_image_header(i)==own.dli_fbase) continue;
        const char *name=strrchr(path,'/');name=name?name+1:path;
        // The existing in-app bridge takes precedence over an additionally
        // imported copy. Never install a second set of hooks in one process.
        if(!strcmp(name,"libGenshinDesktopInput.dylib")) return false;
    }
    Class marker=objc_allocateClassPair(NSObject.class,"GenshinDesktopModeActiveBridge",0);
    if(!marker) return false;
    objc_registerClassPair(marker);return true;
}

__attribute__((constructor)) static void InitializeProbe(void) {
    @autoreleasepool {
        if(!GenshinIdentity::IsGame(NSBundle.mainBundle.bundleIdentifier.UTF8String,
                                   NSBundle.mainBundle.executablePath.UTF8String)) return;
        if(!ClaimDesktopBridgeInstance((void *)InitializeProbe)) {
            NSLog(@"[GenshinDesktopMode] Existing desktop bridge detected; duplicate copy skipped.");return;
        }
        NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/GenshinDesktopInput.log"];
        [[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
                                withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
        probeLog = [NSFileHandle fileHandleForWritingAtPath:path];
        Log(@"probe_loaded bundle=%@ executable=%@", NSBundle.mainBundle.bundleIdentifier,
            NSBundle.mainBundle.executablePath);
        Log(@"main_image=%s slide=0x%llx", _dyld_get_image_name(0),
            (unsigned long long)_dyld_get_image_vmaddr_slide(0));
        Log(@"compatibility_policy identity=bundle_identifier region_gate=0 version_gate=0 uuid_gate=0 check=runtime_code_layout");
        // Package versions are diagnostic labels only. Keep an explicit local
        // report so a future client can distinguish a label change from a
        // genuinely different internal layout without calling old addresses.
        NSDictionary *info=NSBundle.mainBundle.infoDictionary;
        NSString *profile=[NSString stringWithUTF8String:RuntimeProfileName()];
        NSDictionary *compatibility=@{@"plugin_revision":@44,
            @"bundle":NSBundle.mainBundle.bundleIdentifier?:@"",
            @"display_version":info[@"CFBundleShortVersionString"]?:@"",
            @"build":info[@"CFBundleVersion"]?:@"",
            @"version_fields_used_as_gate":@NO,@"uuid_used_as_gate":@NO,
            @"runtime_profile":profile,@"code_layout_accepted":@(IsCompatibleRuntime()),
            @"reason":IsCompatibleRuntime()?@"known_internal_layout":@"internal_layout_changed",
            @"first_mismatch_address":[NSString stringWithFormat:@"0x%llx",(unsigned long long)runtimeLayoutMismatch]};
        NSString *reportPath=[[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"GenshinDesktopCompatibility.json"];
        [[NSJSONSerialization dataWithJSONObject:compatibility options:NSJSONWritingPrettyPrinted error:nil] writeToFile:reportPath atomically:YES];
        if(!IsCompatibleRuntime()) {
            Log(@"probe_disabled runtime_layout_mismatch va=0x%llx",(unsigned long long)runtimeLayoutMismatch);
            return;
        }
        Log(@"input_revision=46 native_authored_motion=%d native_vibration_settings=%d keyboard_ui_geometry_restore=1",
            GDM_EMBEDDED_MOTION_PACK, GDM_EMBEDDED_MOTION_PACK);
        Log(@"input_features=mixed_container_conditions codex_3d_mouse modal_padding_restore friend_wheel build=r46");
        // Authored native PCM uses no ordinary game-audio capture hooks.
#if defined(GDM_CONTROLLER_UI_PREVIEW)
        Log(@"controller_ui_preview=1 physical_controller_emulated=0");
#endif
        Log(@"runtime_profile=%s identity_gate=bundle_identifier",RuntimeProfileName());
        const auto &discovered=ActiveRuntimeBindings();
        Log(@"runtime_discovery dynamic=%d layout_matches=%u layout=0x%llx input_getter=0x%llx abi_verified=1",
            discovered.dynamic,discovered.layoutMatches,(unsigned long long)discovered.layoutEntry,
            (unsigned long long)discovered.inputGetter);
        DesktopUI::InstallInitialLayout();
        NativeBridge::InstallHardwareReader();
        const char *symbols[] = {"il2cpp_domain_get", "il2cpp_class_from_name", "il2cpp_resolve_icall",
                                 "UnitySetKeyState", "UnitySendMessage", "UnityGetGLView"};
        for (const char *name : symbols) Log(@"symbol %s=%p", name, dlsym(RTLD_DEFAULT, name));
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *) { NativeBridge::Deactivate();desktopEscapeGuard.reset(); }];
        [nc addObserverForName:UISceneWillDeactivateNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *) { NativeBridge::Deactivate();desktopEscapeGuard.reset(); }];
        [nc addObserverForName:@"NSApplicationWillResignActiveNotification" object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *) { NativeBridge::Deactivate();desktopEscapeGuard.reset(); }];
        dispatch_async(dispatch_get_main_queue(), ^{InstallSystemMouseMotion();InstallDeferredDesktopDevices();InstallNativeProductBackend([](bool value){if(value)PSAudio::RefreshOutputState();else ControllerHaptics::Stop();});ControllerHaptics::Install();NativeControllerSprites::Install();NativeChatFocus::Install();InteractionWheel::Install();DesktopControlHotSwap::Install();});
        [nc addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *) { NativeBridge::ReleaseKeys();Log(@"keyboard_disconnected released_all=1"); }];
        [nc addObserverForName:GCMouseDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *) { NativeBridge::ReleaseKeys();Log(@"mouse_disconnected released_all=1"); }];
        [nc addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *n) { AttachKeyboard(n.object); }];
        [nc addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *n) { AttachMouse(n.object); }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            InstallSystemMouseMotion();
            if(diagnosticsEnabled) {InspectClass("UnityView");InspectClass("UnityViewControllerBase");}
            AttachKeyboard(GCKeyboard.coalescedKeyboard);
            for (GCMouse *mouse in GCMouse.mice) AttachMouse(mouse);
            Log(@"main_queue_alive windows=%lu", (unsigned long)UIApplication.sharedApplication.windows.count);
            [NSTimer scheduledTimerWithTimeInterval:5 repeats:YES block:^(NSTimer *) {
                ConfigureGamePresentation();
                LogGameFrameRate();
                Log(@"heartbeat keyboard=%u mouse=%u frames=%llu forwarded_keys=%llu forwarded_mouse_frames=%llu action_frames=%llu repairs=%llu hardware_differences=%llu windows=%lu", keyboardEvents, mouseEvents,
                    NativeBridge::frames.load(),NativeBridge::forwardedKeys.load(),NativeBridge::forwardedMouseFrames.load(),
                    NativeBridge::gameActionFrames.load(),
                    NativeBridge::repairedKeyStates.load(),NativeBridge::hardwareDisagreements.load(),
                    (unsigned long)UIApplication.sharedApplication.windows.count);
            }];
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
            dispatch_get_main_queue(), ^{
                nativeInputManager=EngineFunction<char *(*)()>(0x10eec0180)();
                NativeBridge::Install();
            });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 14 * NSEC_PER_SEC),
            dispatch_get_main_queue(), ^{ NativeBridge::InstallGameObserver(); InstallPointerPosition(); InstallCursorBridge(); WheelBridge::Install(); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
            dispatch_get_main_queue(), ^{ DesktopUI::Install(); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 23 * NSEC_PER_SEC),
            dispatch_get_main_queue(), ^{
                if(diagnosticsEnabled) WheelProbe::Install();
                NativePC::Install();TeleportCursorBridge::Install();ControlDevice::Install();
                NativeCharacterPointer::TryInstall();NativeClockPointer::Install();DesktopUIGeometry::Install();
                bool directMap=NativeCameraPointer::TryInstall();
                if(!directMap) {
                    Log(@"native_camera_mouse unavailable=1 retained_r37_map_path=1");
                }
                NativeTeamPointer::Install();if(!directMap)NativeMapDrag::Install();
            });
    }
}

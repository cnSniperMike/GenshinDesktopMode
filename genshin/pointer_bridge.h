#include "pointer_motion.h"
#include "map_pointer_frame.h"
#include "pointer_event.h"
#include "pointer_touch.h"
#include "escape_guard.h"
#include "key_alert_guard.h"
#include "teleport_cursor_policy.h"
#include "pointer_geometry.h"
#include "scroll_event.h"
#include "native_web_style.h"
#include "native_web_escape.h"
#include <cmath>
#include "mac_pointer_host.h"
#include "native_cursor.h"

static std::atomic<int64_t> rawMotionX{0},rawMotionY{0};

static std::atomic<bool> cursorVisible{true};
static std::atomic<uint64_t> cursorShowRevision{0};
static TeleportCursorPolicy teleportCursorPolicy;
static std::atomic<int> cursorLockMode{0};
static NSMutableArray<UIPointerInteraction *> *desktopPointerInteractions;
static bool cameraCursorHidden;
static bool tabCursorRequested;
static NSData *gameCursorPNG;
static CGSize gameCursorSize;
static CGPoint gameCursorHotspot;
static id loginNativeCursor, gameNativeCursor, ownedNativeCursor;
static __weak id gameHostWindow;
static NSUInteger menuTrackingDepth;
static bool waitForContent;
namespace TitleQuit { static bool Handle(id window,id event,bool down); }
static UIView *FindUnityView(UIView *view);
static void UpdateCursorImage(const CGRect *eventBounds=nullptr, const CGPoint *eventPoint=nullptr);
static void RefreshMacPointerHost(id eventWindow=nil);
static void RefreshPointerStateNow(void);
#include "native_account_input.h"
static void UpdateNativeOverlayState(void);
static bool WantsVisibleCursor(void);
static bool nativePointerCaptured;
static bool pointerCaptureSuspended;
static std::atomic<bool> gameplayPointerStarted{false};
static CGPoint capturedPointer;
static DesktopMotion desktopMotion;
static bool desktopMapMotionBatched;
static id desktopMouseMonitor;
static id capturedNativeWindow;
static bool previousMouseMovedEvents;
static id freeNativeWindow;
static bool previousFreeMouseMovedEvents;
static bool capturedCursorRects;
static id transparentNativeCursor;
static void (*originalCursorSet)(id,SEL);
static void (*originalWindowSendEvent)(id,SEL,id);
static void (*originalApplicationSendEvent)(id,SEL,id);
static bool desktopNativePressInContent;
static DesktopMouseEventSequence desktopButtonSequence;
static DesktopMouseEventSequence desktopDragSequence;
static DesktopMouseEventSequence desktopMotionSequence;
static DesktopScrollEventSequence desktopScrollSequence;
static DesktopTouchHistory desktopTouchHistory;
static DesktopTouchStreamOwnership desktopMapTouchStreams;
static DesktopEscapeGuard desktopEscapeGuard;
static bool desktopNoticeEscapeOwned;
static auto associatePointer=(int (*)(uint32_t))dlsym(RTLD_DEFAULT,"CGAssociateMouseAndMouseCursorPosition");
static auto warpPointer=(int (*)(CGPoint))dlsym(RTLD_DEFAULT,"CGWarpMouseCursorPosition");
static bool ControllerOwnsGamePointer(void) {
    // SDK pages keep their own physical mouse and gold WebKit cursor. Within
    // the game, controller virtual-cursor rendering belongs to the game.
    return NativeBridge::controllerInputActive.load() && !NativeBridge::nativeOverlayActive.load();
}

static id NativeGameWindow(void) {
    return NativeBridge::macWindowSuspended.load() ? nil : gameHostWindow;
}
static bool DesktopWebAppIsActive(void) {
    // A native SDK web page can deactivate the Unity UIKit scene while its
    // macOS application and web window still own keyboard/mouse focus.
    Class cls=NSClassFromString(@"NSApplication");
    id app=((id (*)(id,SEL))objc_msgSend)(cls,sel_registerName("sharedApplication"));
    return app&&((BOOL (*)(id,SEL))objc_msgSend)(app,sel_registerName("isActive"));
}
static bool ShouldSilenceUnhandledGameKey(void) {
    if(NativeBridge::IsForeground() && NativeGameWindow()) {
        id app=((id (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSApplication"),sel_registerName("sharedApplication"));
        id event=((id (*)(id,SEL))objc_msgSend)(app,sel_registerName("currentEvent"));
        if(event && ((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("type"))==10) {
            NSUInteger flags=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("modifierFlags"));
            // AppKit normally beeps here after the responder chain failed.
            // Input has already been offered to editors/IME and game handlers.
            // Control/Option modify gameplay (walk and show cursor). Command
            // chords and text input already handled by an editor pass normally.
            if(DesktopKeyAlerts::IsSilentGameKey(10,flags)) {
                static unsigned suppressed[4]={};
                unsigned group=((flags&(1ULL<<18))?1:0)|((flags&(1ULL<<19))?2:0);
                if(++suppressed[group]<=3) Log(@"game_key_alert_suppressed count=%u control=%d option=%d",
                    suppressed[group],(group&1)!=0,(group&2)!=0);
                return true;
            }
        }
    }
    return false;
}
static void InstallGameKeyAlertGuard(void) {
    static bool installed=false;if(installed) return;
    installed=DesktopKeyAlerts::Install(ShouldSilenceUnhandledGameKey);
    Log(@"game_key_alert_guard installed=%d scope=unhandled_game_keyDown",installed);
}
static CGRect NativeGameContentRect(id window) {
    NSUInteger style=((NSUInteger (*)(id,SEL))objc_msgSend)(window,sel_registerName("styleMask"));
    if(style&(1ULL<<14)) {
        id content=((id (*)(id,SEL))objc_msgSend)(window,sel_registerName("contentView"));
        CGRect rect=((CGRect (*)(id,SEL))objc_msgSend)(content,sel_registerName("bounds"));
        return ((CGRect (*)(id,SEL,CGRect,id))objc_msgSend)(content,sel_registerName("convertRect:toView:"),rect,nil);
    }
    return ((CGRect (*)(id,SEL))objc_msgSend)(window,sel_registerName("contentLayoutRect"));
}
static bool GameContentBounds(CGRect &bounds,bool fitToGame=true) {
    id window=NativeGameWindow();if(!window) return false;
    CGRect rect=NativeGameContentRect(window);
    rect=((CGRect (*)(id,SEL,CGRect))objc_msgSend)(window,sel_registerName("convertRectToScreen:"),rect);
    NSArray *screens=((id (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSScreen"),sel_registerName("screens"));
    if(!screens.count || rect.size.width<100 || rect.size.height<100) return false;
    CGRect primary=((CGRect (*)(id,SEL))objc_msgSend)(screens.firstObject,sel_registerName("frame"));
    bounds=CGRectMake(rect.origin.x,CGRectGetMaxY(primary)-CGRectGetMaxY(rect),rect.size.width,rect.size.height);
    // Publish the live Unity viewport when the window changes size, screen or scale.
    // The 20 Hz main-run-loop refresh also covers in-game resolution changes.
    static CGRect measuredBounds=CGRectNull;
    static CGFloat measuredScale=0;
    if (NSThread.isMainThread) {
        const CGFloat scale=((CGFloat (*)(id,SEL))objc_msgSend)(window,sel_registerName("backingScaleFactor"));
        if (!CGRectEqualToRect(measuredBounds,bounds) || scale!=measuredScale) {
            NativeBridge::RefreshScreenMetrics();
            measuredBounds=bounds;
            measuredScale=scale;
        }
    }
    const auto [width,height]=NativeBridge::screenMetrics.read();
    if(fitToGame && width && height) {
        auto fit=DesktopGeometry::FitViewport({bounds.origin.x,bounds.origin.y,bounds.size.width,bounds.size.height},width,height);
        bounds=CGRectMake(fit.x,fit.y,fit.width,fit.height);
        static CGRect previous=CGRectNull;
        if(!CGRectEqualToRect(bounds,previous)) {
            previous=bounds;
            Log(@"pointer_viewport container=(%.0f,%.0f,%.0f,%.0f) render=(%d,%d) viewport=(%.0f,%.0f,%.0f,%.0f)",
                rect.origin.x,CGRectGetMaxY(primary)-CGRectGetMaxY(rect),rect.size.width,rect.size.height,
                width,height,bounds.origin.x,bounds.origin.y,bounds.size.width,bounds.size.height);
        }
    }
    return true;
}
static CGPoint DesktopMouseLocation(void) {
    CGPoint point=((CGPoint (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSEvent"),sel_registerName("mouseLocation"));
    NSArray *screens=((id (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSScreen"),sel_registerName("screens"));
    CGRect primary=((CGRect (*)(id,SEL))objc_msgSend)(screens.firstObject,sel_registerName("frame"));
    point.y=CGRectGetMaxY(primary)-point.y;
    return point;
}
static int WarpDesktopPointer(CGPoint point) {
    if(!warpPointer) return -1;
    // Batched map packets do not update the relative-motion history. If a
    // menu closes before that frame, seed the real pre-warp location now so
    // the first camera delta cannot include a stale map position.
    if(desktopMapMotionBatched) {
        CGPoint current=DesktopMouseLocation();
        desktopMotion.reset({current.x,current.y});desktopMapMotionBatched=false;
    }
    int result=warpPointer(point);
    if(!result) desktopMotion.didWarp({point.x,point.y});
    return result;
}
static void SetGuardedCursor(id cursor, SEL selector) {
    if (NSThread.isMainThread && ownedNativeCursor && !NativeBridge::macWindowSuspended.load()) {
        cursor = ownedNativeCursor;
    }
    originalCursorSet(cursor, selector);
}
static void InstallNativeCursorGuard(void) {
    Class cursorClass=NSClassFromString(@"NSCursor");
    Method method=class_getInstanceMethod(cursorClass,sel_registerName("set"));
    if(!method || originalCursorSet) return;
    UIGraphicsImageRendererFormat *format=[UIGraphicsImageRendererFormat defaultFormat];
    format.scale=1;format.opaque=NO;
    UIGraphicsImageRenderer *renderer=[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(8,8) format:format];
    NSData *png=[renderer PNGDataWithActions:^(UIGraphicsImageRendererContext *context) {
        CGContextClearRect(context.CGContext,CGRectMake(0,0,8,8));
    }];
    id nativeImage=((id (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSImage"),sel_registerName("alloc"));
    nativeImage=((id (*)(id,SEL,id))objc_msgSend)(nativeImage,sel_registerName("initWithData:"),png);
    id cursor=((id (*)(id,SEL))objc_msgSend)(cursorClass,sel_registerName("alloc"));
    transparentNativeCursor=((id (*)(id,SEL,id,CGPoint))objc_msgSend)(cursor,sel_registerName("initWithImage:hotSpot:"),nativeImage,CGPointZero);
    if(!transparentNativeCursor) return;
    originalCursorSet=(void (*)(id,SEL))method_setImplementation(method,(IMP)SetGuardedCursor);
    loginNativeCursor = MakeDesktopNativeCursor(LoginArrowPNG(), CGSizeMake(32,32), CGPointMake(2,2));
    Log(@"cursor_presentation backend=NSCursor login=%d", loginNativeCursor != nil);
}
static void PlaceConfinedPointer(CGPoint point,const CGRect &bounds,bool syncPhysical=true) {
    // Keep clicks away from the titlebar, window borders and adjacent apps.
    point.x=std::clamp(point.x,CGRectGetMinX(bounds)+3.0,CGRectGetMaxX(bounds)-3.0);
    point.y=std::clamp(point.y,CGRectGetMinY(bounds)+3.0,CGRectGetMaxY(bounds)-3.0);
    capturedPointer=point;
    if(syncPhysical) WarpDesktopPointer(point);
    NativeBridge::Position((point.x-bounds.origin.x)/bounds.size.width,
        1.0-(point.y-bounds.origin.y)/bounds.size.height);
    UpdateCursorImage();
}
static void ReleasePointerCapture(void) {
    if(!nativePointerCaptured) return;
    int result=associatePointer?associatePointer(true):-1;
    nativePointerCaptured=false;
    desktopDragSequence.reset();
    desktopMotionSequence.reset();
    desktopTouchHistory.reset();
    if(capturedNativeWindow) {
        if (capturedCursorRects) MacPointerSend<void>(capturedNativeWindow, "enableCursorRects");
        capturedCursorRects = false;
        ((void (*)(id,SEL,BOOL))objc_msgSend)(capturedNativeWindow,sel_registerName("setAcceptsMouseMovedEvents:"),previousMouseMovedEvents);
        capturedNativeWindow=nil;
    }
    CGPoint point=DesktopMouseLocation();desktopMotion.reset({point.x,point.y});
    Log(@"pointer_capture enabled=0 result=%d",result);
}
static void ReleaseFreePointerTracking(void) {
    if(!freeNativeWindow) return;
    ((void (*)(id,SEL,BOOL))objc_msgSend)(freeNativeWindow,sel_registerName("setAcceptsMouseMovedEvents:"),previousFreeMouseMovedEvents);
    freeNativeWindow=nil;
}
static DesktopMapPointerFrame desktopMapFrame;
static __weak id desktopMapFrameWindow;
// All three mouse edges use the location AppKit captured for that event.
static CGPoint GameMouseEventPoint(id window,id event) {
    CGPoint local=((CGPoint (*)(id,SEL))objc_msgSend)(event,sel_registerName("locationInWindow"));
    CGPoint screen=((CGPoint (*)(id,SEL,CGPoint))objc_msgSend)(window,sel_registerName("convertPointToScreen:"),local);
    NSArray *screens=((id (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSScreen"),sel_registerName("screens"));
    CGRect primary=((CGRect (*)(id,SEL))objc_msgSend)(screens.firstObject,sel_registerName("frame"));
    return CGPointMake(screen.x,CGRectGetMaxY(primary)-screen.y);
}
static bool DefersMapPointerPacket(void) {
    return desktopMapFrame.ready() && NativeBridge::mapDragActive &&
        !NativeBridge::nativeOverlayActive && !pointerCaptureSuspended;
}
static bool PrepareMapPointerFrame(id window) {
    if(desktopMapFrame.ready()&&desktopMapFrameWindow==window)return true;
    CGRect bounds;
    if(!GameContentBounds(bounds))return false;
    desktopMapFrameWindow=window;
    return desktopMapFrame.publish(bounds.origin.x,bounds.origin.y,bounds.size.width,bounds.size.height,
        NativeBridge::mapFocusRevision.load());
}
static void SyncFreeDesktopPointer(const CGPoint *eventLocation=nullptr) {
    if(DefersMapPointerPacket() || (!eventLocation && desktopNativePressInContent &&
        NativeBridge::mapPageVisible.load())) return;
    if(ControllerOwnsGamePointer()) return;
    if(!gameplayPointerStarted || pointerCaptureSuspended || !WantsVisibleCursor() || !NativeBridge::IsForeground()) return;
    CGRect bounds;if(!GameContentBounds(bounds)) return;
    capturedPointer=eventLocation?*eventLocation:DesktopMouseLocation();
    if(desktopMapMotionBatched) {
        desktopMotion.reset({capturedPointer.x,capturedPointer.y});desktopMapMotionBatched=false;
    }
    NativeBridge::Position((capturedPointer.x-bounds.origin.x)/bounds.size.width,
        1.0-(capturedPointer.y-bounds.origin.y)/bounds.size.height);
    // Use the same viewport for hit testing and drawing this packet.
    UpdateCursorImage(&bounds, &capturedPointer);
}
static void ReconcilePointerCapture(void) {
    CGRect bounds;
    bool webFocus=NativeBridge::nativeOverlayActive.load()&&DesktopWebAppIsActive();
    bool manage=(webFocus||(!ControllerOwnsGamePointer() && gameplayPointerStarted && !pointerCaptureSuspended &&
        NativeBridge::IsForeground())) && GameContentBounds(bounds);
    if(!NativeGameWindow() || !manage) {ReleasePointerCapture();ReleaseFreePointerTracking();return;}
    if(webFocus || WantsVisibleCursor()) {
        ReleasePointerCapture();
        id window=NativeGameWindow();
        if(window!=freeNativeWindow) {
            ReleaseFreePointerTracking();freeNativeWindow=window;
            previousFreeMouseMovedEvents=((BOOL (*)(id,SEL))objc_msgSend)(window,sel_registerName("acceptsMouseMovedEvents"));
            ((void (*)(id,SEL,BOOL))objc_msgSend)(window,sel_registerName("setAcceptsMouseMovedEvents:"),YES);
            Log(@"pointer_mode visible_free=1 system_absolute_position=1");
        }
        if(webFocus) {
            if(!((BOOL (*)(id,SEL))objc_msgSend)(window,sel_registerName("acceptsMouseMovedEvents")))
                ((void (*)(id,SEL,BOOL))objc_msgSend)(window,sel_registerName("setAcceptsMouseMovedEvents:"),YES);
        }
        SyncFreeDesktopPointer();return;
    }
    ReleaseFreePointerTracking();
    if(!associatePointer || !warpPointer) return;
    if(!nativePointerCaptured) {
        if (waitForContent && !CGRectContainsPoint(bounds, DesktopMouseLocation())) return;
        waitForContent = false;
        int result=associatePointer(false);
        if(result) {Log(@"pointer_capture failed=%d",result);return;}
        nativePointerCaptured=true;
        capturedNativeWindow=NativeGameWindow();
        previousMouseMovedEvents=((BOOL (*)(id,SEL))objc_msgSend)(capturedNativeWindow,sel_registerName("acceptsMouseMovedEvents"));
        ((void (*)(id,SEL,BOOL))objc_msgSend)(capturedNativeWindow,sel_registerName("setAcceptsMouseMovedEvents:"),YES);
        capturedCursorRects = MacPointerSend<BOOL>(capturedNativeWindow, "areCursorRectsEnabled");
        if (capturedCursorRects) MacPointerSend<void>(capturedNativeWindow, "disableCursorRects");
        CGPoint point=DesktopMouseLocation();desktopMotion.reset({point.x,point.y});
        PlaceConfinedPointer(CGPointMake(CGRectGetMidX(bounds),CGRectGetMidY(bounds)),bounds);
        Log(@"pointer_capture enabled=1 result=0 bounds=(%.0f,%.0f,%.0f,%.0f)",
            bounds.origin.x,bounds.origin.y,bounds.size.width,bounds.size.height);
    } else {
        // Accommodate window resizing and display moves without releasing capture.
        CGPoint point=capturedPointer;
        if(point.x<CGRectGetMinX(bounds)+3 || point.x>CGRectGetMaxX(bounds)-3 ||
           point.y<CGRectGetMinY(bounds)+3 || point.y>CGRectGetMaxY(bounds)-3)
            PlaceConfinedPointer(point,bounds);
    }
}
static bool WantsVisibleCursor(void) {
    if(NativeBridge::nativeOverlayActive) return true;
    if(teleportCursorPolicy.hidden) return false;
    // Keep the shortcut wheel usable during the initial layout transition too.
    return cursorVisible.load() || tabCursorRequested || NativeBridge::IsKeyHeld(9);
}
static void CenterDesktopPointer(void) {
    NativeBridge::CenterPointer();
    if(!NativeBridge::IsForeground()) return;
    CGRect bounds;if(!GameContentBounds(bounds)) return;
    capturedPointer=CGPointMake(CGRectGetMidX(bounds),CGRectGetMidY(bounds));
    Log(@"cursor_centered result=%d",WarpDesktopPointer(capturedPointer));
}
static void SetOwnedCursor(id desired) {
    if (!originalCursorSet) return;
    id previous = ownedNativeCursor;
    ownedNativeCursor = desired;
    if (desired) ApplyDesktopNativeCursor(desired, originalCursorSet, previous != desired);
    else if (previous)
        originalCursorSet(MacPointerSend(NSClassFromString(@"NSCursor"), "arrowCursor"), sel_registerName("set"));
    if (previous != desired)
        Log(@"cursor_owner shape=%s", !desired ? "system" : desired == transparentNativeCursor ? "hidden" : "game");
}

static void UpdateCursorImage(const CGRect *eventBounds, const CGPoint *eventPoint) {
    if (!NSThread.isMainThread || !originalCursorSet) return;
    if (!NativeGameWindow() || ControllerOwnsGamePointer() ||
        (pointerCaptureSuspended && !NativeBridge::nativeOverlayActive)) {
        SetOwnedCursor(nil);
        return;
    }
    if (nativePointerCaptured && !WantsVisibleCursor()) {
        SetOwnedCursor(transparentNativeCursor);
        return;
    }
    CGRect bounds;
    if (eventBounds) bounds = *eventBounds;
    else if (!GameContentBounds(bounds)) { SetOwnedCursor(nil); return; }
    CGPoint point = eventPoint ? *eventPoint : DesktopMouseLocation();
    bool visible = !gameplayPointerStarted || WantsVisibleCursor();
    SetOwnedCursor(visible && CGRectContainsPoint(bounds, point) ? (gameNativeCursor ?: loginNativeCursor) : nil);
}

static void ApplyNativeCursorVisibility(bool forceVisible=false) {
    bool hidden = !forceVisible && NativeGameWindow() && !ControllerOwnsGamePointer() &&
        gameplayPointerStarted && !pointerCaptureSuspended && NativeBridge::IsForeground() && !WantsVisibleCursor();
    if (hidden && !cameraCursorHidden && nativePointerCaptured) CenterDesktopPointer();
    cameraCursorHidden = hidden;
    if (forceVisible) SetOwnedCursor(nil);
    else UpdateCursorImage();
}

static void RefreshPointerStateNow(void) {
    ReconcilePointerCapture();
    ApplyNativeCursorVisibility();
    for (UIPointerInteraction *interaction in desktopPointerInteractions) [interaction invalidate];
    for (UIWindow *window in UIApplication.sharedApplication.windows) {
        [window.rootViewController setNeedsUpdateOfPrefersPointerLocked];
        for(UIViewController *child in window.rootViewController.childViewControllers)
            [child setNeedsUpdateOfPrefersPointerLocked];
    }

}
static void RefreshPointerLock(void) {
    if(NSThread.isMainThread) RefreshPointerStateNow();
    else dispatch_async(dispatch_get_main_queue(), ^{RefreshPointerStateNow();});
}

static void SetMacPointerHost(id window) {
    bool suspended = window == nil;
    if (window == gameHostWindow && NativeBridge::macWindowSuspended.load() == suspended) return;
    NativeBridge::macWindowSuspended = suspended;
    gameHostWindow = window;
    waitForContent = true;
    desktopNativePressInContent = false;
    tabCursorRequested = false;
    desktopEscapeGuard.reset();
    desktopNoticeEscapeOwned = false;
    desktopMapFrame.cancel();
    NativeBridge::Deactivate();
    NativeBridge::handoffMouseBlocked.fetch_or(static_cast<uint32_t>(
        MacPointerSend<NSUInteger>(NSClassFromString(@"NSEvent"), "pressedMouseButtons")));
    ReleasePointerCapture();
    ReleaseFreePointerTracking();
    desktopDragSequence.reset();
    desktopMotionSequence.reset();
    desktopTouchHistory.reset();
    CGPoint point = DesktopMouseLocation();
    desktopMotion.reset({point.x,point.y});
    RefreshPointerStateNow();
    Log(@"cursor_host game=%d window=%@", !suspended, NSStringFromClass([window class]));
}

static void RefreshMacPointerHost(id eventWindow) {
    id app = PointerApplication();
    id key = MacPointerSend(app, "keyWindow");
    bool eligible = !menuTrackingDepth && MacPointerSend<BOOL>(app, "isActive") &&
        !MacPointerSend(app, "modalWindow") && IsCatalystGameWindow(key);
    if (eligible && eventWindow && eventWindow != key) eligible = false;
    SetMacPointerHost(eligible ? key : nil);
}

static void InstallMacPointerHost() {
    static bool installed;
    if (installed) return;
    installed = true;
    InstallNativeCursorGuard();
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    for (NSString *name in @[@"NSMenuDidBeginTrackingNotification", @"NSMenuDidEndTrackingNotification",
        @"NSWindowDidBecomeKeyNotification", @"NSWindowDidResignKeyNotification",
        @"NSWindowWillBeginSheetNotification", @"NSWindowDidEndSheetNotification",
        @"NSApplicationDidBecomeActiveNotification", @"NSApplicationWillResignActiveNotification"]) {
        [center addObserverForName:name object:nil queue:nil usingBlock:^(NSNotification *note) {
            if ([note.name isEqualToString:@"NSMenuDidBeginTrackingNotification"]) {
                ++menuTrackingDepth;
                SetMacPointerHost(nil);
            } else if ([note.name isEqualToString:@"NSMenuDidEndTrackingNotification"]) {
                if (menuTrackingDepth) --menuTrackingDepth;
                dispatch_async(dispatch_get_main_queue(), ^{ RefreshMacPointerHost(); });
            } else if ([note.name isEqualToString:@"NSApplicationWillResignActiveNotification"] ||
                (note.object == gameHostWindow && ([note.name isEqualToString:@"NSWindowDidResignKeyNotification"] ||
                                                   [note.name isEqualToString:@"NSWindowWillBeginSheetNotification"]))) {
                SetMacPointerHost(nil);
            } else RefreshMacPointerHost();
        }];
    }
    RefreshMacPointerHost();
}
static void SetDesktopTabHeld(bool held) {
    auto update=^{
        if(held && (pointerCaptureSuspended || !NativeBridge::IsForeground())) return;
        if(tabCursorRequested==held) return;
        tabCursorRequested=held;RefreshPointerStateNow();
        CGRect bounds={};bool located=GameContentBounds(bounds);
        Log(@"cursor_tab held=%d visible=%d capture=%d native_arrow=%d normalized=(%.3f,%.3f)",
            held,WantsVisibleCursor(),nativePointerCaptured,ownedNativeCursor && ownedNativeCursor != transparentNativeCursor,
            located?(capturedPointer.x-bounds.origin.x)/bounds.size.width:0,
            located?(capturedPointer.y-bounds.origin.y)/bounds.size.height:0);
    };
    if(NSThread.isMainThread) update();else dispatch_async(dispatch_get_main_queue(),update);
}
static uint32_t DesktopScrollContext(void) {
    if(ControllerOwnsGamePointer() || pointerCaptureSuspended || !NativeBridge::IsForeground() || NativeBridge::nativeOverlayActive) return 0;
    // Cursor mode changes synchronously with the UI. A delayed map scan must
    // not cancel the beginning of an otherwise continuous zoom gesture.
    return WantsVisibleCursor()?2:1;
}
static uint32_t DesktopScrollZoomContext(void) {
    // Reuse the existing map visibility cache, with no new scene scans.
    if(ControllerOwnsGamePointer() || NativeBridge::nativeOverlayActive || !gameplayPointerStarted) return 0;
    if(!WantsVisibleCursor() && cursorLockMode.load()==1) return 2;
    if(WantsVisibleCursor() && NativeBridge::friendPageVisible.load()) return 5;
    if(WantsVisibleCursor() && NativeBridge::settingsPageVisible.load()) return 4;
    if(WantsVisibleCursor() && NativeBridge::paimonPageVisible.load()) return 3;
    return NativeBridge::mapPageVisible.load()?1:0;
}
#include "native_web_scroll.h"
static void ProcessDesktopScroll(id event,const DesktopScrollDispatchScope::Key &key) {
    if(!NativeBridge::enabled || !desktopScrollSequence.first(event)) return;
    uint32_t target=DesktopScrollContext();if(!target) return;
    id game=NativeGameWindow();
    id window=((id (*)(id,SEL))objc_msgSend)(event,sel_registerName("window"));
    if(!game || (window && window!=game)) return;
    CGRect bounds;if(!GameContentBounds(bounds)) return;
    CGPoint point=DesktopMouseLocation();
    static auto cgLocation=(CGPoint (*)(CFTypeRef))dlsym(RTLD_DEFAULT,"CGEventGetLocation");
    CFTypeRef cg=((CFTypeRef (*)(id,SEL))objc_msgSend)(event,sel_registerName("CGEvent"));
    if(cg && cgLocation) point=cgLocation(cg);
    if(!CGRectContainsPoint(bounds,point)) return;
    bool mos=DesktopScrollIsMos(event);
    // Resolve this menu only while scrolling visible game UI. No additional
    // scene search runs during camera gameplay, ordinary frames or web pages.
    if(target==2) {
        NativeBridge::friendPageVisible=NativeBridge::friendScrollContext&&NativeBridge::friendScrollContext();
        NativeBridge::settingsPageVisible=NativeBridge::settingsScrollContext&&NativeBridge::settingsScrollContext();
        NativeBridge::paimonPageVisible=!NativeBridge::settingsPageVisible.load()&&
            NativeBridge::paimonScrollContext&&NativeBridge::paimonScrollContext();
    }
    static auto cgInteger=(int64_t (*)(CFTypeRef,uint32_t))dlsym(RTLD_DEFAULT,"CGEventGetIntegerValueField");
    // Read the original line fields. Do not rewrite or repost the event.
    double linesX=cg&&cgInteger?(double)cgInteger(cg,12):0;
    double linesY=cg&&cgInteger?(double)cgInteger(cg,11):0;
    NativeBridge::SystemScroll(key,target,mos,linesX,linesY);
    static unsigned logged=0;
    if((key.x || key.y) && logged++<6)
        Log(@"system_scroll_packet mos=%d precise=%d context=%u delta=(%.4f,%.4f) lines=(%.0f,%.0f) phase=%u momentum=%u",
            mos,key.precise,target,key.x,key.y,linesX,linesY,key.phase,key.momentum);
}
static bool DispatchOwnedMapMotion(id event,NSUInteger type);
static id ProcessDesktopMouseMotion(id event) {
    NSUInteger type=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("type"));
    if(DispatchOwnedMapMotion(event,type))return event;
    if(ControllerOwnsGamePointer() || pointerCaptureSuspended || !NativeBridge::IsForeground() || !NativeGameWindow()) return event;
    // The same event can cross NSApplication, NSWindow and local monitors.
    // Forward its native system delta once, including held-button movement.
    if(!desktopMotionSequence.firstDelivery(event)) return event;
    id window=((id (*)(id,SEL))objc_msgSend)(event,sel_registerName("window"));
    CGPoint location=type==6 && window && !nativePointerCaptured ?
        GameMouseEventPoint(window,event) : DesktopMouseLocation();
    double dx=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("deltaX"));
    double dy=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("deltaY"));
    auto delta=desktopMotion.consume({dx,dy},{location.x,location.y});
    desktopMapMotionBatched=false;
    if(!pointerCaptureSuspended && NativeBridge::IsForeground() && NativeGameWindow()) {
        if(diagnosticsEnabled) {
        static NSUInteger previousType=0;static double began=0,rawX=0,rawY=0,outX=0,outY=0;
        static uint64_t samples=0;
        double now=NSProcessInfo.processInfo.systemUptime;
        if(type!=previousType || now-began>=1) {
            auto gcX=rawMotionX.exchange(0),gcY=rawMotionY.exchange(0);
            if(samples) Log(@"motion_diagnostics type=%lu samples=%llu event_abs=(%.1f,%.1f) output_abs=(%.1f,%.1f) gc_raw_abs=(%.1f,%.1f)",
                (unsigned long)previousType,samples,rawX,rawY,outX,outY,gcX/1000.0,gcY/1000.0);
            previousType=type;began=now;rawX=rawY=outX=outY=0;samples=0;
        }
        rawX+=fabs(dx);rawY+=fabs(dy);outX+=fabs(delta.x);outY+=fabs(delta.y);++samples;
        static unsigned sourceSamples[32]={};
        if(type<32 && sourceSamples[type]++<3) {
            static auto cgDouble=(double (*)(CFTypeRef,uint32_t))dlsym(RTLD_DEFAULT,"CGEventGetDoubleValueField");
            CFTypeRef cg=((CFTypeRef (*)(id,SEL))objc_msgSend)(event,sel_registerName("CGEvent"));
            if(cg && cgDouble) Log(@"motion_source type=%lu NSEvent=(%.3f,%.3f) CGEvent=(%.3f,%.3f)",
                (unsigned long)type,dx,dy,cgDouble(cg,4),cgDouble(cg,5));
        }
        }
        if(!NativeBridge::nativeOverlayActive) NativeBridge::Move(delta.x,-delta.y);
        // SDK web windows still resolve their own pointer and viewport.
        if(NativeBridge::nativeOverlayActive) SyncFreeDesktopPointer();
        else SyncFreeDesktopPointer(&location);
        static uint64_t events=0;
        if(++events<=4 || events%1200==0)
            Log(@"system_mouse_motion events=%llu delta=(%.3f,%.3f) capture=%d visible=%d",events,
                delta.x,delta.y,nativePointerCaptured,WantsVisibleCursor());
    }
    return event;
}
static void ObservePrimaryEvent(id window,id event,NSUInteger type) {
    if((type!=1 && type!=2) || !desktopButtonSequence.firstDelivery(event)) return;
    bool down=type==1;
    if (NativeAccountInput::Refresh(down)) {
        desktopNativePressInContent=false;
        return;
    }
    bool game=window && window==NativeGameWindow() && !pointerCaptureSuspended &&
        DesktopWebAppIsActive() && NativeBridge::IsForeground() && !NativeBridge::nativeOverlayActive;
    bool inside=false;
    DesktopPrimaryButton::Point point{};
    if(game) {
        CGRect content=NativeGameContentRect(window);
        CGPoint local=((CGPoint (*)(id,SEL))objc_msgSend)(event,sel_registerName("locationInWindow"));
        const auto [w,h]=NativeBridge::screenMetrics.read();
        auto rect=DesktopGeometry::FitViewport({content.origin.x,content.origin.y,content.size.width,content.size.height},w,h);
        if(rect.width>0 && rect.height>0) {
            float u=(local.x-rect.x)/rect.width,v=(local.y-rect.y)/rect.height;
            inside=u>=0 && u<1 && v>=0 && v<1;
            point={u,v,!nativePointerCaptured && WantsVisibleCursor()};
        }
    }
    NSUInteger flags=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("modifierFlags"));
    bool eligible=inside && !(flags&(1ULL<<20));
    if(down) {
        desktopMapFrame.cancel();
        desktopNativePressInContent=eligible;
    } else if(game && point.valid && desktopNativePressInContent) {
        // Pin the final event before the OS moves the free cursor again.
        desktopMapFrame.cancel();
        CGPoint location=GameMouseEventPoint(window,event);
        SyncFreeDesktopPointer(&location);
    }
    if(!down && !game && desktopNativePressInContent) NativeBridge::CancelDesktopPointer();
    NativeBridge::PrimaryEvent(down,eligible,point);
}
static void DispatchDesktopWindowEvent(id window,SEL selector,id event) {
    NSUInteger type=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("type"));
    id target = window;
    if (target != gameHostWindow || NativeBridge::macWindowSuspended.load()) RefreshMacPointerHost(target);
    if (!NativeGameWindow() || (target && target != gameHostWindow)) {
        originalWindowSendEvent(window, selector, event);
        return;
    }
    if (type == 5 || type == 8 || type == 9 || type == 17) {
        if (!gameplayPointerStarted || type != 5) UpdateCursorImage();
    }
    ObservePrimaryEvent(window,event,type);
    if (NativeBridge::accountSheetActive.load()) {
        originalWindowSendEvent(window,selector,event);
        return;
    }
    if(type==22) {
        auto key=DesktopScrollKey(event);DesktopScrollDispatchScope delivery(key);
        if(delivery.first) ProcessDesktopScroll(event,key);
        if(DesktopWebWheel::Handle(window,event,key))return;
        originalWindowSendEvent(window,selector,event);return;
    }
    bool drag=type==6 || type==7 || type==27;
    bool release=type==2 || type==4 || type==26;
    if((drag || release) && !NativeBridge::nativeOverlayActive && !pointerCaptureSuspended && NativeBridge::IsForeground() && window==NativeGameWindow()) {
        static uint64_t delivered=0,duplicates=0;
        if(!desktopDragSequence.firstDelivery(event) && WantsVisibleCursor()) {
            if(++duplicates<=3) Log(@"drag_window_duplicate skipped=%llu",duplicates);
            return;
        }
        if(drag) event=ProcessDesktopMouseMotion(event);
        else if(type!=2) SyncFreeDesktopPointer();
        if(++delivered<=3 || delivered%1200==0)
            Log(@"drag_window_dispatch delivered=%llu duplicates=%llu",delivered,duplicates);
        // Absolute position/button state already drives the native Unity UI
        // and camera. Catalyst's mouseDragged translation spins a nested run
        // loop per packet. Keep window chrome/web views on their OS path.
        if(type==6&&desktopNativePressInContent&&NativeBridge::nativePointerGestures.load()&&
           NativeBridge::nativeDesktopMouseUI.load()) return;
    }
    if(type==2) {desktopNativePressInContent=false;desktopMapFrame.cancel();}
    originalWindowSendEvent(window,selector,event);
}
static bool DispatchOwnedMapMotion(id event,NSUInteger type) {
    // Unity has already claimed this real left drag. Window dispatch already
    // suppresses Catalyst translation for it; finish at application dispatch
    // instead of repeating scene/window queries and device detection at the
    // mouse's report rate. Down/up, hover, web, chrome and mode changes retain
    // the original route. No synthetic event or touch is produced.
    if(type!=6 || !NativeBridge::mapDragActive.load() || NativeBridge::nativeOverlayActive.load() ||
       pointerCaptureSuspended || !desktopNativePressInContent ||
       !NativeBridge::nativePointerGestures.load() || !NativeBridge::nativeDesktopMouseUI.load() ||
       ControllerOwnsGamePointer() || nativePointerCaptured || !WantsVisibleCursor() ||
       desktopMotion.warped || !NativeBridge::canBatchMapMotion || !NativeBridge::observeBatchedMapMotion ||
       !NativeBridge::canBatchMapMotion())return false;
    id window=((id (*)(id,SEL))objc_msgSend)(event,sel_registerName("window"));
    NSUInteger flags=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("modifierFlags"));
    if(!window || window!=NativeGameWindow() || !DesktopWebAppIsActive() || (flags&(1ULL<<20)))return false;
    double dx=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("deltaX"));
    double dy=((CGFloat (*)(id,SEL))objc_msgSend)(event,sel_registerName("deltaY"));
    if(!std::isfinite(dx)||!std::isfinite(dy)||!PrepareMapPointerFrame(window))return false;
    if(!desktopMotionSequence.firstDelivery(event))return true;
    NativeBridge::observeBatchedMapMotion(dx,dy);
    auto frame=desktopMapFrame.snapshot();
    CGPoint point=GameMouseEventPoint(window,event);
    double u,v;
    if(frame.position(point.x,point.y,u,v) &&
       desktopMapFrame.record(frame,NativeBridge::mapFocusRevision.load(),point.x,point.y)) {
        NativeBridge::Position(u,v);
    }
    NativeBridge::Move(dx,-dy);desktopMapMotionBatched=true;
    return true;
}
static void DispatchDesktopApplicationEvent(id application,SEL selector,id event) {
    NSUInteger type=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("type"));
    id target = MacPointerSend(event, "window");
    if (target != gameHostWindow || NativeBridge::macWindowSuspended.load()) RefreshMacPointerHost(target);
    if (!NativeGameWindow() || (target && target != gameHostWindow)) {
        originalApplicationSendEvent(application, selector, event);
        return;
    }
    if ((type==1 || type==2) && TitleQuit::Handle(target,event,type==1)) return;
    if (type == 5 || type == 8 || type == 9 || type == 17) {
        if (!gameplayPointerStarted || type != 5) UpdateCursorImage();
    }
    if(type==1 || type==2)
        ObservePrimaryEvent(((id (*)(id,SEL))objc_msgSend)(event,sel_registerName("window")),event,type);
    if ((type==10 || type==11) ? NativeAccountInput::Refresh() : NativeBridge::accountSheetActive.load()) {
        originalApplicationSendEvent(application,selector,event);
        return;
    }
    if(DispatchOwnedMapMotion(event,type))return;
    // Publish ordinary motion before the device observer checks window and
    // geometry. The observer queues mode changes and does not consume input.
    const bool motionFirst=type==5 && !NativeBridge::nativeOverlayActive.load();
    if(motionFirst) ProcessDesktopMouseMotion(event);
    if(type==1||type==3||type==25||type==10||type==12)DesktopWebWheel::Cancel();
    if(type==1||type==3||type==25)NativeBridge::CancelCoarseWheel();
    if(type==10 || type==11) {
        unsigned short code=((unsigned short (*)(id,SEL))objc_msgSend)(event,sel_registerName("keyCode"));
        NativeBridge::SystemKey(code,type==10);
    }
    if(NativeBridge::observeDesktopEvent) NativeBridge::observeDesktopEvent(event);
    if(type==22) {
        auto key=DesktopScrollKey(event);DesktopScrollDispatchScope delivery(key);
        if(delivery.first) ProcessDesktopScroll(event,key);
        originalApplicationSendEvent(application,selector,event);return;
    }
    if(!motionFirst && (type==5 || type==6 || type==7 || type==27)) ProcessDesktopMouseMotion(event);
    originalApplicationSendEvent(application,selector,event);
}
static void InstallSystemMouseMotion(void) {
    InstallMacPointerHost();
    // Mac Catalyst's immediate dragging route can bypass local monitors. Read
    // each drag at NSWindow's public dispatch entry, before UIKit sees it.
    // Mouse movement without a button retains the user-confirmed monitor path.
    if(!originalWindowSendEvent) {
        Method send=class_getInstanceMethod(NSClassFromString(@"NSWindow"),sel_registerName("sendEvent:"));
        if(send) originalWindowSendEvent=(void (*)(id,SEL,id))method_setImplementation(send,(IMP)DispatchDesktopWindowEvent);
        Log(@"drag_window_bridge installed=%d",originalWindowSendEvent!=nullptr);
    }
    if(!originalApplicationSendEvent) {
        Method send=class_getInstanceMethod(NSClassFromString(@"NSApplication"),sel_registerName("sendEvent:"));
        if(send) originalApplicationSendEvent=(void (*)(id,SEL,id))method_setImplementation(send,(IMP)DispatchDesktopApplicationEvent);
        Log(@"drag_application_bridge installed=%d",originalApplicationSendEvent!=nullptr);
    }
    if(originalWindowSendEvent || originalApplicationSendEvent) NativeBridge::EnablePrimaryEvents();
    InstallGameKeyAlertGuard();
    NativeBridge::scrollContextReader=DesktopScrollContext;
    NativeBridge::scrollZoomReader=DesktopScrollZoomContext;
    NativeBridge::systemScrollActive=originalApplicationSendEvent || originalWindowSendEvent || desktopMouseMonitor;
    Class eventClass=NSClassFromString(@"NSEvent");
    SEL add=sel_registerName("addLocalMonitorForEventsMatchingMask:handler:");
    if(desktopMouseMonitor || ![eventClass respondsToSelector:add]) return;
    CGPoint point=DesktopMouseLocation();desktopMotion.reset({point.x,point.y});
    uint64_t mask=(1ULL<<5)|(1ULL<<6)|(1ULL<<7)|(1ULL<<27)|(1ULL<<10)|(1ULL<<11)|(1ULL<<22);
    id (^handler)(id)=^id(id event) {
        NSUInteger type=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("type"));
        if (NativeBridge::accountSheetActive.load()) return event;
        if (!NativeGameWindow()) return event;
        if(type==22) {
            if(!originalApplicationSendEvent && !originalWindowSendEvent) ProcessDesktopScroll(event,DesktopScrollKey(event));
            return event;
        }
        if(type==10 || type==11) {
            unsigned short code=((unsigned short (*)(id,SEL))objc_msgSend)(event,sel_registerName("keyCode"));
            if(code==48) SetDesktopTabHeld(type==10);
            if(code==53) {
                id window=((id (*)(id,SEL))objc_msgSend)(event,sel_registerName("window"));
                if(!window) window=NativeGameWindow();
                NSUInteger style=window?((NSUInteger (*)(id,SEL))objc_msgSend)(window,sel_registerName("styleMask")):0;
                NSUInteger flags=((NSUInteger (*)(id,SEL))objc_msgSend)(event,sel_registerName("modifierFlags"));
                bool fullscreen=(style&(1ULL<<14))!=0; // NSWindowStyleMaskFullScreen
                bool gameWindow=window && window==NativeGameWindow();
                bool repeat=desktopEscapeGuard.ownsPress;
                auto decision=desktopEscapeGuard.event((unsigned)type,code,flags,gameWindow,NativeBridge::IsForeground());
                if(decision.consume) {
                    if(decision.pressed && !repeat && NativeBridge::nativeOverlayActive)
                        desktopNoticeEscapeOwned=CloseDesktopNoticeOnEscape(NativeBridge::SuppressNoticeEscape);
                    if(desktopNoticeEscapeOwned) {
                        if(!decision.pressed) desktopNoticeEscapeOwned=false;
                        Log(@"notice_escape pressed=%d game_key_suppressed=1",decision.pressed);
                        return nil;
                    }
                    NativeBridge::QueueKey(27,decision.pressed);
                    Log(@"escape_game_guard pressed=%d fullscreen=%d forwarded_to_game=1",decision.pressed,fullscreen);
                    return nil;
                }
            }
            return event;
        }
        return ProcessDesktopMouseMotion(event);
    };
    desktopMouseMonitor=((id (*)(id,SEL,uint64_t,id))objc_msgSend)(eventClass,add,mask,handler);
    NativeBridge::systemScrollActive=originalApplicationSendEvent || originalWindowSendEvent || desktopMouseMonitor;
    Log(@"system_scroll_bridge installed=%d source=NSEvent precise_points_per_unit=%.0f frame_synced=1",
        NativeBridge::systemScrollActive.load(),DesktopScrollState::precisePointsPerUnit);
    Log(@"system_mouse_motion installed=%d source=NSEvent speed_multiplier=1",desktopMouseMonitor!=nil);
}
static BOOL PrefersDesktopPointerLock(id self,SEL selector) {
    return !ControllerOwnsGamePointer() && !nativePointerCaptured && !pointerCaptureSuspended && cursorLockMode.load()==1 && !WantsVisibleCursor() && NativeBridge::IsForeground();
}
static bool CursorVisible(void) {return cursorVisible.load();}
static int CursorLockMode(void) {return cursorLockMode.load();}
static void SetCursorVisible(bool visible) {
    if(visible) ++cursorShowRevision;
    bool old=cursorVisible.exchange(visible);
    if (old!=visible) {Log(@"cursor_request visible=%d",visible);RefreshPointerLock();}
}
static void SetCursorLockMode(int mode) {
    if(mode==1) gameplayPointerStarted=true;
    int old=cursorLockMode.exchange(mode);
    if (old!=mode) {Log(@"cursor_request lock_mode=%d",mode);RefreshPointerLock();}
}
static void SetCursorTexture(void *texture,const float *hotspot,int mode,float unused) {
    static void *previousTexture;
    CGPoint point=CGPointMake(hotspot?hotspot[0]:0,hotspot?hotspot[1]:0);
    if(!texture) {
        previousTexture=nullptr;
        dispatch_async(dispatch_get_main_queue(), ^{
            gameNativeCursor=nil;gameCursorPNG=nil;RefreshPointerStateNow();
        });
        Log(@"cursor_texture cleared");return;
    }
    if(texture==previousTexture) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (gameCursorPNG && !CGPointEqualToPoint(gameCursorHotspot, point)) {
                gameCursorHotspot = point;
                ConfigureDesktopWebStyle(gameCursorPNG, gameCursorSize, point);
                gameNativeCursor = desktopWebGoldCursor;
                UpdateCursorImage();
            }
        });
        return;
    }
    auto findClass=EngineFunction<void *(*)(const char *,const char *,const char *)>(0x10f057bd4);
    auto lookup=EngineFunction<void *(*)(void *,const char *,int,int)>(0x10f0572e8);
    auto invoke=EngineFunction<void *(*)(void *,void *,void **,void **)>(0x10e9bb768);
    auto className=EngineFunction<const char *(*)(void *)>(0x10e9bb600);
    if(strcmp(className(*(void **)texture),"Texture2D")) {Log(@"cursor_texture unexpected_type");return;}
    void *cls=findClass("UnityEngine.ImageConversionModule.dll","UnityEngine","ImageConversion");
    void *method=cls?lookup(cls,"EncodeToPNG",1,0):nullptr;
    if(!method) {Log(@"cursor_texture missing_encoder");return;}
    void *args[]={texture},*exception=nullptr;
    void *array=invoke(method,nullptr,args,&exception);
    if(!array || exception) {Log(@"cursor_texture encode_failed exception=%p",exception);return;}
    uint64_t size=*(uint64_t *)((char *)array+24);
    if(!size || size>4*1024*1024) {Log(@"cursor_texture invalid_size=%llu",size);return;}
    NSData *png=[NSData dataWithBytes:(char *)array+32 length:size];
    previousTexture=texture;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIImage *img=[UIImage imageWithData:png];
        if(!img) {Log(@"cursor_texture image_decode_failed");return;}
        gameCursorPNG=png;gameCursorSize=img.size;gameCursorHotspot=point;
        ConfigureDesktopWebStyle(png,img.size,point);
        gameNativeCursor = desktopWebGoldCursor;
        [png writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/GenshinDesktopCursor.png"] atomically:YES];
        Log(@"cursor_texture installed bytes=%llu hotspot=(%.1f,%.1f)",size,point.x,point.y);
        RefreshPointerStateNow();
    });
}
static void InstallCursorBridge(void) {
    if (!IsCompatibleRuntime()) return;
    InstallNativeCursorGuard();
    InstallSystemMouseMotion();
    struct Patch {uintptr_t va;const char *expected;void *replacement;};
    const Patch patches[]={
        {0x10f7f8830,"\xff\x03\x01\xd1\xe9\x23\x01\x6d\xf4\x4f\x02\xa9\xfd\x7b\x03\xa9",(void *)SetCursorTexture},
        {0x10f7f8884,"\xfd\x7b\xbf\xa9\xfd\x03\x00\x91\xe2\x36\xd9\x97\x08\x00\x40\xf9",(void *)CursorVisible},
        {0x10f7f88a4,"\xf4\x4f\xbe\xa9\xfd\x7b\x01\xa9\xfd\x43\x00\x91\xf3\x03\x00\xaa",(void *)SetCursorVisible},
        {0x10f7f88d4,"\xfd\x7b\xbf\xa9\xfd\x03\x00\x91\xce\x36\xd9\x97\x08\x00\x40\xf9",(void *)CursorLockMode},
        {0x10f7f88f0,"\xf4\x4f\xbe\xa9\xfd\x7b\x01\xa9\xfd\x43\x00\x91\xf3\x03\x00\xaa",(void *)SetCursorLockMode}
    };
    for (auto p:patches) if (!RuntimePrologueMatches(p.va,EngineFunction<void *>(p.va))) {
        Log(@"cursor_bridge refused_unexpected_code");return;
    }
    for (auto p:patches) {
        uint8_t jump[16];NativeBridge::AbsoluteJump(jump,(uintptr_t)p.replacement);
        if (!NativeBridge::PatchCode(EngineFunction<void *>(p.va),jump,16)) return;
    }
    Class cls=NSClassFromString(@"UnityViewControllerBase");
    Method method=class_getInstanceMethod(UIViewController.class,@selector(prefersPointerLocked));
    if (cls && method) class_replaceMethod(cls,@selector(prefersPointerLocked),(IMP)PrefersDesktopPointerLock,method_getTypeEncoding(method));
    NSNotificationCenter *nc=NSNotificationCenter.defaultCenter;
    for(NSString *name in @[UIApplicationDidBecomeActiveNotification,@"NSApplicationDidBecomeActiveNotification"])
        [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
            pointerCaptureSuspended=false;RefreshPointerLock();
        }];
    for(NSString *name in @[UIApplicationWillResignActiveNotification,@"NSApplicationWillResignActiveNotification",@"NSApplicationWillTerminateNotification"])
        [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
            pointerCaptureSuspended=true;tabCursorRequested=false;desktopMapFrame.cancel();
            desktopEscapeGuard.reset();desktopNoticeEscapeOwned=false;
            ReleasePointerCapture();ReleaseFreePointerTracking();ApplyNativeCursorVisibility(true);
        }];
    [nc addObserverForName:UIPointerLockStateDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
        for (UIWindow *window in UIApplication.sharedApplication.windows)
            Log(@"cursor_actual locked=%d",window.windowScene.pointerLockState.locked);
    }];
    // UIKit can defer the pointer refresh until the next click after a menu
    // closes. Reconcile the requested state without requiring another click.
    [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *) {
        static bool previousVisible=true;
        NativeBridge::RefreshScreenMetrics();
        static unsigned tick=0;if(++tick%2==0) UpdateNativeOverlayState();
        bool visible=WantsVisibleCursor();
        ReconcilePointerCapture();
        if(visible!=previousVisible) {previousVisible=visible;RefreshPointerStateNow();}
        ApplyNativeCursorVisibility();
        if(!ControllerOwnsGamePointer() && !nativePointerCaptured && !pointerCaptureSuspended && cursorLockMode.load()==1 && !visible && NativeBridge::IsForeground())
            for(UIWindow *window in UIApplication.sharedApplication.windows)
                if(!window.windowScene.pointerLockState.locked)
                    [window.rootViewController setNeedsUpdateOfPrefersPointerLocked];
    }];
    Log(@"cursor_bridge installed controller=%d",cls!=Nil);
}

@interface DesktopPointerObserver : NSObject <UIGestureRecognizerDelegate,UIPointerInteractionDelegate>
- (void)hover:(UIHoverGestureRecognizer *)recognizer;
@end
@implementation DesktopPointerObserver
- (void)hover:(UIHoverGestureRecognizer *)recognizer {
    if(ControllerOwnsGamePointer()) return;
    if(!NativeGameWindow() || nativePointerCaptured) return;
    if(gameplayPointerStarted && WantsVisibleCursor()) {SyncFreeDesktopPointer();return;}
    UIView *view=recognizer.view;
    if (!view || view.bounds.size.width<=0 || view.bounds.size.height<=0) return;
    CGPoint point=[recognizer locationInView:view];
    NativeBridge::Position(point.x/view.bounds.size.width,1.0-point.y/view.bounds.size.height);
    UpdateCursorImage();
}
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)first shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)second {
    return YES;
}
- (UIPointerStyle *)pointerInteraction:(UIPointerInteraction *)interaction styleForRegion:(UIPointerRegion *)region {
    if(ControllerOwnsGamePointer()) return nil;
    return NativeGameWindow() && nativePointerCaptured && !WantsVisibleCursor() ? [UIPointerStyle hiddenPointerStyle] : nil;
}
@end

static UIView *FindUnityView(UIView *view) {
    if ([view isKindOfClass:NSClassFromString(@"UnityView")]) return view;
    for (UIView *child in view.subviews) {UIView *found=FindUnityView(child);if (found) return found;}
    return nil;
}

static void UpdateNativeOverlayState(void) {
    NativeAccountInput::Refresh();
    desktopWebStyleDiagnostic=[](bool gold) {Log(@"native_web_style_applied gold=%d",gold);};
    static bool styleHooks=false;
    if(!styleHooks && (styleHooks=InstallDesktopWebStyleHooks())) Log(@"native_web_style_hooks installed=1");
    NSMutableArray *webs=[NSMutableArray array];
    if(DesktopWebAppIsActive()) {
        for(UIWindow *window in UIApplication.sharedApplication.windows) {
            unsigned budget=256;
            if(!window.hidden && window.alpha>=0.02) CollectDesktopWebViews(window,budget,false,webs);
        }
        {
            id app=((id (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSApplication"),sel_registerName("sharedApplication"));
            NSArray *windows=((id (*)(id,SEL))objc_msgSend)(app,sel_registerName("windows"));
            for(id window in windows) {
                if(!((BOOL (*)(id,SEL))objc_msgSend)(window,sel_registerName("isVisible"))) continue;
                id root=((id (*)(id,SEL))objc_msgSend)(window,sel_registerName("contentView"));
                unsigned budget=256;CollectDesktopWebViews(root,budget,true,webs);
            }
        }
    }
    bool appearing=webs.count>0 && !NativeBridge::nativeOverlayActive;
    if(appearing) for(id web in webs) DescribeDesktopWebEscapeHost(web);
    for(id web in webs)
        if(PrepareDesktopWebStyle(web,appearing)) Log(@"native_web_style scheduled text_selection=0 class=%@",NSStringFromClass([web class]));
    bool visible=webs.count>0,before=NativeBridge::nativeOverlayActive.exchange(visible);
    if(visible!=before) NativeBridge::CancelDesktopPointer();
    DesktopWebWheel::Views(webs);
    if(!visible) NativeBridge::nativeNoticeEscapeActive=false;
    else if(appearing || !NativeBridge::nativeNoticeEscapeActive)
        NativeBridge::nativeNoticeEscapeActive=(bool)DesktopWebEscapeAction();
    if(visible!=before) {
        Log(@"native_web_gold_cursor visible=%d",visible);
        Log(@"native_web_focus ui_state=%ld mac_active=%d capture_suspended=%d",(long)UIApplication.sharedApplication.applicationState,DesktopWebAppIsActive(),pointerCaptureSuspended);
        RefreshPointerStateNow();
        if(visible) Log(@"notice_escape available=%d",NativeBridge::nativeNoticeEscapeActive.load());
        if(visible && desktopWebGoldCursor && originalCursorSet)
            SetGuardedCursor(desktopWebGoldCursor,sel_registerName("set"));
    }
}

using UnityTouchCallback=void (*)(id,SEL,NSSet<UITouch *> *,UIEvent *);
static UnityTouchCallback originalUnityTouchesBegan,originalUnityTouchesMoved,originalUnityTouchesEnded,originalUnityTouchesCancelled;
static CGPoint (*originalTouchLocation)(id,SEL,UIView *);
static CGPoint (*originalTouchPreviousLocation)(id,SEL,UIView *);
struct UnityTouchCoordinates {
    __unsafe_unretained UITouch *touch;
    __unsafe_unretained UIView *view;
    CGPoint current,previous;
    unsigned currentReads=0,previousReads=0;
};
static thread_local UnityTouchCoordinates *unityTouchCoordinates;
static CGPoint SharedTouchLocation(UITouch *touch,SEL selector,UIView *view) {
    if(unityTouchCoordinates && unityTouchCoordinates->touch==touch) {
        ++unityTouchCoordinates->currentReads;
        return [unityTouchCoordinates->view convertPoint:unityTouchCoordinates->current toView:view];
    }
    return originalTouchLocation(touch,selector,view);
}
static CGPoint SharedTouchPreviousLocation(UITouch *touch,SEL selector,UIView *view) {
    if(unityTouchCoordinates && unityTouchCoordinates->touch==touch) {
        ++unityTouchCoordinates->previousReads;
        return [unityTouchCoordinates->view convertPoint:unityTouchCoordinates->previous toView:view];
    }
    return originalTouchPreviousLocation(touch,selector,view);
}
static void DeliverUnityMouseTouch(UIView *view,SEL selector,NSSet<UITouch *> *touches,UIEvent *event,
                                  UITouchPhase phase,UnityTouchCallback original) {
    bool began=phase==UITouchPhaseBegan;
    bool accountTouch=began && NativeAccountInput::Refresh();
    bool game=began && !accountTouch && !NativeBridge::nativeOverlayActive && !pointerCaptureSuspended && NativeBridge::IsForeground();
    bool claim=game && NativeBridge::nativeDesktopMouseUI &&
        (NativeBridge::nativePointerGestures || nativePointerCaptured);
    if(game && !claim && NativeBridge::mapTouchOwnership) claim=NativeBridge::mapTouchOwnership(true);
    NSMutableSet<UITouch *> *remaining=nil;
    for(UITouch *touch in touches) {
        // An indirect pointer starting in gamepad mode can request a handoff.
        // Its UIKit stream must not also start an attack in the old layout.
        bool switchClick=game && NativeBridge::controllerInputActive && DesktopControlPreference::gameplayReady &&
            touch.type==UITouchTypeIndirectPointer;
        if(desktopMapTouchStreams.suppress((__bridge const void *)touch,(int)phase,accountTouch||claim||switchClick)) {
            if(!remaining) remaining=[touches mutableCopy];
            [remaining removeObject:touch];
        }
    }
    if(remaining && !remaining.count) return;
    if(remaining) touches=remaining;
    if(NativeBridge::nativeDesktopMouseUI.load()) {
        // Forward the rest of a stream whose Began was delivered to Unity,
        // including its terminal phase after a capture or ownership change.
        original(view,selector,touches,event);
        return;
    }
    UITouch *touch=touches.count==1?touches.anyObject:nil;
    bool visible=nativePointerCaptured && !pointerCaptureSuspended && WantsVisibleCursor() && NativeBridge::IsForeground();
    NSUInteger buttons=((NSUInteger (*)(id,SEL))objc_msgSend)(NSClassFromString(@"NSEvent"),sel_registerName("pressedMouseButtons"));
    bool primary=(buttons&1) || (event.buttonMask&UIEventButtonMaskPrimary);
    CGRect bounds;
    bool havePosition=visible && GameContentBounds(bounds);
    const void *identity=(__bridge const void *)touch;
    bool tracked=touch && desktopTouchHistory.identity==identity;
    CGPoint point=CGPointMake(desktopTouchHistory.current.x,desktopTouchHistory.current.y);
    if(havePosition) point=CGPointMake(
        view.bounds.origin.x+(capturedPointer.x-bounds.origin.x)/bounds.size.width*view.bounds.size.width,
        view.bounds.origin.y+(capturedPointer.y-bounds.origin.y)/bounds.size.height*view.bounds.size.height);
    if(touch && havePosition && primary && phase==UITouchPhaseBegan) {
        desktopTouchHistory.begin(identity,{point.x,point.y});tracked=true;
    } else if(tracked) desktopTouchHistory.update(identity,{point.x,point.y});
    if(!tracked || !originalTouchLocation || !originalTouchPreviousLocation) {
        original(view,selector,touches,event);return;
    }
    CGPoint incoming=originalTouchLocation(touch,@selector(locationInView:),view);
    UnityTouchCoordinates coordinates={touch,view,point,
        CGPointMake(desktopTouchHistory.previous.x,desktopTouchHistory.previous.y)};
    UnityTouchCoordinates *saved=unityTouchCoordinates;
    unityTouchCoordinates=&coordinates;
    // Only the game callback sees the coherent mouse coordinates. UIKit keeps
    // its original touch identity, phase and timing, and no mouse event is
    // replaced or posted. All unrelated touches and other views pass through.
    @try {original(view,selector,touches,event);}
    @finally {unityTouchCoordinates=saved;}
    static uint64_t moved=0;
    if(phase!=UITouchPhaseMoved || ++moved<=4 || moved%120==0)
        Log(@"unity_touch_bridge phase=%ld touch=%p incoming=(%.2f,%.2f) forwarded=(%.2f,%.2f) delta=(%.2f,%.2f) reads=(%u,%u)",
            (long)phase,identity,incoming.x,incoming.y,point.x,point.y,
            point.x-coordinates.previous.x,point.y-coordinates.previous.y,coordinates.currentReads,coordinates.previousReads);
    if(phase==UITouchPhaseEnded || phase==UITouchPhaseCancelled) desktopTouchHistory.reset();
}
static void SharedUnityTouchesBegan(UIView *v,SEL s,NSSet<UITouch *> *t,UIEvent *e) {
    DeliverUnityMouseTouch(v,s,t,e,UITouchPhaseBegan,originalUnityTouchesBegan);
}
static void SharedUnityTouchesMoved(UIView *v,SEL s,NSSet<UITouch *> *t,UIEvent *e) {
    DeliverUnityMouseTouch(v,s,t,e,UITouchPhaseMoved,originalUnityTouchesMoved);
}
static void SharedUnityTouchesEnded(UIView *v,SEL s,NSSet<UITouch *> *t,UIEvent *e) {
    DeliverUnityMouseTouch(v,s,t,e,UITouchPhaseEnded,originalUnityTouchesEnded);
}
static void SharedUnityTouchesCancelled(UIView *v,SEL s,NSSet<UITouch *> *t,UIEvent *e) {
    DeliverUnityMouseTouch(v,s,t,e,UITouchPhaseCancelled,originalUnityTouchesCancelled);
}
static void InstallUnityTouchCoordinates(void) {
    if(originalUnityTouchesMoved) return;
    Class viewClass=NSClassFromString(@"UnityView");
    struct Hook {SEL selector;IMP replacement;UnityTouchCallback *original;};
    Hook hooks[]={
        {@selector(touchesBegan:withEvent:),(IMP)SharedUnityTouchesBegan,&originalUnityTouchesBegan},
        {@selector(touchesMoved:withEvent:),(IMP)SharedUnityTouchesMoved,&originalUnityTouchesMoved},
        {@selector(touchesEnded:withEvent:),(IMP)SharedUnityTouchesEnded,&originalUnityTouchesEnded},
        {@selector(touchesCancelled:withEvent:),(IMP)SharedUnityTouchesCancelled,&originalUnityTouchesCancelled}
    };
    Method location=class_getInstanceMethod(UITouch.class,@selector(locationInView:));
    Method previous=class_getInstanceMethod(UITouch.class,@selector(previousLocationInView:));
    if(!location || !previous) return;
    for(auto hook:hooks) if(!class_getInstanceMethod(viewClass,hook.selector)) return;
    originalTouchLocation=(CGPoint (*)(id,SEL,UIView *))method_setImplementation(location,(IMP)SharedTouchLocation);
    originalTouchPreviousLocation=(CGPoint (*)(id,SEL,UIView *))method_setImplementation(previous,(IMP)SharedTouchPreviousLocation);
    for(auto hook:hooks) *hook.original=(UnityTouchCallback)method_setImplementation(class_getInstanceMethod(viewClass,hook.selector),hook.replacement);
    Log(@"unity_touch_bridge installed=1 scoped_to_game_callback=1");
}

static void InstallPointerPosition(void) {
    if (!IsCompatibleRuntime()) return;
    auto findClass=EngineFunction<void *(*)(const char *,const char *,const char *)>(0x10f057bd4);
    auto lookup=EngineFunction<void *(*)(void *,const char *,int,int)>(0x10f0572e8);
    void *screen=findClass("UnityEngine.CoreModule.dll","UnityEngine","Screen");
    if (!screen) return;
    NativeBridge::screenWidthMethod=lookup(screen,"get_width",0,0);
    NativeBridge::screenHeightMethod=lookup(screen,"get_height",0,0);
    if (!NativeBridge::screenWidthMethod || !NativeBridge::screenHeightMethod) return;
    NativeBridge::screenWidth=(int (*)(void *))ManagedMethodCode(NativeBridge::screenWidthMethod);
    NativeBridge::screenHeight=(int (*)(void *))ManagedMethodCode(NativeBridge::screenHeightMethod);
    if(!NativeBridge::screenWidth || !NativeBridge::screenHeight) {
        Log(@"pointer_position refused_screen_method_abi");return;
    }
    NativeBridge::RefreshScreenMetrics();
    InstallUnityTouchCoordinates();
    static DesktopPointerObserver *observer;
    observer=[DesktopPointerObserver new];
    desktopPointerInteractions=[NSMutableArray array];
    for (UIWindow *window in UIApplication.sharedApplication.windows) {
        UIView *view=FindUnityView(window);if (!view) continue;
        UIHoverGestureRecognizer *hover=[[UIHoverGestureRecognizer alloc] initWithTarget:observer action:@selector(hover:)];
        hover.delegate=observer;hover.cancelsTouchesInView=NO;
        [view addGestureRecognizer:hover];
        UIPointerInteraction *pointer=[[UIPointerInteraction alloc] initWithDelegate:observer];
        [view addInteraction:pointer];[desktopPointerInteractions addObject:pointer];
        const auto [w,h]=NativeBridge::screenMetrics.read();
        Log(@"pointer_position installed width=%d height=%d",w,h);
    }
}

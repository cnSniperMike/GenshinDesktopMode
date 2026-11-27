#pragma once

// Resolve the original button and ask the active EventSystem which graphic
// receives this event. Account sheets, game dialogs and other buttons keep
// their own input, even when they cover the title screen.
namespace TitleQuit {
using namespace SceneUIProbe;
static constexpr const char *buttonPath="/Canvas/Pages/LoginMainPage/GrpLogin/Bottom/LeftButtons/BtnQuit_PC";

static int ButtonAt(float x,float y) {
    static void *gameObject=findClass("UnityEngine.CoreModule.dll","UnityEngine","GameObject");
    static void *find=gameObject?lookup(gameObject,"Find",1,0):nullptr;
    if (!find) return 0;
    auto stringFromUTF8=EngineFunction<void *(*)(const char *)>(0x10e9bb794);
    void *pathArgs[]={stringFromUTF8(buttonPath)};
    void *button=Call(find,nullptr,pathArgs);
    if (!button || NativeAccountInput::Visible()) return 0;
    static void *systemClass=findClass("UnityEngine.UI.dll","UnityEngine.EventSystems","EventSystem");
    static void *getSystem=systemClass?lookup(systemClass,"get_current",0,0):nullptr;
    void *system=Call(getSystem,nullptr);
    void *module=Property(system,"get_currentInputModule");
    if (!module) return 0;
    int leftMouse=-1;
    void *pointerArgs[]={&leftMouse};
    void *pointer=Call(lookup(*(void **)module,"GetLastPointerEventData",1,0),module,pointerArgs);
    if (!pointer) {
        char *owner=NativePC::InputOwner();
        void *configured=owner?*(void **)(owner+GenshinOffsets().ownerModule):nullptr;
        if (configured && configured==module) pointer=NativePC::MousePointer(configured);
    }
    // Raycast the event location, even for a short click with no intervening
    // game frame. Leave the input module's live position and hit list alone.
    static void *objectClass=findClass("mscorlib.dll","System","Object");
    static void *clone=objectClass?lookup(objectClass,"MemberwiseClone",0,0):nullptr;
    void *probe=pointer?Call(clone,pointer):nullptr;
    if (!probe) return 0;
    struct { float x,y; } position{x,y};
    void *posArgs[]={&position};
    if (!ControlDevice::VoidCall(lookup(*(void **)probe,"set_position",1,0),probe,posArgs)) return 0;
    static void *raycast=systemClass?lookup(systemClass,"RaycastAll",2,0):nullptr;
    static void *activator=findClass("mscorlib.dll","System","Activator");
    static void *create=TeleportCursorBridge::OneTypeArgument(activator,"CreateInstance");
    if (!raycast || !create) return 0;
    auto parameter=EngineFunction<void *(*)(void *,unsigned)>(0x10e9bb704);
    auto typeObject=EngineFunction<void *(*)(void *)>(0x10e9bb7b8);
    void *listArgs[]={typeObject(parameter(raycast,1))};
    void *hits=Call(create,nullptr,listArgs);
    if (!hits) return 0;
    void *rayArgs[]={probe,hits};
    if (!ControlDevice::VoidCall(raycast,system,rayArgs)) return 0;
    void *count=Property(hits,"get_Count");
    if (!count || *(int *)((char *)count+16)<=0) return 0;
    int first=0;
    void *indexArgs[]={&first};
    void *hit=Call(lookup(*(void **)hits,"get_Item",1,0),hits,indexArgs);
    void *graphic=ControlDevice::PointerField(hit,"m_GameObject",16,"GameObject");
    void *transform=Property(graphic,"get_transform"), *target=Property(button,"get_transform");
    for (unsigned level=0;transform && level<16;++level) {
        if (transform==target) {
            void *identity=Property(button,"GetInstanceID");
            return identity?*(int *)((char *)identity+16):0;
        }
        transform=Property(transform,"get_parent");
    }
    return 0;
}

static void Confirm(id window) {
    // Keep the confirmation attached to the game window. AppKit performs
    // the normal quit after the user confirms.
    id alert=[NSClassFromString(@"NSAlert") new];
    if (!alert) return;
    NSString *language=NSLocale.preferredLanguages.firstObject;
    BOOL chinese=[language hasPrefix:@"zh"] || [language hasPrefix:@"yue"];
    MacPointerSend<void>(alert,"setMessageText:",chinese?@"退出游戏？":@"Quit the game?");
    MacPointerSend(alert,"addButtonWithTitle:",chinese?@"退出游戏":@"Quit Game");
    id cancel=MacPointerSend(alert,"addButtonWithTitle:",chinese?@"取消":@"Cancel");
    MacPointerSend<void>(cancel,"setKeyEquivalent:",@"\033");
    MacPointerSend<void>(alert,"beginSheetModalForWindow:completionHandler:",window,^(NSInteger response) {
        if (response==1000) MacPointerSend<void>(PointerApplication(),"terminate:",nil);
    });
}

static bool Handle(id window,id event,bool down) {
    static int pressedButton;
    static uint64_t focusAtPress;
    const int previous=pressedButton;
    pressedButton=0;
    if ((!down && !previous) || !NSThread.isMainThread || !NativeBridge::frames.load() ||
        !window || window!=NativeGameWindow() || !NativeBridge::IsForeground() ||
        NativeBridge::nativeOverlayActive || ControllerOwnsGamePointer() || nativePointerCaptured ||
        MacPointerSend(PointerApplication(),"modalWindow") || MacPointerSend(window,"attachedSheet")) return false;
    CGRect content=NativeGameContentRect(window);
    const auto [w,h]=NativeBridge::screenMetrics.read();
    if (!w || !h) return false;
    const auto rect=DesktopGeometry::FitViewport({content.origin.x,content.origin.y,content.size.width,content.size.height},w,h);
    if (rect.width<=0 || rect.height<=0) return false;
    CGPoint local=MacPointerSend<CGPoint>(event,"locationInWindow");
    const double u=(local.x-rect.x)/rect.width,v=(local.y-rect.y)/rect.height;
    // A cheap rejection avoids managed queries during ordinary gameplay.
    const int hit=u>=0 && u<.5 && v>=0 && v<.5 ? ButtonAt(u*w,v*h) : 0;
    const auto revision=NativeBridge::mapFocusRevision.load();
    if (down && hit) {
        pressedButton=hit;
        focusAtPress=revision;
        NativeBridge::handoffMouseBlocked.fetch_or(1);
        return true;
    }
    if (!down && previous) {
        if (previous==hit && revision==focusAtPress) {
            Log(@"title_quit confirmed_target=BtnQuit_PC");
            Confirm(window);
        }
        return true;
    }
    return false;
}
}

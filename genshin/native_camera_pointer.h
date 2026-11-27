#include "map_flick.h"
#include "map_mouse_gesture.h"
#include "map_projection.h"

namespace NativeCameraPointer {
using namespace SceneUIProbe;
struct Vector2 {float x,y;};
using DesktopMapProjection::Vector3;
static void (*originalUpdate)(void *,void *),(*originalDisable)(void *,void *);
static void (*swipe)(void *,Vector2,void *),(*speed)(void *,Vector3,void *);
static void *swipeMethod,*speedMethod,*gotoMethod,*dialogsMethod,*clickedGetter,*clickInvoke;
static bool installed;
static void *trackedTouch,*trackedCamera;
static DesktopMapMouseGesture drag;
static std::atomic<bool> mapActive{false};
static DesktopMapFlick mapFlick;

static void *Method(void *cls,const char *name,const char *result,
                    std::initializer_list<const char *> parameters) {
    if(!cls)return nullptr;
    void *method=lookup(cls,name,(int)parameters.size(),0);
    if(!method || !ManagedMethodCode(method)) return nullptr;
    auto from=EngineFunction<void *(*)(void *)>(0x10e9bb5e0);
    auto returns=EngineFunction<void *(*)(void *)>(0x10e9bb6e8);
    auto parameter=EngineFunction<void *(*)(void *,unsigned)>(0x10e9bb704);
    if(strcmp(className(from(returns(method))),result)) return nullptr;
    unsigned index=0;
    for(const char *expected:parameters)
        if(strcmp(className(from(parameter(method,index++))),expected)) return nullptr;
    return method;
}

// The native camera publishes its pivot and visible world extents to the map
// renderer. Hold a map coordinate in that space for the whole mouse gesture.
namespace Anchor {
static void *cameraGetter,*cameraRect,*getPivot,*getPosition,*getWidth,*getHeight,*moveCamera,*updateCamera;
static bool cameraField,ready,usingAnchor;
static DesktopMapProjection::Viewport view;
static Vector3 mapPoint;
static Vector2 followPoint;
static unsigned samples,limited;

static Vector3 read(void *method,void *object) {
    return ((Vector3 (*)(void *,void *))ManagedMethodCode(method))(object,method);
}
static void Prepare(void *map) {
    auto cam=findClass("UnityEngine.CoreModule.dll","UnityEngine","Camera");
    cameraField=HasManagedField(map,"_camera",32,"Camera");
    cameraGetter=Method(map,"get_projectiveUICamera","Camera",{});
    cameraRect=Method(cam,"get_rect","Rect",{});
    getPivot=Method(map,"GetCameraPivot","Vector3",{});
    getPosition=Method(map,"GetCameraPosition","Vector3",{});
    getWidth=Method(map,"GetFrustumWidth","Single",{"Vector3"});
    getHeight=Method(map,"GetFrustumHeight","Single",{"Vector3"});
    moveCamera=Method(map,"MoveCamera","Void",{"Vector3"});
    updateCamera=Method(map,"UpdateCameraPosition","Void",{});
    ready=(cameraField || cameraGetter) && cameraRect && getPivot && getPosition &&
        getWidth && getHeight && moveCamera && updateCamera;
}
static DesktopMapProjection::FlatMap Frustum(void *map) {
    auto pos=read(getPosition,map);
    auto extent=[&](void *method) {
        return ((float (*)(void *,Vector3,void *))ManagedMethodCode(method))(map,pos,method);
    };
    return {view,extent(getWidth),extent(getHeight)};
}
static void Begin(void *map,Vector2 press) {
    usingAnchor=false;samples=limited=0;
    if(!ready)return;
    void *projection=cameraField?*(void **)((char *)map+32):nullptr;
    if(!projection && cameraGetter)projection=Call(cameraGetter,map);
    if(!projection)return;
    using Rect=DesktopMapProjection::Rect;
    auto rect=((Rect (*)(void *,void *))ManagedMethodCode(cameraRect))(projection,cameraRect);
    view={rect,float(drag.viewport.width),float(drag.viewport.height)};
    auto f=Frustum(map);
    if(!f.valid())return;
    mapPoint=f.at(press.x,press.y,read(getPivot,map));
    usingAnchor=true;
}
static void Move(void *map,Vector3 delta) {
    ((void (*)(void *,Vector3,void *))ManagedMethodCode(moveCamera))(map,delta,moveCamera);
}
static void Follow(void *map,Vector2 pixel) {
    followPoint=pixel;
    auto f=Frustum(map);
    if(!f.valid())return;
    Vector3 pivot=read(getPivot,map),under=f.at(pixel.x,pixel.y,pivot);
    Vector3 delta{mapPoint.x-under.x,mapPoint.y-under.y,0};
    if(delta.x==0 && delta.y==0)return;
    Move(map,delta);
    Vector3 after=read(getPivot,map);
    // At a native boundary a diagonal can be refused even when one axis is
    // still reachable. Pan that axis and rebase the grab at the boundary.
    if(after.x==pivot.x && after.y==pivot.y && delta.x!=0 && delta.y!=0) {
        Move(map,{delta.x,0,0});Move(map,{0,delta.y,0});
        after=read(getPivot,map);
    }
    ((void (*)(void *,void *))ManagedMethodCode(updateCamera))(map,updateCamera);
    if(std::fabs(pivot.x+delta.x-after.x)>.001f || std::fabs(pivot.y+delta.y-after.y)>.001f) {
        ++limited;mapPoint=f.at(pixel.x,pixel.y,after);
    } else ++samples;
}
static void Finish() {
    if(diagnosticsEnabled && usingAnchor && (samples||limited))
        Log(@"native_map_anchor samples=%u boundary_samples=%u plane=map_frustum",samples,limited);
    usingAnchor=false;
}
}

static bool RegisterOnlyPrologue(const void *code) {
    const uint32_t *p=(const uint32_t *)code;
    for(unsigned i=0;i<4;++i) {
        uint32_t op=p[i];
        bool storePair=(op&0x3b400000u)==0x29000000u && ((op>>5)&31)==31;
        bool stackSubtract=(op&0xffc003ffu)==0xd10003ffu;
        bool framePointer=(op&0xffc003ffu)==0x910003fdu;
        bool moveRegister=(op&0xffe0ffe0u)==0xaa0003e0u;
        if(!storePair&&!stackSubtract&&!framePointer&&!moveRegister) return false;
    }
    return true;
}
static void *Trampoline(void *target) {
    if(!RegisterOnlyPrologue(target)) return nullptr;
    void *code=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(code==MAP_FAILED) return nullptr;
    memcpy(code,target,16);
    NativeBridge::AbsoluteJump((char *)code+16,(uintptr_t)target+16);
    sys_icache_invalidate(code,32);
    if(mprotect(code,4096,PROT_READ|PROT_EXEC)) {munmap(code,4096);return nullptr;}
    return code;
}
static void Goto(void *camera,int mode) {
    void *args[]={&mode,nullptr};Call(gotoMethod,camera,args);
}
static void Action(void *touch,size_t offset) {
    void *action=*(void **)((char *)touch+offset);
    if(action) Call(lookup(*(void **)action,"Invoke",0,0),action);
}
static void FinishCameraPan(void *camera) {
    speed(camera,Vector3{},speedMethod);
    Goto(camera,2);
    Anchor::Finish();
}

// Keep the same camera mode and pan coordinates through mouse-up. The
// physical drag ends immediately; boundary recovery starts after the coast.
static void CoastFrame(void *camera,int height,double now) {
    auto delta=mapFlick.advance(height,now);
    if (delta.x || delta.y) {
        if (Anchor::usingAnchor)
            Anchor::Follow(camera,{Anchor::followPoint.x+delta.x,Anchor::followPoint.y+delta.y});
        else swipe(camera,{delta.x,delta.y},swipeMethod);
    }
    if (!mapFlick.active()) FinishCameraPan(camera);
}
static void StopTracking(void *touch,void *camera) {
    const bool wasDragging=drag.cancel();
    const bool wasCoasting=mapFlick.active();
    mapFlick.cancel();
    if (camera && (wasDragging || wasCoasting)) {
        FinishCameraPan(camera);
        if (wasDragging) Action(touch,80);
    }
    Anchor::usingAnchor=false;
    NativeBridge::mapDragActive=false;
}
static bool HasWidget(void *pointer) {
    void *hit=Property(pointer,"get_pointerEnter");
    static void *componentType=TeleportCursorBridge::TypeObject(
        "UnityEngine.CoreModule.dll","UnityEngine","Component");
    for(unsigned depth=0;hit&&depth<16;++depth) {
        void *method=TeleportCursorBridge::OneTypeArgument(*(void **)hit,"GetComponents");
        void *args[]={componentType},*array=Call(method,hit,args);
        uint64_t count=array?*(uint64_t *)((char *)array+24):0;
        if(count>100) return true;
        for(uint64_t i=0;i<count;++i) {
            void *component=((void **)((char *)array+32))[i];if(!component) continue;
            void *cls=*(void **)component;
            auto parent=EngineFunction<void *(*)(void *)>(0x10e9bb608);
            for(unsigned level=0;cls&&level<8;cls=parent(cls),++level) {
                const char *name=className(cls);
                if(!strcmp(name,"Selectable")||!strcmp(name,"ScrollRect")||
                   !strcmp(name,"MonoMapMark")||!strcmp(name,"MonoMapMarkTips")||
                   !strcmp(name,"MonoMapMarkTipsPanel")) return true;
            }
        }
        void *transform=Property(hit,"get_transform");
        transform=Property(transform,"get_parent");hit=Property(transform,"get_gameObject");
    }
    return false;
}
static bool MapSurface(void *touch,bool requirePointer) {
    // Resolve the native UI raycast only on a new physical press. Buttons,
    // sliders and lists keep their own PointerEventData drag handlers.
    void *blocked=Call(dialogsMethod,touch);
    if(blocked&&*(bool *)((char *)blocked+16)) return false;
    char *owner=NativePC::InputOwner();
    void *module=owner?*(void **)(owner+GenshinOffsets().ownerModule):nullptr;
    void *pointer=module?NativePC::MousePointer(module):nullptr;
    return pointer?!HasWidget(pointer):!requirePointer;
}
static void Click(void *touch,Vector2 point) {
    if(!clickedGetter || !clickInvoke || !MapSurface(touch,true)) return;
    void *event=Call(clickedGetter,touch);
    if(!event) return;
    // MonoExUITouch's original tap sends BaseFinger.position (screen pixels)
    // to this event. The map page owns projection, selection and the editor.
    void *exception=nullptr,*args[]={&point};
    invoke(clickInvoke,event,args,&exception);
    Log(@"native_map_click dispatched=%d point=(%.1f,%.1f)",!exception,point.x,point.y);
}
static int MapContext(void) {return installed?(mapActive?1:0):-1;}
static void Update(void *object,void *method) {
    originalUpdate(object,method);
    char *touch=(char *)object;
    char *context=*(char **)(touch+40);
    static void *lastContextClass;static size_t monoOffset;
    void *contextClass=context?*(void **)context:nullptr;
    if(contextClass!=lastContextClass) {
        lastContextClass=contextClass;monoOffset=0;
        // The map context is identified by its typed view field, never by
        // a game version, region, translated title or obfuscated class name.
        if(contextClass) {
            auto next=EngineFunction<void *(*)(void *,void **)>(0x10e9bb5ec);
            auto flags=EngineFunction<unsigned (*)(void *)>(0x10e9bb694);
            auto offset=EngineFunction<unsigned (*)(void *)>(0x10e9bb69c);
            auto from=EngineFunction<void *(*)(void *)>(0x10e9bb5e0);
            void *it=nullptr;
            while(void *field=next(contextClass,&it))
                if(!(flags(field)&0x10)&&!strcmp(className(from(ManagedFieldType(field))),"MonoInLevelMapPage"))
                    monoOffset=offset(field);
        }
    }
    void *camera=*(void **)(touch+56);
    if(!monoOffset) {
        if(object==trackedTouch) {
            StopTracking(object,camera==trackedCamera?camera:nullptr);
            trackedTouch=trackedCamera=nullptr;
            mapActive=false;
        }
        return;
    }
    void *mono=*(void **)(context+monoOffset);
    static void *monoClass,*activeMethod;
    static bool (*isActive)(void *,void *);
    if(mono&&*(void **)mono!=monoClass) {
        monoClass=*(void **)mono;
        activeMethod=Method(monoClass,"get_isActiveAndEnabled","Boolean",{});
        isActive=activeMethod?(decltype(isActive))ManagedMethodCode(activeMethod):nullptr;
    }
    const bool active=mono && camera && isActive && NativeBridge::nativeDesktopMouseUI &&
        !NativeBridge::controllerInputActive && !NativeBridge::nativeOverlayActive && isActive(mono,activeMethod);
    if (!active && object!=trackedTouch) return;
    const bool held=(NativeBridge::physicalMouseSnapshot.load() & 1)!=0;
    if (trackedTouch && (trackedTouch!=object || trackedCamera!=camera)) {
        StopTracking(object,nullptr);
        drag.held=held;
    }
    trackedTouch=object;trackedCamera=camera;mapActive=active;
    if (!active || !nativeInputManager || !NativeBridge::screenWidth || !NativeBridge::screenHeight) {
        StopTracking(object,camera);
        drag.held=held;
        return;
    }
    if (!held && !drag.held && !mapFlick.active()) return;
    static void *described;
    if(active&&camera!=described) {
        described=camera;
        Log(@"native_camera_mouse map_bound=1 typed_view_offset=%zu camera=%p",monoOffset,camera);
    }
    bool foreground=!pointerCaptureSuspended&&NativeBridge::IsForeground();
    float x=*(float *)(nativeInputManager+0xd0),y=*(float *)(nativeInputManager+0xd4);
    const auto [w,h]=NativeBridge::screenMetrics.read();
    CGRect bounds{};
    bool inside=x>=0&&y>=0&&x<w&&y<h;
    auto frame=desktopMapFrame.snapshot();
    if(frame.active && frame.focus==NativeBridge::mapFocusRevision.load())
        bounds=CGRectMake(frame.x,frame.y,frame.width,frame.height);
    else if(inside) inside=GameContentBounds(bounds);
    bool available=active&&foreground&&WantsVisibleCursor()&&inside;
    const bool fresh=held&&!drag.held;
    if(fresh&&available)available=drag.viewport.capture(w,h,bounds.size.width,bounds.size.height);
    else if(drag.armed)available=available&&drag.viewport.matches(w,h,bounds.size.width,bounds.size.height);
    const double now=CACurrentMediaTime();
    const auto focus=NativeBridge::mapFocusRevision.load();
    const bool sameViewport=drag.viewport.matches(w,h,bounds.size.width,bounds.size.height);
    if (mapFlick.active()) {
        if (!held && available && sameViewport && focus==drag.revision) {
            CoastFrame(camera,h,now);
            return;
        }
        mapFlick.cancel();
        FinishCameraPan(camera);
    }
    if (fresh) {
        mapFlick.begin(now);
    }
    const bool released=!held && available && sameViewport && focus==drag.revision;
    auto step=drag.sample(held,available,fresh&&available&&MapSurface(object,false),focus,x,y);
    if (fresh && drag.armed) Anchor::Begin(camera,{x,y});
    if (step.begin) {
        speed(camera,Vector3{},speedMethod);
        Goto(camera,1);
        Action(object,72);
        NativeBridge::mapDragActive=true;
    }
    const float dx=step.dx, dy=step.dy;
    if (drag.panning || dx || dy) mapFlick.dragFrame(dx,dy,h,now);
    if (Anchor::usingAnchor && (drag.panning || (step.end && released)))
        Anchor::Follow(camera,{x,y});
    else if (dx || dy) swipe(camera,{dx,dy},swipeMethod);
    if (step.end) {
        const bool coast=released && mapFlick.release(now);
        if (!coast) FinishCameraPan(camera);
        Action(object,80);
        NativeBridge::mapDragActive=false;
        if (coast) CoastFrame(camera,h,now);
    }
    if (step.click) {Anchor::Finish();Click(object,{x,y});}
    if (!drag.armed && !mapFlick.active() && Anchor::usingAnchor) Anchor::Finish();
}
static void Disable(void *object,void *method) {
    if(object==trackedTouch) {
        void *camera=*(void **)((char *)object+56);
        StopTracking(object,camera==trackedCamera?camera:nullptr);
        trackedTouch=trackedCamera=nullptr;
        mapActive=false;
    }
    originalDisable(object,method);
}
static bool TryInstall(void) {
    if(installed) return true;
    if(!NativeBridge::enabled||!NativeBridge::frames.load()) return false;
    void *cls=findClass("Assembly-CSharp.dll","MoleMole","MonoExUITouch");
    void *cam=findClass("Assembly-CSharp.dll","MoleMole","MonoExUICamera");
    if(!cls||!cam) return false;
    Anchor::Prepare(cam);
    // Validate every field used by this optional adapter against runtime
    // metadata. An unrecognized layout leaves the accepted R37 path intact.
    if(!HasManagedField(cls,nullptr,40,"GGOFADHAPNB")||
       !HasManagedField(cls,nullptr,56,"MonoExUICamera")||
       !HasManagedField(cls,nullptr,72,"Action")||!HasManagedField(cls,nullptr,80,"Action")) return false;
    void *update=Method(cls,"Update","Void",{}),*disable=Method(cls,"OnDisable","Void",{});
    dialogsMethod=Method(cls,"HasDialogsAtTop","Boolean",{});
    clickedGetter=lookup(cls,"get_onClicked",0,0);
    if(clickedGetter) {
        auto result=EngineFunction<void *(*)(void *)>(0x10e9bb6e8);
        auto from=EngineFunction<void *(*)(void *)>(0x10e9bb5e0);
        void *eventClass=from(result(clickedGetter));
        if(eventClass) clickInvoke=Method(eventClass,"Invoke","Void",{"Vector2"});
    }
    swipeMethod=Method(cam,"SwipeCamera","Void",{"Vector2"});
    speedMethod=Method(cam,"SetCameraSpeed","Void",{"Vector3"});
    gotoMethod=Method(cam,"GotoState","Void",{"HLLEANAOOGC","Object"});
    if(!update||!disable||!dialogsMethod||!swipeMethod||!speedMethod||!gotoMethod) return false;
    void *targets[]={(void *)ManagedMethodCode(update),(void *)ManagedMethodCode(disable)};
    uint8_t saved[2][16];
    for(unsigned i=0;i<2;++i) {
        if(!RegisterOnlyPrologue(targets[i])) return false;
        memcpy(saved[i],targets[i],16);
    }
    originalUpdate=(void (*)(void *,void *))Trampoline(targets[0]);
    originalDisable=(void (*)(void *,void *))Trampoline(targets[1]);
    if(!originalUpdate||!originalDisable) return false;
    void *replacements[]={(void *)Update,(void *)Disable};
    for(unsigned i=0;i<2;++i) {
        uint8_t jump[16];NativeBridge::AbsoluteJump(jump,(uintptr_t)replacements[i]);
        if(!NativeBridge::PatchCode(targets[i],jump,16)) {
            for(unsigned j=0;j<=i;++j) NativeBridge::PatchCode(targets[j],saved[j],16);
            return false;
        }
    }
    swipe=(decltype(swipe))ManagedMethodCode(swipeMethod);
    speed=(decltype(speed))ManagedMethodCode(speedMethod);
    installed=true;NativeBridge::nativePointerGestures=true;
    NativeBridge::mapContextReader=MapContext;
    Log(@"native_camera_mouse installed=1 map=1 click_event=%d projected_drag=%d generic_ui=pointer_event_data virtual_touch=0",clickInvoke!=nullptr,Anchor::ready);
    return true;
}
}

#pragma once
#include "keyboard_sources.h"

// ANSI hardware locations from the macOS Events.h virtual-key table.
// This is a foreground-only state query, not an event tap or text recorder.
static int MacKeyForHID(int hid) {
    static const int letters[]={0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    static const int digits[]={18,19,20,21,23,22,26,28,25,29};
    static const int functions[]={122,120,99,118,96,97,98,100,101,109,103,111};
    static const int extendedFunctions[]={105,107,113,106,64,79,80,90};
    if(hid>=4 && hid<=29) return letters[hid-4];
    if(hid>=30 && hid<=39) return digits[hid-30];
    if(hid>=58 && hid<=69) return functions[hid-58];
    if(hid>=104 && hid<=111) return extendedFunctions[hid-104];
    switch(hid) {
        case 40:return 36;case 41:return 53;case 42:return 51;case 43:return 48;case 44:return 49;
        case 45:return 27;case 46:return 24;case 47:return 33;case 48:return 30;case 49:return 42;
        case 51:return 41;case 52:return 39;case 53:return 50;case 54:return 43;case 55:return 47;case 56:return 44;
        case 79:return 124;case 80:return 123;case 81:return 125;case 82:return 126;
        case 224:return 59;case 225:return 56;case 226:return 58;case 227:return 55;
        case 228:return 62;case 229:return 60;case 230:return 61;case 231:return 54;
        default:return -1;
    }
}

namespace NativeBridge {
static DesktopKeyboardSources keyboardSources;
static std::atomic<unsigned> gcMovementSnapshot{0},hardwareMovementSnapshot{0};
static bool (*systemKeyState)(int,uint16_t);
static std::array<bool,350> hardwareVerified{};
static std::atomic<uint64_t> hardwareDisagreements{0};

static std::atomic<bool> macWindowSuspended{true};

static bool IsForeground(void) {
    if (macWindowSuspended.load()) return false;
    if (UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) return false;
    // Catalyst can retain an active UIScene while its macOS window is inactive.
    Class cls=NSClassFromString(@"NSApplication");
    if (!cls) return true;
    id app=((id (*)(id,SEL))objc_msgSend)(cls,sel_registerName("sharedApplication"));
    return app && ((BOOL (*)(id,SEL))objc_msgSend)(app,sel_registerName("isActive"));
}

static KeyboardState::Keys KeyboardSnapshot(bool foreground) {
    KeyboardState::Keys result{};
    if (!foreground) return result;
    GCKeyboardInput *input=[GCKeyboard.coalescedKeyboard.keyboardInput capture];
    unsigned gcMovement=0,hardwareMovement=0;
    for (int hid=4;hid<=231;++hid) {
        int key=UnityKeyForHID(hid);if (!key) continue;
        // A delivered AppKit edge already wins reconciliation until focus changes.
        // Skip redundant hardware queries for those keys on the normal input path.
        auto known = keyboardSources.native[key].load(std::memory_order_relaxed);
        if (known && !diagnosticsEnabled) { result[key] = known == 2; continue; }
        bool gcDown=input && [input buttonForKeyCode:(GCKeyCode)hid].pressed;
        bool down=gcDown;
        unsigned movement=key==97?1:key==100?2:key==115?4:key==119?8:0;
        if(gcDown) gcMovement|=movement;
        int macKey=MacKeyForHID(hid);
        if (systemKeyState && macKey>=0) {
            bool hardwareDown=systemKeyState(1,(uint16_t)macKey); // HID system state
            if(hardwareDown) hardwareMovement|=movement;
            // Verify the native query on a real press before trusting a negative.
            if (hardwareDown && (!IsModernRuntime() || gcDown)) hardwareVerified[key]=true;
            if (hardwareVerified[key]) {
                if (down!=hardwareDown) ++hardwareDisagreements;
                // In the modern runtime, a stale positive system bit must
                // never invent a press that neither native events nor GC saw.
                down=IsModernRuntime()?(gcDown && hardwareDown):hardwareDown;
            }
        }
        // An explicit AppKit release wins over both GC's capture and a stuck
        // HID-system key bit (observed for virtual key 0 / A on this client).
        // Device polling remains the fallback for keys without native events.
        result[key]=keyboardSources.resolve(key,down);
    }
    gcMovementSnapshot=gcMovement;hardwareMovementSnapshot=hardwareMovement;
    return result;
}

static void InstallHardwareReader(void) {
    void *cg=dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",RTLD_LAZY|RTLD_LOCAL);
    systemKeyState=(bool (*)(int,uint16_t))dlsym(cg?cg:RTLD_DEFAULT,"CGEventSourceKeyState");
    Log(@"hardware_state_reader available=%d foreground_only=1",systemKeyState!=nullptr);
}
}

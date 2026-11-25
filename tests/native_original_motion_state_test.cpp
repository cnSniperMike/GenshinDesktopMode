// 原版震动路由与拉弓去重的回归测试。
// 用于防止去重时再次切断飞行、下落攻击的震动，或遗漏另一个拉弓入口。
// 直接调用生产代码，使用模拟总线、动作数据和原版播放函数验证：
// 1. 主总线及其辅助子树能正确路由，私有总线和异常循环/过深结构不会被接管。
// 2. 私有震动后端可用时，两个已确认重复的拉弓播放动作均被拦截，计数分别记录。
// 3. 后端不可用或关闭时回退原版；其他动作、停止请求和私有轨道仍保留参数及返回值。
// 本测试不需要游戏资源或连接手柄，验证范围是路由与转发逻辑，不包含实际混音或手感。

#include "../genshin/native_original_motion_state.h"
#include <array>
#include <cassert>
#include <cstdio>

struct Bus {uint32_t id;const Bus *parent;};
static bool Routed(const Bus *bus){
    return NativeMotion::HasOriginalMotionRoot(bus,
        [](const Bus *b){return b->id;},[](const Bus *b){return b->parent;});
}
static bool replacementActive;
static unsigned originalCalls;
static void *lastAction,*lastPending;
static bool Ready(){return replacementActive;}
static int Original(void *action,void *pending){
    ++originalCalls;lastAction=action;lastPending=pending;return 37;
}
struct Action {
    std::array<char,0x38> bytes{};
    Action(uint32_t id,uint32_t target,uint16_t type=0x0403){
        memcpy(bytes.data()+0x10,&id,4);memcpy(bytes.data()+0x30,&target,4);memcpy(bytes.data()+0x34,&type,2);
    }
    void *get(){return bytes.data();}
};

int main(){
    Bus root{NativeMotion::OriginalCapabilities::rootBus,nullptr};
    Bus aux{0x19d4ed25,&root},child{0x178ec0e9,&aux};
    // Actual output lookup supplies only the master bus, even when the voice
    // fed an auxiliary child. Flight/plunge therefore require this to pass.
    assert(Routed(&root)&&Routed(&aux)&&Routed(&child));
    Bus privateRoot{0xfd4d0061,nullptr},privateBus{0xed000be0,&privateRoot};
    assert(!Routed(&privateBus)&&!Routed(nullptr));
    Bus cycle{0x88f25c00,nullptr};cycle.parent=&cycle;
    assert(!Routed(&cycle));
    Bus a{0x88f25c00,nullptr},b{0x178ec0e9,&a};a.parent=&b;
    assert(!Routed(&a));
    std::array<Bus,65> deep{};
    for(unsigned i=0;i<deep.size();++i)
        deep[i]={0x178ec0e9u,i+1<deep.size()?&deep[i+1]:&root};
    assert(!Routed(deep.data()));

    NativeMotion::OriginalMotionActionRelay relay;
    relay.original=Original;
    Action bow{0x2ce9920d,0x310b53b2};
    Action alternateBow{0x2948cffc,0x310b53b2};
    int pending=23;
    // Without a working replacement, even the duplicate uses native playback.
    assert(relay.execute(bow.get(),&pending)==37&&originalCalls==1);
    assert(relay.execute(alternateBow.get(),&pending)==37&&originalCalls==2);
    relay.replacementReady=Ready;
    assert(relay.execute(bow.get(),&pending)==37&&originalCalls==3);
    assert(relay.execute(alternateBow.get(),&pending)==37&&originalCalls==4);
    replacementActive=true;
    assert(relay.execute(bow.get(),&pending)==1&&originalCalls==4&&relay.suppressed==1);
    assert(relay.execute(alternateBow.get(),&pending)==1&&originalCalls==4&&relay.suppressed==2);
    assert(relay.suppressedPrimary==1&&relay.suppressedAlternate==1);
    // Native start/stop and flight/plunge audio actions still execute with
    // their original arguments and return values while bow dedup is active.
    for(auto action:{
        Action{0x19a44fc8,0x0779b52d}, // Bow's ordinary sound.
        Action{0x013ceaf5,0x15f44089}, // Flight start, native auxiliary sends.
        Action{0x24ce2268,0x0b7f25c4,0x0103}, // Flight stop.
        Action{0x1769e4d8,0x29a5ea5a}, // Flight end.
        Action{0x3d975091,0x1a05f496}, // Plunge.
        Action{0x14786e9d,0x0af37fdf}, // Landing.
        Action{0xed000123,0xed000456}, // Private authored action.
        Action{0x2ce9920d,0x310b53b2,0x0103},
        Action{0x2ce9920d,0x15f44089},
        Action{0x2948cffc,0x310b53b2,0x0103},
        Action{0x2948cffc,0x15f44089},
        Action{0x12345678,0x310b53b2} // Same sound through an unverified action.
    }){
        unsigned before=originalCalls;
        assert(relay.execute(action.get(),&pending)==37&&originalCalls==before+1);
        assert(lastAction==action.get()&&lastPending==&pending);
    }
    replacementActive=false;
    assert(relay.execute(bow.get(),&pending)==37&&relay.suppressed==2);
    assert(relay.execute(alternateBow.get(),&pending)==37&&relay.suppressed==2);
    assert(relay.suppressedPrimary==1&&relay.suppressedAlternate==1);
    puts("Original motion: root-only mixer lookup retains flight/plunge; both duplicate bow entries suppressed only with replacement; ordinary/private/start/stop, unverified entries and disabled fallback preserved.");
}

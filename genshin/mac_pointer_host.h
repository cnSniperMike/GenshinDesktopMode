#pragma once
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <type_traits>
#include <initializer_list>

// Catalyst exposes the real menu, panel and window focus through AppKit.
template<class Result = id, class... Args>
static Result MacPointerSend(id object, const char *name, Args... args) {
    if (!object) {
        if constexpr (std::is_void_v<Result>) return;
        else return Result{};
    }
    return reinterpret_cast<Result (*)(id, SEL, Args...)>(objc_msgSend)(
        object, sel_registerName(name), args...);
}

static id PointerApplication() {
    return MacPointerSend(NSClassFromString(@"NSApplication"), "sharedApplication");
}

static bool IsCatalystGameWindow(id window) {
    if (!window || [window isKindOfClass:NSClassFromString(@"NSPanel")] ||
        MacPointerSend<BOOL>(window, "isSheet") || MacPointerSend(window, "sheetParent") ||
        MacPointerSend(window, "attachedSheet") || MacPointerSend<BOOL>(window, "isMiniaturized"))
        return false;
    // A Catalyst scene owns the game surface. An ordinary NSWindow can
    // become both key and main (e.g. the About window).
    for (Class cls = [window class]; cls; cls = class_getSuperclass(cls)) {
        if ([NSStringFromClass(cls) hasPrefix:@"UINS"]) return true;
    }
    id content = MacPointerSend(window, "contentView");
    return [NSStringFromClass([content class]) hasPrefix:@"UINS"];
}

static NSData *LoginArrowPNG() {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/GenshinDesktopCursor.png"];
    NSData *cached = [NSData dataWithContentsOfFile:path];
    if (cached.length && cached.length <= 64 * 1024 &&
        CGSizeEqualToSize([UIImage imageWithData:cached].size, CGSizeMake(32, 32))) return cached;

    // First launch, before Unity supplies its cursor texture.
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat defaultFormat];
    format.scale = 2;
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(32, 32) format:format];
    return [renderer PNGDataWithActions:^(UIGraphicsImageRendererContext *context) {
        CGContextRef cg = context.CGContext;
        auto facet = [&](UIColor *color, std::initializer_list<CGPoint> points) {
            CGContextBeginPath(cg);
            auto it = points.begin();
            CGContextMoveToPoint(cg, it->x, it->y);
            while (++it != points.end()) CGContextAddLineToPoint(cg, it->x, it->y);
            CGContextClosePath(cg);
            CGContextSetFillColorWithColor(cg, color.CGColor);
            CGContextFillPath(cg);
        };
        facet([UIColor colorWithWhite:0.15 alpha:0.9],
              {{1,1},{14,6},{26,7},{23,16},{26,26},{16,23},{8,27},{6,14}});
        facet(UIColor.whiteColor, {{2,2},{14,7},{25,8},{16,15}});
        facet([UIColor colorWithRed:1 green:.91 blue:.47 alpha:1], {{2,2},{16,15},{8,25},{7,14}});
        facet([UIColor colorWithRed:1 green:.78 blue:0 alpha:1], {{25,8},{22,16},{25,25},{16,21},{8,25},{16,15}});
    }];
}

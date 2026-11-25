#pragma once
#import <CommonCrypto/CommonDigest.h>
#include "native_client_profiles.h"
#include "resource_manifest.h"
#ifndef GDM_EMBEDDED_MOTION_PACK
#define GDM_EMBEDDED_MOTION_PACK 0
#endif
template<class T>static T F(uintptr_t va){return (T)(va+_dyld_get_image_vmaddr_slide(0));}
static std::atomic<bool> nativeResourceReady{false};
static const char *NativePackFilename(){return NativeResourceFilename;}
static NSString *NativeDigest(NSData *data){
    if(data.length>UINT32_MAX)return nil;
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,hash);
    char hex[CC_SHA256_DIGEST_LENGTH*2+1];
    for(unsigned i=0;i<CC_SHA256_DIGEST_LENGTH;++i)snprintf(hex+i*2,3,"%02x",hash[i]);
    return [NSString stringWithUTF8String:hex];
}
static bool NativeCodeProfile(){
    if(!IsModernRuntime())return false;
    const auto *table=IsGlobalModernRuntime()?NativeGlobalFingerprints:NativeCNFingerprints;
    for(size_t i=0;i<NativeFingerprintCount;++i){
        auto range=RuntimeImageRange(table[i].address,32,false);
        if(!range||memcmp(range,table[i].code,32))return false;
    }
    return true;
}
static void PrepareNativeResource(){
    Dl_info own{};if(!dladdr((void *)PrepareNativeResource,&own)||!own.dli_fbase)return;
    auto header=(const mach_header_64 *)own.dli_fbase;
    const uint8_t *bytes=nullptr;size_t size=0;
    for(uint32_t i=0;i<_dyld_image_count();++i){
        if(_dyld_get_image_header(i)!=(const mach_header *)header)continue;
        auto command=(const load_command *)(header+1);auto slide=_dyld_get_image_vmaddr_slide(i);
        for(unsigned n=0;n<header->ncmds;++n){
            if(command->cmd==LC_SEGMENT_64){auto segment=(const segment_command_64 *)command;
                auto sections=(const section_64 *)(segment+1);
                for(unsigned s=0;s<segment->nsects;++s)if(!strcmp(sections[s].segname,"__GDM")&&!strcmp(sections[s].sectname,"__motion_pack")){
                    bytes=(const uint8_t *)(sections[s].addr+slide);size=sections[s].size;
                }
            }command=(const load_command *)((const char *)command+command->cmdsize);
        }break;
    }
    if(!bytes||size!=NativeResourceBytes){Log(@"native_motion resource_missing=1");return;}
    NSData *data=[NSData dataWithBytesNoCopy:(void *)bytes length:size freeWhenDone:NO];
    NSString *digest=[NSString stringWithUTF8String:NativeResourceSHA256];
    if(![NativeDigest(data) isEqualToString:digest]){Log(@"native_motion resource_hash_mismatch=1");return;}
    // Generated cache only, outside the App and the read-only PC installation.
    NSString *folder=[NSHomeDirectory()stringByAppendingPathComponent:@"Library/MihoyoDownload/Persistent/AudioAssets"];
    BOOL isDir=NO;
    if(![NSFileManager.defaultManager fileExistsAtPath:folder isDirectory:&isDir]||!isDir){Log(@"native_motion resource_folder_unavailable=1");return;}
    NSString *path=[folder stringByAppendingPathComponent:[NSString stringWithUTF8String:NativeResourceFilename]];
    NSData *existing=[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    bool cached=existing.length==size&&[NativeDigest(existing) isEqualToString:digest];
    if(!cached){
        if(![data writeToFile:path options:NSDataWritingAtomic error:nil]){Log(@"native_motion resource_cache_failed=1");return;}
        NSData *written=[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
        if(![NativeDigest(written) isEqualToString:digest])return;
    }
    nativeResourceReady=true;Log(@"native_motion resource_ready=1 cached=%d bytes=%zu pc_source_untouched=1",cached,size);
}
#define W Log
#include "native_motion_source.h"
#include "sink_observer.h"
#include "private_output_probe.h"
#include "native_live_probe.h"
#undef W
#if GDM_NATIVE_SETTINGS
#include "native_vibration_settings.h"
#endif
static void InstallNativeProductBackend(void (*settingsChanged)(bool)=nullptr){
    if (!GDM_EMBEDDED_MOTION_PACK) {
        Log(@"native_motion disabled=1 reason=motion_pack_not_bundled");
        return;
    }
#if GDM_NATIVE_SETTINGS
    NativeVibrationSettings::onEnabledChanged=settingsChanged;
#endif
    if(!NativeCodeProfile()){Log(@"native_motion disabled=1 reason=code_profile_mismatch");return;}
    Log(@"native_motion client_profile=%@ verified=1",IsGlobalModernRuntime()?@"global":@"cn");
    if(!SinkObserver::Install()){Log(@"native_motion disabled=1 reason=sink_binding");return;}
    #if GDM_NATIVE_PRODUCT
    NativeOriginalMotion::Install();
    #endif
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{PrepareNativeResource();});
    [NSTimer scheduledTimerWithTimeInterval:.02 repeats:YES block:^(NSTimer *timer){
        static bool begun=false;static unsigned status=0;
        if(!begun){
            // Observe already established input readiness; never initialize
            // managed metadata or switch the user's input mode from here.
            if(nativeResourceReady.load()&&NativeBridge::enabled&&NativeBridge::frames.load()>120)
                begun=NativeLive::Begin();
            return;
        }
        NativeLive::Tick();
#if GDM_NATIVE_SETTINGS
        if(NativeLive::ready.load())NativeVibrationSettings::Install();
#endif
        if(++status%250==0)NativeLive::Status();
        if(NativeLive::phase==5||NativeLive::phase==9)[timer invalidate];
    }];
    Log(@"native_motion installed=1 authored_file_graph=1 native_control_source=1 physical_output_unverified=1");
}

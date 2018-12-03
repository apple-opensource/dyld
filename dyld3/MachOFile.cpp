/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <TargetConditionals.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>

#include "MachOFile.h"
#include "SupportedArchs.h"

#ifndef EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
    #define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE 0x02
#endif

#ifndef CPU_SUBTYPE_ARM64_E
    #define CPU_SUBTYPE_ARM64_E    2
#endif

#ifndef CPU_SUBTYPE_ARM64_32_V8
    #define CPU_SUBTYPE_ARM64_32_V8    1
#endif

#ifndef CPU_TYPE_ARM64_32
    #ifndef CPU_ARCH_ABI64_32
        #define CPU_ARCH_ABI64_32            0x02000000
    #endif
    #define CPU_TYPE_ARM64_32            (CPU_TYPE_ARM | CPU_ARCH_ABI64_32)
#endif

namespace dyld3 {

////////////////////////////  FatFile ////////////////////////////////////////

const FatFile* FatFile::isFatFile(const void* fileStart)
{
    const FatFile* fileStartAsFat = (FatFile*)fileStart;
    if ( (fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC)) || (fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return fileStartAsFat;
    else
        return nullptr;
}

void FatFile::forEachSlice(Diagnostics& diag, uint64_t fileLen, void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const
{
	if ( this->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
        if ( OSSwapBigToHostInt32(nfat_arch) > ((4096 - sizeof(fat_header)) / sizeof(fat_arch)) ) {
            diag.error("fat header too large: %u entries", OSSwapBigToHostInt32(nfat_arch));
            return;
        }
        bool stop = false;
        const fat_arch* const archs = (fat_arch*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; i < OSSwapBigToHostInt32(nfat_arch); ++i) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
            uint32_t offset     = OSSwapBigToHostInt32(archs[i].offset);
            uint32_t len        = OSSwapBigToHostInt32(archs[i].size);
            if ( greaterThanAddOrOverflow(offset, len, fileLen) ) {
                diag.error("slice %d extends beyond end of file", i);
                return;
            }
            callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            if ( stop )
                break;
        }
    }
    else if ( this->magic == OSSwapBigToHostInt32(FAT_MAGIC_64) ) {
        if ( OSSwapBigToHostInt32(nfat_arch) > ((4096 - sizeof(fat_header)) / sizeof(fat_arch)) ) {
            diag.error("fat header too large: %u entries", OSSwapBigToHostInt32(nfat_arch));
            return;
        }
        bool stop = false;
        const fat_arch_64* const archs = (fat_arch_64*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; i < OSSwapBigToHostInt32(nfat_arch); ++i) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
            uint64_t offset     = OSSwapBigToHostInt64(archs[i].offset);
            uint64_t len        = OSSwapBigToHostInt64(archs[i].size);
            if ( greaterThanAddOrOverflow(offset, len, fileLen) ) {
                diag.error("slice %d extends beyond end of file", i);
                return;
            }
            callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            if ( stop )
                break;
        }
    }
    else {
        diag.error("not a fat file");
    }
}

bool FatFile::isFatFileWithSlice(Diagnostics& diag, uint64_t fileLen, const char* archName, uint64_t& sliceOffset, uint64_t& sliceLen, bool& missingSlice) const
{
    missingSlice = false;
    if ( (this->magic != OSSwapBigToHostInt32(FAT_MAGIC)) && (this->magic != OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return false;

    __block bool found = false;
    forEachSlice(diag, fileLen, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
        const char* sliceArchName = MachOFile::archName(sliceCpuType, sliceCpuSubType);
        if ( strcmp(sliceArchName, archName) == 0 ) {
            sliceOffset = (char*)sliceStart - (char*)this;
            sliceLen    = sliceSize;
            found       = true;
            stop        = true;
        }
    });
    if ( diag.hasError() )
        return false;

    if ( !found )
        missingSlice = true;

    // when looking for x86_64h fallback to x86_64
    if ( !found && (strcmp(archName, "x86_64h") == 0) )
        return isFatFileWithSlice(diag, fileLen, "x86_64", sliceOffset, sliceLen, missingSlice);

    return found;
}


////////////////////////////  MachOFile ////////////////////////////////////////


const MachOFile::ArchInfo MachOFile::_s_archInfos[] = {
    { "x86_64",   CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_ALL  },
    { "x86_64h",  CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_H    },
    { "i386",     CPU_TYPE_I386,     CPU_SUBTYPE_I386_ALL    },
    { "arm64",    CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64_ALL   },
#if SUPPORT_ARCH_arm64e
    { "arm64e",   CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64_E     },
#endif
#if SUPPORT_ARCH_arm64_32
    { "arm64_32", CPU_TYPE_ARM64_32, CPU_SUBTYPE_ARM64_32_V8 },
#endif
    { "armv7k",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7K     },
    { "armv7s",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7S     },
    { "armv7",    CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7      }
};

const MachOFile::PlatformInfo MachOFile::_s_platformInfos[] = {
    { "macOS",       Platform::macOS,             LC_VERSION_MIN_MACOSX   },
    { "iOS",         Platform::iOS,               LC_VERSION_MIN_IPHONEOS },
    { "tvOS",        Platform::tvOS,              LC_VERSION_MIN_TVOS     },
    { "watchOS",     Platform::watchOS,           LC_VERSION_MIN_WATCHOS  },
    { "bridgeOS",    Platform::bridgeOS,          LC_BUILD_VERSION        },
    { "iOSMac",      Platform::iOSMac,            LC_BUILD_VERSION        },
    { "iOS-sim",     Platform::iOS_simulator,     LC_BUILD_VERSION        },
    { "tvOS-sim",    Platform::tvOS_simulator,    LC_BUILD_VERSION        },
    { "watchOS-sim", Platform::watchOS_simulator, LC_BUILD_VERSION        },
};


bool MachOFile::is64() const
{
    return (this->magic == MH_MAGIC_64);
}

uint32_t MachOFile::pointerSize() const
{
    if (this->magic == MH_MAGIC_64)
        return 8;
    else
        return 4;
}

bool MachOFile::uses16KPages() const
{
    switch (this->cputype) {
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM:
        case CPU_TYPE_ARM64_32:
            return true;
        default:
            return false;
    }
}

bool MachOFile::isArch(const char* aName) const
{
    return (strcmp(aName, archName(this->cputype, this->cpusubtype)) == 0);
}

const char* MachOFile::archName(uint32_t cputype, uint32_t cpusubtype)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( (cputype == info.cputype) && ((cpusubtype & ~CPU_SUBTYPE_MASK) == info.cpusubtype) ) {
            return info.name;
        }
    }
    return "unknown";
}

uint32_t MachOFile::cpuTypeFromArchName(const char* archName)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( strcmp(archName, info.name) == 0 ) {
            return info.cputype;
        }
    }
    return 0;
}

uint32_t MachOFile::cpuSubtypeFromArchName(const char* archName)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( strcmp(archName, info.name) == 0 ) {
            return info.cpusubtype;
        }
    }
    return 0;
}

const char* MachOFile::archName() const
{
    return archName(this->cputype, this->cpusubtype);
}

static void appendDigit(char*& s, unsigned& num, unsigned place, bool& startedPrinting)
{
    if ( num >= place ) {
        unsigned dig = (num/place);
        *s++ = '0' + dig;
        num -= (dig*place);
        startedPrinting = true;
    }
    else if ( startedPrinting ) {
        *s++ = '0';
    }
}

static void appendNumber(char*& s, unsigned num)
{
    assert(num < 99999);
    bool startedPrinting = false;
    appendDigit(s, num, 10000, startedPrinting);
    appendDigit(s, num,  1000, startedPrinting);
    appendDigit(s, num,   100, startedPrinting);
    appendDigit(s, num,    10, startedPrinting);
    appendDigit(s, num,     1, startedPrinting);
    if ( !startedPrinting )
        *s++ = '0';
}

void MachOFile::packedVersionToString(uint32_t packedVersion, char versionString[32])
{
    // sprintf(versionString, "%d.%d.%d", (packedVersion >> 16), ((packedVersion >> 8) & 0xFF), (packedVersion & 0xFF));
    char* s = versionString;
    appendNumber(s, (packedVersion >> 16));
    *s++ = '.';
    appendNumber(s, (packedVersion >> 8) & 0xFF);
    *s++ = '.';
    appendNumber(s, (packedVersion & 0xFF));
    *s++ = '\0';
}

bool MachOFile::supportsPlatform(Platform reqPlatform) const
{
    __block bool foundRequestedPlatform = false;
    __block bool foundOtherPlatform = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        if ( platform == reqPlatform )
            foundRequestedPlatform = true;
        else
            foundOtherPlatform = true;
    });
    if ( foundRequestedPlatform )
        return true;

    // we did find some platform info, but not requested, so return false
    if ( foundOtherPlatform )
        return false;

    // binary has no explict load command to mark platform
    // could be an old macOS binary, look at arch
    if  ( reqPlatform == Platform::macOS ) {
        if ( this->cputype == CPU_TYPE_X86_64 )
            return true;
        if ( this->cputype == CPU_TYPE_I386 )
            return true;
    }

    return false;
}

Platform MachOFile::currentPlatform()
{
#if TARGET_OS_BRIDGE
    return Platform::bridgeOS;
#elif TARGET_OS_WATCH
    return Platform::watchOS;
#elif TARGET_OS_TV
    return Platform::tvOS;
#elif TARGET_OS_IOS
    return Platform::iOS;
#elif TARGET_OS_MAC
    return Platform::macOS;
#else
    #error unknown platform
#endif
}

#if __x86_64__
static bool isHaswell()
{
    // FIXME: figure out a commpage way to check this
    static bool sAlreadyDetermined = false;
    static bool sHaswell = false;
    if ( !sAlreadyDetermined ) {
        struct host_basic_info info;
        mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
        mach_port_t hostPort = mach_host_self();
        kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
        mach_port_deallocate(mach_task_self(), hostPort);
        sHaswell = (result == KERN_SUCCESS) && (info.cpu_subtype == CPU_SUBTYPE_X86_64_H);
        sAlreadyDetermined = true;
    }
    return sHaswell;
}
#endif

const char* MachOFile::currentArchName()
{
#if __ARM_ARCH_7K__
    return "armv7k";
#elif __ARM_ARCH_7A__
    return "armv7";
#elif __ARM_ARCH_7S__
    return "armv7s";
#elif __arm64e__
    return "arm64e";
#elif __arm64__
#if __LP64__
    return "arm64";
#else
    return "arm64_32";
#endif
#elif __x86_64__
    return isHaswell() ? "x86_64h" : "x86_64";
#elif __i386__
    return "i386";
#else
    #error unknown arch
#endif
}


bool MachOFile::isDylib() const
{
    return (this->filetype == MH_DYLIB);
}

bool MachOFile::isBundle() const
{
    return (this->filetype == MH_BUNDLE);
}

bool MachOFile::isMainExecutable() const
{
    return (this->filetype == MH_EXECUTE);
}

bool MachOFile::isDynamicExecutable() const
{
    if ( this->filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    __block bool hasDyldLoad = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_LOAD_DYLINKER ) {
            hasDyldLoad = true;
            stop = true;
        }
    });
    return hasDyldLoad;
}

bool MachOFile::isPIE() const
{
    return (this->flags & MH_PIE);
}

const char* MachOFile::platformName(Platform reqPlatform)
{
    for (const PlatformInfo& info : _s_platformInfos) {
        if ( info.platform == reqPlatform )
            return info.name;
    }
    return "unknown platform";
}

void MachOFile::forEachSupportedPlatform(void (^handler)(Platform platform, uint32_t minOS, uint32_t sdk)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        const build_version_command* buildCmd = (build_version_command *)cmd;
        const version_min_command*   versCmd  = (version_min_command*)cmd;
        switch ( cmd->cmd ) {
            case LC_BUILD_VERSION:
                handler((Platform)(buildCmd->platform), buildCmd->minos, buildCmd->sdk);
                break;
            case LC_VERSION_MIN_MACOSX:
                handler(Platform::macOS, versCmd->version, versCmd->sdk);
                break;
            case LC_VERSION_MIN_IPHONEOS:
                if ( (this->cputype == CPU_TYPE_X86_64) || (this->cputype == CPU_TYPE_I386) )
                    handler(Platform::iOS_simulator, versCmd->version, versCmd->sdk); // old sim binary
                else
                    handler(Platform::iOS, versCmd->version, versCmd->sdk);
                break;
            case LC_VERSION_MIN_TVOS:
                if ( this->cputype == CPU_TYPE_X86_64 )
                    handler(Platform::tvOS_simulator, versCmd->version, versCmd->sdk); // old sim binary
                else
                    handler(Platform::tvOS, versCmd->version, versCmd->sdk);
                break;
            case LC_VERSION_MIN_WATCHOS:
                if ( (this->cputype == CPU_TYPE_X86_64) || (this->cputype == CPU_TYPE_I386) )
                    handler(Platform::watchOS_simulator, versCmd->version, versCmd->sdk); // old sim binary
                else
                    handler(Platform::watchOS, versCmd->version, versCmd->sdk);
                break;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}


bool MachOFile::isMachO(Diagnostics& diag, uint64_t fileSize) const
{
    if ( !hasMachOMagic() ) {
        diag.error("file does not start with MH_MAGIC[_64]");
        return false;
    }
    if ( this->sizeofcmds + sizeof(mach_header_64) > fileSize ) {
        diag.error("load commands exceed length of first segment");
        return false;
    }
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) { });
    return diag.noError();
}

bool MachOFile::hasMachOMagic() const
{
    return ( (this->magic == MH_MAGIC) || (this->magic == MH_MAGIC_64) );
}

void MachOFile::forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const
{
    bool stop = false;
    const load_command* startCmds = nullptr;
    if ( this->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)this + sizeof(mach_header_64));
    else if ( this->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)this + sizeof(mach_header));
    else {
        diag.error("file does not start with MH_MAGIC[_64]");
        return;  // not a mach-o file
    }
    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + this->sizeofcmds);
    const load_command* cmd = startCmds;
    for (uint32_t i = 0; i < this->ncmds; ++i) {
        const load_command* nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
        if ( cmd->cmdsize < 8 ) {
            diag.error("malformed load command #%d, size too small %d", i, cmd->cmdsize);
            return;
        }
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            diag.error("malformed load command #%d, size too large 0x%X", i, cmd->cmdsize);
            return;
        }
        callback(cmd, stop);
        if ( stop )
            return;
        cmd = nextCmd;
    }
}

const char* MachOFile::installName() const
{
    const char*  name;
    uint32_t     compatVersion;
    uint32_t     currentVersion;
    if ( getDylibInstallName(&name, &compatVersion, &currentVersion) )
        return name;
    return nullptr;
}

bool MachOFile::getDylibInstallName(const char** installName, uint32_t* compatVersion, uint32_t* currentVersion) const
{
    Diagnostics diag;
    __block bool found = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ID_DYLIB ) {
            const dylib_command*  dylibCmd = (dylib_command*)cmd;
            *compatVersion  = dylibCmd->dylib.compatibility_version;
            *currentVersion = dylibCmd->dylib.current_version;
            *installName    = (char*)dylibCmd + dylibCmd->dylib.name.offset;
            found = true;
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return found;
}

bool MachOFile::getUuid(uuid_t uuid) const
{
    Diagnostics diag;
    __block bool found = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UUID ) {
            const uuid_command* uc = (const uuid_command*)cmd;
            memcpy(uuid, uc->uuid, sizeof(uuid_t));
            found = true;
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    if ( !found )
        bzero(uuid, sizeof(uuid_t));
    return found;
}

void MachOFile::forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                const char* loadPath = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                callback(loadPath, (cmd->cmd == LC_LOAD_WEAK_DYLIB), (cmd->cmd == LC_REEXPORT_DYLIB), (cmd->cmd == LC_LOAD_UPWARD_DYLIB),
                                    dylibCmd->dylib.compatibility_version, dylibCmd->dylib.current_version, stop);
            }
            break;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOFile::forDyldEnv(void (^callback)(const char* envVar, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( cmd->cmd == LC_DYLD_ENVIRONMENT ) {
            const dylinker_command* envCmd = (dylinker_command*)cmd;
            const char* keyEqualsValue = (char*)envCmd + envCmd->name.offset;
            // only process variables that start with DYLD_ and end in _PATH
            if ( (strncmp(keyEqualsValue, "DYLD_", 5) == 0) ) {
                const char* equals = strchr(keyEqualsValue, '=');
                if ( equals != NULL ) {
                    if ( strncmp(&equals[-5], "_PATH", 5) == 0 ) {
                        callback(keyEqualsValue, stop);
                    }
                }
            }
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

bool MachOFile::enforceCompatVersion() const
{
    __block bool result = true;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        switch ( platform ) {
            case Platform::macOS:
                if ( minOS >= 0x000A0E00 )  // macOS 10.14
                    result = false;
                break;
            case Platform::iOS:
            case Platform::tvOS:
            case Platform::iOS_simulator:
            case Platform::tvOS_simulator:
                if ( minOS >= 0x000C0000 )  // iOS 12.0
                    result = false;
                break;
            case Platform::watchOS:
            case Platform::watchOS_simulator:
                if ( minOS >= 0x00050000 )  // watchOS 5.0
                    result = false;
                break;
            case Platform::bridgeOS:
                if ( minOS >= 0x00030000 )  // bridgeOS 3.0
                    result = false;
                break;
            case Platform::iOSMac:
                result = false;
                break;
            case Platform::unknown:
                break;
        }
    });
    return result;
}


void MachOFile::forEachSegment(void (^callback)(const SegmentInfo& info, bool& stop)) const
{
    Diagnostics diag;
    const bool intel32 = (this->cputype == CPU_TYPE_I386);
    __block uint32_t segIndex = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            const section_64* const sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section_64* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
            }
            SegmentInfo info;
            info.fileOffset        = segCmd->fileoff;
            info.fileSize          = segCmd->filesize;
            info.vmAddr            = segCmd->vmaddr;
            info.vmSize            = segCmd->vmsize;
            info.sizeOfSections    = sizeOfSections;
            info.segName           = segCmd->segname;
            info.protections       = segCmd->initprot;
            info.textRelocs        = false;
            info.p2align           = p2align;
            info.segIndex          = segIndex;
            callback(info, stop);
            ++segIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            bool  hasTextRelocs = false;
            const section* const sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
                if ( sect->flags & (S_ATTR_EXT_RELOC|S_ATTR_LOC_RELOC) )
                    hasTextRelocs = true;
           }
            SegmentInfo info;
            info.fileOffset        = segCmd->fileoff;
            info.fileSize          = segCmd->filesize;
            info.vmAddr            = segCmd->vmaddr;
            info.vmSize            = segCmd->vmsize;
            info.sizeOfSections    = sizeOfSections;
            info.segName           = segCmd->segname;
            info.protections       = segCmd->initprot;
            info.textRelocs        = intel32 && !info.writable() && hasTextRelocs;
            info.p2align           = p2align;
            info.segIndex          = segIndex;
            callback(info, stop);
            ++segIndex;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOFile::forEachSection(void (^callback)(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop)) const
{
    Diagnostics diag;
    const bool intel32 = (this->cputype == CPU_TYPE_I386);
    __block uint32_t segIndex = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        SectionInfo sectInfo;
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            const section_64* const sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section_64* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
            }
            sectInfo.segInfo.fileOffset        = segCmd->fileoff;
            sectInfo.segInfo.fileSize          = segCmd->filesize;
            sectInfo.segInfo.vmAddr            = segCmd->vmaddr;
            sectInfo.segInfo.vmSize            = segCmd->vmsize;
            sectInfo.segInfo.sizeOfSections    = sizeOfSections;
            sectInfo.segInfo.segName           = segCmd->segname;
            sectInfo.segInfo.protections       = segCmd->initprot;
            sectInfo.segInfo.textRelocs        = false;
            sectInfo.segInfo.p2align           = p2align;
            sectInfo.segInfo.segIndex          = segIndex;
            for (const section_64* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
                const char* sectName = sect->sectname;
                char sectNameCopy[20];
                if ( sectName[15] != '\0' ) {
                    strlcpy(sectNameCopy, sectName, 17);
                    sectName = sectNameCopy;
                }
                bool malformedSectionRange = (sect->addr < segCmd->vmaddr) || greaterThanAddOrOverflow(sect->addr, sect->size, segCmd->vmaddr + segCmd->filesize);
                sectInfo.sectName       = sectName;
                sectInfo.sectFileOffset = sect->offset;
                sectInfo.sectFlags      = sect->flags;
                sectInfo.sectAddr       = sect->addr;
                sectInfo.sectSize       = sect->size;
                sectInfo.sectAlignP2    = sect->align;
                sectInfo.reserved1      = sect->reserved1;
                sectInfo.reserved2      = sect->reserved2;
                callback(sectInfo, malformedSectionRange, stop);
            }
            ++segIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            bool  hasTextRelocs = false;
            const section* const sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
                if ( sect->flags & (S_ATTR_EXT_RELOC|S_ATTR_LOC_RELOC) )
                    hasTextRelocs = true;
            }
            sectInfo.segInfo.fileOffset        = segCmd->fileoff;
            sectInfo.segInfo.fileSize          = segCmd->filesize;
            sectInfo.segInfo.vmAddr            = segCmd->vmaddr;
            sectInfo.segInfo.vmSize            = segCmd->vmsize;
            sectInfo.segInfo.sizeOfSections    = sizeOfSections;
            sectInfo.segInfo.segName           = segCmd->segname;
            sectInfo.segInfo.protections       = segCmd->initprot;
            sectInfo.segInfo.textRelocs        = intel32 && !sectInfo.segInfo.writable() && hasTextRelocs;
            sectInfo.segInfo.p2align           = p2align;
            sectInfo.segInfo.segIndex          = segIndex;
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
                const char* sectName = sect->sectname;
                char sectNameCopy[20];
                if ( sectName[15] != '\0' ) {
                    strlcpy(sectNameCopy, sectName, 17);
                    sectName = sectNameCopy;
                }
                bool malformedSectionRange = (sect->addr < segCmd->vmaddr) || greaterThanAddOrOverflow(sect->addr, sect->size, segCmd->vmaddr + segCmd->filesize);
                sectInfo.sectName       = sectName;
                sectInfo.sectFileOffset = sect->offset;
                sectInfo.sectFlags      = sect->flags;
                sectInfo.sectAddr       = sect->addr;
                sectInfo.sectSize       = sect->size;
                sectInfo.sectAlignP2    = sect->align;
                sectInfo.reserved1      = sect->reserved1;
                sectInfo.reserved2      = sect->reserved2;
                callback(sectInfo, malformedSectionRange, stop);
            }
            ++segIndex;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

bool MachOFile::hasWeakDefs() const
{
    return (this->flags & MH_WEAK_DEFINES);
}

bool MachOFile::hasThreadLocalVariables() const
{
    return (this->flags & MH_HAS_TLV_DESCRIPTORS);
}

static bool endsWith(const char* str, const char* suffix)
{
    size_t strLen    = strlen(str);
    size_t suffixLen = strlen(suffix);
    if ( strLen < suffixLen )
        return false;
    return (strcmp(&str[strLen-suffixLen], suffix) == 0);
}

bool MachOFile::canBePlacedInDyldCache(const char* path, void (^failureReason)(const char*)) const
{
    // only dylibs can go in cache
    if ( this->filetype != MH_DYLIB ) {
        failureReason("Not MH_DYLIB");
        return false; // cannot continue, installName() will assert() if not a dylib
    }

    // only dylibs built for /usr/lib or /System/Library can go in cache
    bool retval = true;
    const char* dylibName = installName();
    if ( dylibName[0] != '/' ) {
        retval = false;
        failureReason("install name not an absolute path");
    }
    else if ( (strncmp(dylibName, "/usr/lib/", 9) != 0) && (strncmp(dylibName, "/System/Library/", 16) != 0) ) {
        retval = false;
        failureReason("Not in '/usr/lib/' or '/System/Library/'");
    }

    // flat namespace files cannot go in cache
    if ( (this->flags & MH_TWOLEVEL) == 0 ) {
        retval = false;
        failureReason("Not built with two level namespaces");
    }

    // don't put debug variants into dyld cache
    if ( endsWith(path, "_profile.dylib") || endsWith(path, "_debug.dylib") || endsWith(path, "_profile") || endsWith(path, "_debug") || endsWith(path, "/CoreADI") ) {
        retval = false;
        failureReason("Variant image");
    }

    // dylib must have extra info for moving DATA and TEXT segments apart
    __block bool hasExtraInfo = false;
    __block bool hasDyldInfo = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO )
            hasExtraInfo = true;
        if ( cmd->cmd == LC_DYLD_INFO_ONLY )
            hasDyldInfo = true;
    });
    if ( !hasExtraInfo ) {
        retval = false;
        failureReason("Missing split seg info");
    }
    if ( !hasDyldInfo ) {
        retval = false;
        failureReason("Old binary, missing dyld info");
    }

    // dylib can only depend on other dylibs in the shared cache
    __block bool allDepPathsAreGood = true;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( (strncmp(loadPath, "/usr/lib/", 9) != 0) && (strncmp(loadPath, "/System/Library/", 16) != 0) ) {
            allDepPathsAreGood = false;
            stop = true;
        }
    });
    if ( !allDepPathsAreGood ) {
        retval = false;
        failureReason("Depends on dylibs ineligable for dyld cache");
    }

    // dylibs with interposing info cannot be in cache
    __block bool hasInterposing = false;
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( ((info.sectFlags & SECTION_TYPE) == S_INTERPOSING) || ((strcmp(info.sectName, "__interpose") == 0) && (strcmp(info.segInfo.segName, "__DATA") == 0)) )
            hasInterposing = true;
    });
    if ( hasInterposing ) {
        retval = false;
        failureReason("Has interposing tuples");
    }

    return retval;
}


bool MachOFile::isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const
{
    if ( const encryption_info_command* encCmd = findFairPlayEncryptionLoadCommand() ) {
       if ( encCmd->cryptid == 1 ) {
            // Note: cryptid is 0 in just-built apps.  The AppStore sets cryptid to 1
            textOffset = encCmd->cryptoff;
            size       = encCmd->cryptsize;
            return true;
        }
    }
    textOffset = 0;
    size = 0;
    return false;
}

bool MachOFile::canBeFairPlayEncrypted() const
{
    return (findFairPlayEncryptionLoadCommand() != nullptr);
}

const encryption_info_command* MachOFile::findFairPlayEncryptionLoadCommand() const
{
    __block const encryption_info_command* result = nullptr;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( (cmd->cmd == LC_ENCRYPTION_INFO) || (cmd->cmd == LC_ENCRYPTION_INFO_64) ) {
            result = (encryption_info_command*)cmd;
            stop = true;
        }
    });
    if ( diag.noError() )
        return result;
    else
        return nullptr;
}


bool MachOFile::hasChainedFixups() const
{
#if SUPPORT_ARCH_arm64e
    // for now only arm64e uses chained fixups
    return ( strcmp(archName(), "arm64e") == 0 );
#else
    return false;
#endif
}

uint64_t MachOFile::read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
{
    uint64_t result = 0;
    int         bit = 0;
    do {
        if ( p == end ) {
            diag.error("malformed uleb128");
            break;
        }
        uint64_t slice = *p & 0x7f;

        if ( bit > 63 ) {
            diag.error("uleb128 too big for uint64");
            break;
        }
        else {
            result |= (slice << bit);
            bit += 7;
        }
    }
    while (*p++ & 0x80);
    return result;
}


int64_t MachOFile::read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
{
    int64_t  result = 0;
    int      bit = 0;
    uint8_t  byte = 0;
    do {
        if ( p == end ) {
            diag.error("malformed sleb128");
            break;
        }
        byte = *p++;
        result |= (((int64_t)(byte & 0x7f)) << bit);
        bit += 7;
    } while (byte & 0x80);
    // sign extend negative numbers
    if ( (byte & 0x40) != 0 )
        result |= (~0ULL) << bit;
    return result;
}


} // namespace dyld3






/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <libproc.h>
#include <sys/param.h>
#include <mach/shared_region.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libkern/OSAtomic.h>

#include "dyld_process_info.h"
#include "dyld_process_info_internal.h"
#include "dyld_images.h"
#include "dyld_priv.h"

#include "Tracing.h"

// this was in dyld_priv.h but it is no longer exported
extern "C" {
    const struct dyld_all_image_infos* _dyld_get_all_image_infos();
}

RemoteBuffer::RemoteBuffer() : _localAddress(0), _size(0) {}
bool RemoteBuffer::map(task_t task, mach_vm_address_t remote_address, bool shared) {
    vm_prot_t cur_protection = VM_PROT_NONE;
    vm_prot_t max_protection = VM_PROT_NONE;
    if (_size == 0) {
        _kr = KERN_NO_SPACE;
        return false;
    }
    _localAddress = 0;
    _kr = mach_vm_remap(mach_task_self(),
                        &_localAddress,
                        _size,
                        0,  // mask
                        VM_FLAGS_ANYWHERE | VM_FLAGS_RETURN_DATA_ADDR | (shared ? 0 : VM_FLAGS_RESILIENT_CODESIGN),
                        task,
                        remote_address,
                        !shared,
                        &cur_protection,
                        &max_protection,
                        VM_INHERIT_NONE);
    dyld3::kdebug_trace_dyld_marker(DBG_DYLD_DEBUGGING_VM_REMAP, _localAddress, (uint64_t)_size, _kr, remote_address);
    if (shared && (cur_protection != (VM_PROT_READ|VM_PROT_WRITE))) {
        if (_kr == KERN_SUCCESS && _localAddress != 0) {
            _kr = vm_deallocate(mach_task_self(), _localAddress, _size);
            dyld3::kdebug_trace_dyld_marker(DBG_DYLD_DEBUGGING_VM_UNMAP, _localAddress, (uint64_t)_size, _kr, 0);
        }
        _localAddress = 0;
        _kr = KERN_PROTECTION_FAILURE;
    }
    return (_kr == KERN_SUCCESS);
}

RemoteBuffer::RemoteBuffer(task_t task, mach_vm_address_t remote_address, size_t remote_size, bool shared, bool allow_truncation)
        : _localAddress(0), _size(remote_size), _kr(KERN_SUCCESS) {
    // Try the initial map
    if (map(task, remote_address, shared)) return;
    // It failed, try to calculate the largest size that can fit in the same page as the remote_address
    uint64_t newSize = PAGE_SIZE - remote_address%PAGE_SIZE;;
    // If truncation is allowed and the newSize is different than the original size try that
    if (!allow_truncation && newSize != _size) return;
    _size = newSize;
    if (map(task, remote_address, shared)) return;
    // That did not work, null out the buffer
    _size = 0;
    _localAddress = 0;
}
RemoteBuffer::~RemoteBuffer() {
    if (_localAddress) {
        _kr = vm_deallocate(mach_task_self(), _localAddress, _size);
        dyld3::kdebug_trace_dyld_marker(DBG_DYLD_DEBUGGING_VM_UNMAP, _localAddress, (uint64_t)_size, _kr, 0);
    }
}
void *RemoteBuffer::getLocalAddress() { return (void *)_localAddress; }
size_t RemoteBuffer::getSize() { return _size; }
kern_return_t RemoteBuffer::getKernelReturn() { return _kr; }

void withRemoteBuffer(task_t task, mach_vm_address_t remote_address, size_t remote_size, bool shared, bool allow_truncation, kern_return_t *kr, void (^block)(void *buffer, size_t size)) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    RemoteBuffer buffer(task, remote_address, remote_size, shared, allow_truncation);
    *kr = buffer.getKernelReturn();
    if (*kr == KERN_SUCCESS) {
        block(buffer.getLocalAddress(), buffer.getSize());
    }
}


//
// Opaque object returned by _dyld_process_info_create()
//

struct __attribute__((visibility("hidden"))) dyld_process_info_deleter { // deleter
    //    dyld_process_info_deleter() {};
    //    dyld_process_info_deleter(const dyld_process_info_deleter&) { }
    //    dyld_process_info_deleter(dyld_process_info_deleter&) {}
    //    dyld_process_info_deleter(dyld_process_info_deleter&&) {}
    void operator()(dyld_process_info_base* p) const {
        if (p) {
            free(p);
        }
    };
};

static dyld_process_info_deleter deleter;
typedef std::unique_ptr<dyld_process_info_base, dyld_process_info_deleter> dyld_process_info_ptr;

struct __attribute__((visibility("hidden"))) dyld_process_info_base {
    template<typename T1, typename T2>
    static dyld_process_info_ptr make(task_t task, const T1& allImageInfo, uint64_t timestamp, kern_return_t* kr);
    template<typename T>
    static dyld_process_info_ptr makeSuspended(task_t task, const T& allImageInfo, kern_return_t* kr);

    std::atomic<uint32_t>&      retainCount() const { return _retainCount; }
    dyld_process_cache_info*    cacheInfo() const { return (dyld_process_cache_info*)(((char*)this) + _cacheInfoOffset); }
    dyld_process_state_info*    stateInfo() const { return (dyld_process_state_info*)(((char*)this) + _stateInfoOffset); }
    void                        forEachImage(void (^callback)(uint64_t machHeaderAddress, const uuid_t uuid, const char* path)) const;
    void                        forEachSegment(uint64_t machHeaderAddress, void (^callback)(uint64_t segmentAddress, uint64_t segmentSize, const char* segmentName)) const;

    void retain()
    {
        _retainCount++;
    }

    void release()
    {
        uint32_t newCount = --_retainCount;

        if ( newCount == 0 ) {
            free(this);
        }
    }

private:
    struct ImageInfo {
        uuid_t                  uuid;
        uint64_t                loadAddress;
        const char*             path;
        uint32_t                segmentStartIndex;
        uint32_t                segmentsCount;
    };

    struct SegmentInfo {
        const char*             name;
        uint64_t                addr;
        uint64_t                size;
    };

                                dyld_process_info_base(unsigned imageCount, size_t totalSize);
    void*                       operator new (size_t, void* buf) { return buf; }

    static bool                 inCache(uint64_t addr) { return (addr > SHARED_REGION_BASE) && (addr < SHARED_REGION_BASE+SHARED_REGION_SIZE); }
    kern_return_t               addImage(task_t task, bool sameCacheAsThisProcess, uint64_t imageAddress, uint64_t imagePath, const char* imagePathLocal);

    kern_return_t               addDyldImage(task_t task, uint64_t dyldAddress, uint64_t dyldPathAddress, const char* localPath);

    bool                        invalid() { return ((char*)_stringRevBumpPtr < (char*)_curSegment); }
    const char*                 copyPath(task_t task, uint64_t pathAddr, kern_return_t* kr);
    const char*                 addString(const char*, size_t);
    const char*                 copySegmentName(const char*);

    void                        addInfoFromLoadCommands(const mach_header* mh, uint64_t addressInTask, size_t size);
    kern_return_t               addInfoFromRemoteLoadCommands(task_t task, uint64_t remoteMH);

    void                        inspectLocalImageLoadCommands(uint64_t imageAddress, void* func);
    kern_return_t               inspectRemoteImageLoadCommands(task_t task, uint64_t imageAddress, void* func);

    mutable std::atomic<uint32_t>            _retainCount;
    const uint32_t              _cacheInfoOffset;
    const uint32_t              _stateInfoOffset;
    const uint32_t              _imageInfosOffset;
    const uint32_t              _segmentInfosOffset;
    ImageInfo* const            _firstImage;
    ImageInfo*                  _curImage;
    SegmentInfo* const          _firstSegment;
    SegmentInfo*                _curSegment;
    uint32_t                    _curSegmentIndex;
    char*                       _stringRevBumpPtr;

    // dyld_process_cache_info  cacheInfo;
    // dyld_process_state_info  stateInfo;
    // ImageInfo                images[];
    // SegmentInfo              segments[];
    // char                     stringPool[]
};

dyld_process_info_base::dyld_process_info_base(unsigned imageCount, size_t totalSize)
 :  _retainCount(1), _cacheInfoOffset(sizeof(dyld_process_info_base)),
    _stateInfoOffset(sizeof(dyld_process_info_base) + sizeof(dyld_process_cache_info)),
    _imageInfosOffset(sizeof(dyld_process_info_base) + sizeof(dyld_process_cache_info) + sizeof(dyld_process_state_info)),
    _segmentInfosOffset(sizeof(dyld_process_info_base) + sizeof(dyld_process_cache_info) + sizeof(dyld_process_state_info) + imageCount*sizeof(ImageInfo)),
    _firstImage((ImageInfo*)(((uint8_t*)this) + _imageInfosOffset)),
    _curImage((ImageInfo*)(((uint8_t*)this) + _imageInfosOffset)),
    _firstSegment((SegmentInfo*)(((uint8_t*)this) + _segmentInfosOffset)),
    _curSegment((SegmentInfo*)(((uint8_t*)this) + _segmentInfosOffset)),
    _curSegmentIndex(0),
    _stringRevBumpPtr((char*)(this)+totalSize)
{
}

template<typename T1, typename T2>
dyld_process_info_ptr dyld_process_info_base::make(task_t task, const T1& allImageInfo, uint64_t timestamp, kern_return_t* kr)
{
    __block dyld_process_info_ptr result = nullptr;

    // bail out of dyld is too old
    if ( allImageInfo.version < 15 ) {
        *kr = KERN_FAILURE;
        return nullptr;
    }

    // Check if the process is suspended
    if (allImageInfo.infoArrayChangeTimestamp == 0) {
        result = dyld_process_info_base::makeSuspended<T1>(task, allImageInfo, kr);
        // If we have a result return it, otherwise rescan
        if (result) {
            // If it returned the process is suspended and there is nothing more to do
            return std::move(result);
        } else {
            // Check to see if the process change timestamp is greater than 0, if not then sleep to let the process
            // finish initializing
            if (allImageInfo.infoArrayChangeTimestamp == 0) {
                usleep(1000 * 50); // 50ms
            }
        }
    }

    // Test to see if there are no changes and we can exit early
    if (timestamp != 0 && timestamp == allImageInfo.infoArrayChangeTimestamp) {
        *kr = KERN_SUCCESS;
        return nullptr;
    }

    for(uint32_t i = 0; i < 10; ++i) {
        uint64_t currentTimestamp = allImageInfo.infoArrayChangeTimestamp;
        mach_vm_address_t infoArray = allImageInfo.infoArray;
        if (currentTimestamp == 0) continue;
        if (infoArray == 0) {
            // Check if the task is suspended mid dylib load and exit early
            mach_task_basic_info ti;
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if ((*kr = task_info(task, MACH_TASK_BASIC_INFO, (task_info_t)&ti, &count))) {
                continue;
            }

            // The task is suspended, exit
            if (ti.suspend_count != 0) {
                // Not exactly correct, but conveys that operation may succeed in the future
                *kr = KERN_RESOURCE_SHORTAGE;
                return  nullptr;
            }
            continue;
        };

        // For the moment we are going to truncate any image list longer than 8192 because some programs do
        // terrible things that corrupt their own image lists and we need to stop clients from crashing
        // reading them. We can try to do something more advanced in the future. rdar://27446361
        uint32_t imageCount = allImageInfo.infoArrayCount;
        imageCount = MIN(imageCount, 8192);
        size_t imageArraySize = imageCount * sizeof(T2);

        withRemoteBuffer(task, infoArray, imageArraySize, false, false, kr, ^(void *buffer, size_t size) {
            // figure out how many path strings will need to be copied and their size
            T2* imageArray = (T2 *)buffer;
            const dyld_all_image_infos* myInfo = _dyld_get_all_image_infos();
            bool sameCacheAsThisProcess = !allImageInfo.processDetachedFromSharedRegion
                && !myInfo->processDetachedFromSharedRegion
                && ((memcmp(myInfo->sharedCacheUUID, &allImageInfo.sharedCacheUUID[0], 16) == 0)
                && (myInfo->sharedCacheSlide == allImageInfo.sharedCacheSlide));
            unsigned countOfPathsNeedingCopying = 0;
            if ( sameCacheAsThisProcess ) {
                for (uint32_t i=0; i < imageCount; ++i) {
                    if ( !inCache(imageArray[i].imageFilePath) )
                        ++countOfPathsNeedingCopying;
                }
            }
            else {
                countOfPathsNeedingCopying = imageCount+1;
            }
            unsigned imageCountWithDyld = imageCount+1;

            // allocate result object
            size_t allocationSize = sizeof(dyld_process_info_base)
                                        + sizeof(dyld_process_cache_info)
                                        + sizeof(dyld_process_state_info)
                                        + sizeof(ImageInfo)*(imageCountWithDyld)
                                        + sizeof(SegmentInfo)*imageCountWithDyld*5
                                        + countOfPathsNeedingCopying*PATH_MAX;
            void* storage = malloc(allocationSize);
            auto info = dyld_process_info_ptr(new (storage) dyld_process_info_base(imageCountWithDyld, allocationSize), deleter);
            //info =  new (storage) dyld_process_info_base(imageCountWithDyld, allocationSize); // placement new()

            // fill in base info
            dyld_process_cache_info* cacheInfo = info->cacheInfo();
            memcpy(cacheInfo->cacheUUID, &allImageInfo.sharedCacheUUID[0], 16);
            cacheInfo->cacheBaseAddress    = allImageInfo.sharedCacheBaseAddress;
            cacheInfo->privateCache        = allImageInfo.processDetachedFromSharedRegion;
            // if no cache is used, allImageInfo has all zeros for cache UUID
            cacheInfo->noCache = true;
            for (int i=0; i < 16; ++i) {
                if ( cacheInfo->cacheUUID[i] != 0 ) {
                    cacheInfo->noCache = false;
                }
            }

            dyld_process_state_info* stateInfo = info->stateInfo();
            stateInfo->timestamp           = currentTimestamp;
            stateInfo->imageCount          = imageCountWithDyld;
            stateInfo->initialImageCount   = (uint32_t)(allImageInfo.initialImageCount+1);
            stateInfo->dyldState = dyld_process_state_dyld_initialized;

            if ( allImageInfo.libSystemInitialized != 0 ) {
                stateInfo->dyldState = dyld_process_state_libSystem_initialized;
                if ( allImageInfo.initialImageCount != imageCount ) {
                    stateInfo->dyldState = dyld_process_state_program_running;
                }
            }
            if ( allImageInfo.errorMessage != 0 ) {
                stateInfo->dyldState = allImageInfo.terminationFlags ? dyld_process_state_terminated_before_inits : dyld_process_state_dyld_terminated;
            }
            // fill in info for dyld
            if ( allImageInfo.dyldPath != 0 ) {
                if ((*kr = info->addDyldImage(task, allImageInfo.dyldImageLoadAddress, allImageInfo.dyldPath, NULL))) {
                    result = nullptr;
                    return;
                }
            }
            // fill in info for each image
            for (uint32_t i=0; i < imageCount; ++i) {
                if ((*kr = info->addImage(task, sameCacheAsThisProcess, imageArray[i].imageLoadAddress, imageArray[i].imageFilePath, NULL))) {
                    result = nullptr;
                    return;
                }
            }
            // sanity check internal data did not overflow
            if ( info->invalid() ) {
                *kr = KERN_FAILURE;
                result = nullptr;
                return;
            }

            result = std::move(info);
        });

        if (result) break;
    }

    return std::move(result);
}

template<typename T>
dyld_process_info_ptr dyld_process_info_base::makeSuspended(task_t task, const T& allImageInfo, kern_return_t* kr)
{
    pid_t   pid;
    if ((*kr = pid_for_task(task, &pid))) {
        return NULL;
    }

    mach_task_basic_info ti;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if ((*kr = task_info(task, MACH_TASK_BASIC_INFO, (task_info_t)&ti, &count))) {
        return  nullptr;
    }

    // The task is not suspended, exit
    if (ti.suspend_count == 0) {
        return  nullptr;
    }

    __block unsigned    imageCount = 0;  // main executable and dyld
    __block uint64_t    mainExecutableAddress = 0;
    __block uint64_t    dyldAddress = 0;
    char                dyldPathBuffer[PATH_MAX+1];
    char                mainExecutablePathBuffer[PATH_MAX+1];
    __block char *      dyldPath = &dyldPathBuffer[0];
    __block char *      mainExecutablePath = &mainExecutablePathBuffer[0];
    mach_vm_size_t      size;
    for (mach_vm_address_t address = 0; ; address += size) {
        vm_region_basic_info_data_64_t  info;
        mach_port_t                     objectName;
        unsigned int                    infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        if (kern_return_t r = mach_vm_region(task, &address, &size, VM_REGION_BASIC_INFO,
                                             (vm_region_info_t)&info, &infoCount, &objectName)) {
            break;
        }
        if ( info.protection != (VM_PROT_READ|VM_PROT_EXECUTE) )
            continue;
            // read start of vm region to verify it is a mach header
            withRemoteObject(task, address, false, NULL, ^(mach_header_64 mhBuffer){
                if ( (mhBuffer.magic != MH_MAGIC) && (mhBuffer.magic != MH_MAGIC_64) )
                    return;
                // now know the region is the start of a mach-o file
                if ( mhBuffer.filetype == MH_EXECUTE ) {
                    mainExecutableAddress = address;
                    int len = proc_regionfilename(pid, mainExecutableAddress, mainExecutablePath, PATH_MAX);
                    if ( len != 0 ) {
                        mainExecutablePath[len] = '\0';
                    }
                    ++imageCount;
                }
                else if ( mhBuffer.filetype == MH_DYLINKER ) {
                    dyldAddress = address;
                    int len = proc_regionfilename(pid, dyldAddress, dyldPath, PATH_MAX);
                    if ( len != 0 ) {
                        dyldPath[len] = '\0';
                    }
                    ++imageCount;
                }
            });
            //fprintf(stderr, "vm region: addr=0x%llX, size=0x%llX, prot=0x%X\n", (uint64_t)address, (uint64_t)size, info.protection);
    }
    //fprintf(stderr, "dyld: addr=0x%llX, path=%s\n", dyldAddress, dyldPathBuffer);
    //fprintf(stderr, "app:  addr=0x%llX, path=%s\n", mainExecutableAddress, mainExecutablePathBuffer);

    // allocate result object
    size_t allocationSize =   sizeof(dyld_process_info_base)
                            + sizeof(dyld_process_cache_info)
                            + sizeof(dyld_process_state_info)
                            + sizeof(ImageInfo)*(imageCount)
                            + sizeof(SegmentInfo)*imageCount*5
                            + imageCount*PATH_MAX;
    void* storage = malloc(allocationSize);
    auto obj = dyld_process_info_ptr(new (storage) dyld_process_info_base(imageCount, allocationSize), deleter);
    // fill in base info
    dyld_process_cache_info* cacheInfo = obj->cacheInfo();
    bzero(cacheInfo->cacheUUID, 16);
    cacheInfo->cacheBaseAddress    = 0;
    cacheInfo->noCache             = true;
    cacheInfo->privateCache        = false;

    dyld_process_state_info* stateInfo = obj->stateInfo();
    stateInfo->timestamp           = 0;
    stateInfo->imageCount          = imageCount;
    stateInfo->initialImageCount   = imageCount;
    stateInfo->dyldState           = dyld_process_state_not_started;

    // fill in info for dyld
    if ( dyldAddress != 0 ) {
        if ((*kr = obj->addDyldImage(task, dyldAddress, 0, dyldPath))) {
            return nullptr;
        }
    }

    // fill in info for each image
    if ( mainExecutableAddress != 0 ) {
        if ((*kr = obj->addImage(task, false, mainExecutableAddress, 0, mainExecutablePath))) {
            return nullptr;
        }
    }

    if (allImageInfo.infoArrayChangeTimestamp != 0) {
        return  nullptr;
    }

    count = MACH_TASK_BASIC_INFO_COUNT;
    if ((*kr = task_info(task, MACH_TASK_BASIC_INFO, (task_info_t)&ti, &count))) {
        return  nullptr;
    }

    // The task is not suspended, exit
    if (ti.suspend_count == 0) {
        return  nullptr;
    }

    return obj;
}



const char* dyld_process_info_base::addString(const char* str, size_t maxlen)
{
    size_t len = strnlen(str, maxlen) + 1;
    _stringRevBumpPtr -= len;
    strlcpy(_stringRevBumpPtr, str, len);
    return _stringRevBumpPtr;
}

const char* dyld_process_info_base::copyPath(task_t task, uint64_t stringAddressInTask, kern_return_t* kr)
{
    __block const char* retval = NULL;
    withRemoteBuffer(task, stringAddressInTask, PATH_MAX, false, true, kr, ^(void *buffer, size_t size) {
        retval = addString(static_cast<const char *>(buffer), size);
    });
    return retval;
}

kern_return_t dyld_process_info_base::addImage(task_t task, bool sameCacheAsThisProcess, uint64_t imageAddress, uint64_t imagePath, const char* imagePathLocal)
{
    kern_return_t kr = KERN_SUCCESS;
    _curImage->loadAddress = imageAddress;
    _curImage->segmentStartIndex = _curSegmentIndex;
    if ( imagePathLocal != NULL ) {
        _curImage->path = addString(imagePathLocal, PATH_MAX);
    }
    else if ( sameCacheAsThisProcess && inCache(imagePath) ) {
        _curImage->path = (const char*)imagePath;
    }
    else {
        _curImage->path = copyPath(task, imagePath, &kr);
        if ( kr != KERN_SUCCESS)
            return kr;
    }
    if ( sameCacheAsThisProcess && inCache(imageAddress) ) {
        addInfoFromLoadCommands((mach_header*)imageAddress, imageAddress, 32*1024);
    }
    else {
        kr = addInfoFromRemoteLoadCommands(task, imageAddress);
        if ( kr != KERN_SUCCESS)
            return kr;
    }
    _curImage->segmentsCount = _curSegmentIndex - _curImage->segmentStartIndex;
    _curImage++;
    return KERN_SUCCESS;
}


kern_return_t dyld_process_info_base::addInfoFromRemoteLoadCommands(task_t task, uint64_t remoteMH) {
    __block kern_return_t kr = KERN_SUCCESS;
    __block size_t headerPagesSize = 0;
    __block bool done = false;

    //Since the minimum we can reasonably map is a page, map that.
    withRemoteBuffer(task, remoteMH, PAGE_SIZE, false, false, &kr, ^(void * buffer, size_t size) {
        const mach_header* mh = (const mach_header*)buffer;
        headerPagesSize = sizeof(mach_header) + mh->sizeofcmds;
        if (headerPagesSize <= PAGE_SIZE) {
            addInfoFromLoadCommands(mh, remoteMH, size);
            done = true;
        }
    });

    //The load commands did not fit in the first page, but now we know the size, so remap and try again
    if (!done) {
        if (kr != KERN_SUCCESS) {
            return kr;
        }
        withRemoteBuffer(task, remoteMH, headerPagesSize, false, false, &kr, ^(void * buffer, size_t size) {
            addInfoFromLoadCommands((mach_header*)buffer, remoteMH, size);
        });
    }

    return kr;
}

kern_return_t dyld_process_info_base::addDyldImage(task_t task, uint64_t dyldAddress, uint64_t dyldPathAddress, const char* localPath)
{
    __block kern_return_t kr = KERN_SUCCESS;
    _curImage->loadAddress = dyldAddress;
    _curImage->segmentStartIndex = _curSegmentIndex;
    if ( localPath != NULL ) {
        _curImage->path = addString(localPath, PATH_MAX);
    }
    else {
        _curImage->path = copyPath(task, dyldPathAddress, &kr);
        if ( kr != KERN_SUCCESS)
            return kr;
    }

    kr = addInfoFromRemoteLoadCommands(task, dyldAddress);
    if ( kr != KERN_SUCCESS)
        return kr;

    _curImage->segmentsCount = _curSegmentIndex - _curImage->segmentStartIndex;
    _curImage++;
    return KERN_SUCCESS;
}


void dyld_process_info_base::addInfoFromLoadCommands(const mach_header* mh, uint64_t addressInTask, size_t size)
{
    const load_command* startCmds = NULL;
    if ( mh->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)mh + sizeof(mach_header_64));
    else if ( mh->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)mh + sizeof(mach_header));
    else
        return;  // not a mach-o file, or wrong endianness

    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + mh->sizeofcmds);
    const load_command* cmd = startCmds;
    for(uint32_t i = 0; i < mh->ncmds; ++i) {
        const load_command* nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
        if ( (cmd->cmdsize < 8) || (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            return;  // malformed load command
        }
        if ( cmd->cmd == LC_UUID ) {
            const uuid_command* uuidCmd = (uuid_command*)cmd;
            memcpy(_curImage->uuid, uuidCmd->uuid, 16);
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            _curSegment->name = copySegmentName(segCmd->segname);
            _curSegment->addr = segCmd->vmaddr;
            _curSegment->size = segCmd->vmsize;
            _curSegment++;
            _curSegmentIndex++;
        }
        else if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            _curSegment->name = copySegmentName(segCmd->segname);
            _curSegment->addr = segCmd->vmaddr;
            _curSegment->size = segCmd->vmsize;
            _curSegment++;
            _curSegmentIndex++;
        }
        cmd = nextCmd;
    }
}

const char* dyld_process_info_base::copySegmentName(const char* name)
{
    // don't copy names of standard segments into string pool
    static const char* stdSegNames[] = {"__TEXT", "__DATA", "__LINKEDIT", "__DATA_DIRTY", "__DATA_CONST", "__OBJC", NULL };
    for (const char** s=stdSegNames; *s != NULL; ++s) {
        if ( strcmp(name, *s) == 0 )
        return *s;
    }
    // copy custom segment names into string pool
    return addString(name, 16);
}

void dyld_process_info_base::forEachImage(void (^callback)(uint64_t machHeaderAddress, const uuid_t uuid, const char* path)) const
{
    for (const ImageInfo* p = _firstImage; p < _curImage; ++p) {
        callback(p->loadAddress, p->uuid, p->path);
    }
}

void dyld_process_info_base::forEachSegment(uint64_t machHeaderAddress, void (^callback)(uint64_t segmentAddress, uint64_t segmentSize, const char* segmentName)) const
{
    for (const ImageInfo* p = _firstImage; p < _curImage; ++p) {
        if ( p->loadAddress == machHeaderAddress ) {
            uint64_t slide = 0;
            for (uint32_t i=0; i < p->segmentsCount; ++i) {
                const SegmentInfo* seg = &_firstSegment[p->segmentStartIndex+i];
                if ( strcmp(seg->name, "__TEXT") == 0 ) {
                    slide = machHeaderAddress - seg->addr;
                    break;
                }
            }
            for (uint32_t i=0; i < p->segmentsCount; ++i) {
                const SegmentInfo* seg = &_firstSegment[p->segmentStartIndex+i];
                callback(seg->addr + slide, seg->size, seg->name);
            }
            break;
        }
    }
}

dyld_process_info _dyld_process_info_create(task_t task, uint64_t timestamp, kern_return_t* kr)
{
    __block dyld_process_info result = nullptr;
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;

    task_dyld_info_data_t task_dyld_info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if ( kern_return_t r = task_info(task, TASK_DYLD_INFO, (task_info_t)&task_dyld_info, &count) ) {
        *kr = r;
        return  nullptr;
    }

    //The kernel will return MACH_VM_MIN_ADDRESS for an executable that has not had dyld loaded
    if (task_dyld_info.all_image_info_addr == MACH_VM_MIN_ADDRESS)
        return nullptr;

    if ( task_dyld_info.all_image_info_size > sizeof(dyld_all_image_infos_64) )
        return nullptr;

    // We use a true shared memory buffer here, that way by making sure that libdyld in both processes
    // reads and writes the the timestamp atomically we can make sure we get a coherent view of the
    // remote process.
    // That also means that we *MUST* directly read the memory, which is why we template the make() call
    withRemoteBuffer(task, task_dyld_info.all_image_info_addr, task_dyld_info.all_image_info_size, true, false, kr, ^(void *buffer, size_t size) {
        dyld_process_info_ptr base;
        if (task_dyld_info.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_32 ) {
            const dyld_all_image_infos_32* info = (const dyld_all_image_infos_32*)buffer;
            base = dyld_process_info_base::make<dyld_all_image_infos_32, dyld_image_info_32>(task, *info, timestamp, kr);
        } else {
            const dyld_all_image_infos_64* info = (const dyld_all_image_infos_64*)buffer;
            base = dyld_process_info_base::make<dyld_all_image_infos_64, dyld_image_info_64>(task, *info, timestamp, kr);
        }
        if (base) {
            result = base.release();
        }
    });
    return  result;
}

void _dyld_process_info_get_state(dyld_process_info info, dyld_process_state_info* stateInfo)
{
    *stateInfo = *info->stateInfo();
}

void _dyld_process_info_get_cache(dyld_process_info info, dyld_process_cache_info* cacheInfo)
{
    *cacheInfo = *info->cacheInfo();
}

void _dyld_process_info_retain(dyld_process_info object)
{
    const_cast<dyld_process_info_base*>(object)->retain();
}

void _dyld_process_info_release(dyld_process_info object)
{
    const_cast<dyld_process_info_base*>(object)->release();
}

void _dyld_process_info_for_each_image(dyld_process_info info, void (^callback)(uint64_t machHeaderAddress, const uuid_t uuid, const char* path))
{
    info->forEachImage(callback);
}


void _dyld_process_info_for_each_segment(dyld_process_info info, uint64_t machHeaderAddress, void (^callback)(uint64_t segmentAddress, uint64_t segmentSize, const char* segmentName))
{
    info->forEachSegment(machHeaderAddress, callback);
}




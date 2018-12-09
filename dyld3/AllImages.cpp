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


#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h> // mach_absolute_time()
#include <libkern/OSAtomic.h>

#include <vector>
#include <algorithm>

#include "AllImages.h"
#include "libdyldEntryVector.h"
#include "Logging.h"
#include "Loading.h"
#include "Tracing.h"
#include "DyldSharedCache.h"
#include "PathOverrides.h"
#include "Closure.h"
#include "ClosureBuilder.h"
#include "ClosureFileSystemPhysical.h"

extern const char** appleParams;

// should be a header for these
struct __cxa_range_t {
    const void* addr;
    size_t      length;
};
extern "C" void __cxa_finalize_ranges(const __cxa_range_t ranges[], unsigned int count);

VIS_HIDDEN bool gUseDyld3 = false;


namespace dyld3 {



/////////////////////  AllImages ////////////////////////////


AllImages gAllImages;



void AllImages::init(const closure::LaunchClosure* closure, const DyldSharedCache* dyldCacheLoadAddress, const char* dyldCachePath,
                     const Array<LoadedImage>& initialImages)
{
    _mainClosure        = closure;
    _initialImages      = &initialImages;
    _dyldCacheAddress   = dyldCacheLoadAddress;
    _dyldCachePath      = dyldCachePath;

    if ( _dyldCacheAddress ) {
        const dyld_cache_mapping_info* const fileMappings = (dyld_cache_mapping_info*)((uint64_t)_dyldCacheAddress + _dyldCacheAddress->header.mappingOffset);
        _dyldCacheSlide = (uint64_t)dyldCacheLoadAddress - fileMappings[0].address;
        _imagesArrays.push_back(dyldCacheLoadAddress->cachedDylibsImageArray());
        if ( auto others = dyldCacheLoadAddress->otherOSImageArray() )
            _imagesArrays.push_back(others);
    }
    _imagesArrays.push_back(_mainClosure->images());

    // record first ImageNum to do use for dlopen() calls
    _mainClosure->images()->forEachImage(^(const dyld3::closure::Image* image, bool& stop) {
        closure::ImageNum num = image->imageNum();
        if ( num >= _nextImageNum )
            _nextImageNum = num+1;
    });
 
    // Make temporary old image array, so libSystem initializers can be debugged
    STACK_ALLOC_ARRAY(dyld_image_info, oldDyldInfo, initialImages.count());
    for (const LoadedImage& li : initialImages) {
        oldDyldInfo.push_back({li.loadedAddress(), li.image()->path(), 0});
    }
    _oldAllImageInfos->infoArray        = &oldDyldInfo[0];
    _oldAllImageInfos->infoArrayCount   = (uint32_t)oldDyldInfo.count();
    _oldAllImageInfos->notification(dyld_image_adding, _oldAllImageInfos->infoArrayCount, _oldAllImageInfos->infoArray);
    _oldAllImageInfos->infoArray        = nullptr;
    _oldAllImageInfos->infoArrayCount   = 0;

    _processDOFs = Loader::dtraceUserProbesEnabled();
}

void AllImages::setProgramVars(ProgramVars* vars)
{
    _programVars = vars;
    const dyld3::MachOFile* mf = (dyld3::MachOFile*)_programVars->mh;
    mf->forEachSupportedPlatform(^(dyld3::Platform platform, uint32_t minOS, uint32_t sdk) {
        _platform = (dyld_platform_t)platform;
        //FIXME assert there is only one?
    });
}

void AllImages::setRestrictions(bool allowAtPaths, bool allowEnvPaths)
{
    _allowAtPaths  = allowAtPaths;
    _allowEnvPaths = allowEnvPaths;
}

void AllImages::applyInitialImages()
{
    addImages(*_initialImages);
    runImageNotifiers(*_initialImages);
    _initialImages = nullptr;  // this was stack allocated
}

void AllImages::withReadLock(void (^work)()) const
{
#ifdef OS_UNFAIR_RECURSIVE_LOCK_INIT
     os_unfair_recursive_lock_lock(&_loadImagesLock);
        work();
     os_unfair_recursive_lock_unlock(&_loadImagesLock);
#else
    pthread_mutex_lock(&_loadImagesLock);
        work();
    pthread_mutex_unlock(&_loadImagesLock);
#endif
}

void AllImages::withWriteLock(void (^work)())
{
#ifdef OS_UNFAIR_RECURSIVE_LOCK_INIT
     os_unfair_recursive_lock_lock(&_loadImagesLock);
        work();
     os_unfair_recursive_lock_unlock(&_loadImagesLock);
#else
    pthread_mutex_lock(&_loadImagesLock);
        work();
    pthread_mutex_unlock(&_loadImagesLock);
#endif
}

void AllImages::withNotifiersLock(void (^work)()) const
{
#ifdef OS_UNFAIR_RECURSIVE_LOCK_INIT
     os_unfair_recursive_lock_lock(&_notifiersLock);
        work();
     os_unfair_recursive_lock_unlock(&_notifiersLock);
#else
    pthread_mutex_lock(&_notifiersLock);
        work();
    pthread_mutex_unlock(&_notifiersLock);
#endif
}

void AllImages::mirrorToOldAllImageInfos()
{
     withReadLock(^(){
        // set infoArray to NULL to denote it is in-use
        _oldAllImageInfos->infoArray = nullptr;

        // if array not large enough, re-alloc it
        uint32_t imageCount = (uint32_t)_loadedImages.count();
        if ( _oldArrayAllocCount < imageCount ) {
            uint32_t newAllocCount    = imageCount + 16;
            dyld_image_info* newArray = (dyld_image_info*)::malloc(sizeof(dyld_image_info)*newAllocCount);
            if ( _oldAllImageArray != nullptr ) {
                ::memcpy(newArray, _oldAllImageArray, sizeof(dyld_image_info)*_oldAllImageInfos->infoArrayCount);
                ::free(_oldAllImageArray);
            }
            _oldAllImageArray   = newArray;
            _oldArrayAllocCount = newAllocCount;
        }

        // fill out array to mirror current image list
        int index = 0;
        for (const LoadedImage& li : _loadedImages) {
            _oldAllImageArray[index].imageLoadAddress = li.loadedAddress();
            _oldAllImageArray[index].imageFilePath    = imagePath(li.image());
            _oldAllImageArray[index].imageFileModDate = 0;
            ++index;
        }

        // set infoArray back to base address of array (so other process can now read)
        _oldAllImageInfos->infoArrayCount           = imageCount;
        _oldAllImageInfos->infoArrayChangeTimestamp = mach_absolute_time();
        _oldAllImageInfos->infoArray                = _oldAllImageArray;

        // <radr://problem/42668846> update UUID array if needed
        uint32_t nonCachedCount = 1; // always add dyld
        for (const LoadedImage& li : _loadedImages) {
            if ( !li.loadedAddress()->inDyldCache() )
                ++nonCachedCount;
        }
        if ( nonCachedCount != _oldAllImageInfos->uuidArrayCount ) {
            // set infoArray to NULL to denote it is in-use
            _oldAllImageInfos->uuidArray = nullptr;
            // make sure allocation can hold all uuids
            if ( _oldUUIDAllocCount < nonCachedCount ) {
                uint32_t        newAllocCount = (nonCachedCount + 3) & (-4); // round up to multiple of 4
                dyld_uuid_info* newArray      = (dyld_uuid_info*)::malloc(sizeof(dyld_uuid_info)*newAllocCount);
                if ( _oldUUIDArray != nullptr )
                    ::free(_oldUUIDArray);
                _oldUUIDArray       = newArray;
                _oldUUIDAllocCount  = newAllocCount;
            }
            // add dyld then all images not in dyld cache
            const MachOFile* dyldMF = (MachOFile*)_oldAllImageInfos->dyldImageLoadAddress;
            _oldUUIDArray[0].imageLoadAddress = dyldMF;
            dyldMF->getUuid(_oldUUIDArray[0].imageUUID);
            index = 1;
            for (const LoadedImage& li : _loadedImages) {
                if ( !li.loadedAddress()->inDyldCache() ) {
                    _oldUUIDArray[index].imageLoadAddress = li.loadedAddress();
                    li.loadedAddress()->getUuid(_oldUUIDArray[index].imageUUID);
                    ++index;
                }
            }
            // set uuidArray back to base address of array (so kernel can now read)
            _oldAllImageInfos->uuidArray           = _oldUUIDArray;
            _oldAllImageInfos->uuidArrayCount      = nonCachedCount;
        }
    });
}

void AllImages::addImages(const Array<LoadedImage>& newImages)
{
    // copy into _loadedImages
    withWriteLock(^(){
        _loadedImages.append(newImages);
        // if any image not in the shared cache added, recompute bounds
        for (const LoadedImage& li : newImages) {
            if ( !((MachOAnalyzer*)li.loadedAddress())->inDyldCache() ) {
                recomputeBounds();
                break;
            }
        }
    });
}

void AllImages::runImageNotifiers(const Array<LoadedImage>& newImages)
{
    uint32_t count = (uint32_t)newImages.count();
    assert(count != 0);

    if ( _oldAllImageInfos != nullptr ) {
        // sync to old all image infos struct
        mirrorToOldAllImageInfos();

        // tell debugger about new images
        dyld_image_info oldDyldInfo[count];
        for (uint32_t i=0; i < count; ++i) {
            oldDyldInfo[i].imageLoadAddress = newImages[i].loadedAddress();
            oldDyldInfo[i].imageFilePath    = imagePath(newImages[i].image());
            oldDyldInfo[i].imageFileModDate = 0;
        }
        _oldAllImageInfos->notification(dyld_image_adding, count, oldDyldInfo);
    }

    // log loads
    for (const LoadedImage& li : newImages) {
        log_loads("dyld: %s\n", imagePath(li.image()));
    }

#if !TARGET_IPHONE_SIMULATOR
    // call kdebug trace for each image
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A))) {
        for (const LoadedImage& li : newImages) {
            const closure::Image* image = li.image();
            struct stat  stat_buf;
            fsid_t       fsid = {{ 0, 0 }};
            fsobj_id_t   fsobjid = { 0, 0 };
            if ( !image->inDyldCache() && (stat(imagePath(image), &stat_buf) == 0) ) {
                fsobjid = *(fsobj_id_t*)&stat_buf.st_ino;
                fsid    = {{ stat_buf.st_dev, 0 }};
            }
            uuid_t uuid;
            image->getUuid(uuid);
            kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, &uuid, fsobjid, fsid, li.loadedAddress());
        }
    }
#endif
    // call each _dyld_register_func_for_add_image function with each image
    withNotifiersLock(^{
        for (NotifyFunc func : _loadNotifiers) {
            for (const LoadedImage& li : newImages) {
                dyld3::ScopedTimer timer(DBG_DYLD_TIMING_FUNC_FOR_ADD_IMAGE, (uint64_t)li.loadedAddress(), (uint64_t)func, 0);
                log_notifications("dyld: add notifier %p called with mh=%p\n", func, li.loadedAddress());
                if ( li.image()->inDyldCache() )
                    func(li.loadedAddress(), (uintptr_t)_dyldCacheSlide);
                else
                    func(li.loadedAddress(), li.loadedAddress()->getSlide());
            }
        }
        for (LoadNotifyFunc func : _loadNotifiers2) {
            for (const LoadedImage& li : newImages) {
                dyld3::ScopedTimer timer(DBG_DYLD_TIMING_FUNC_FOR_ADD_IMAGE, (uint64_t)li.loadedAddress(), (uint64_t)func, 0);
                log_notifications("dyld: add notifier %p called with mh=%p\n", func, li.loadedAddress());
                if ( li.image()->inDyldCache() )
                    func(li.loadedAddress(), li.image()->path(), false);
                else
                    func(li.loadedAddress(), li.image()->path(), !li.image()->neverUnload());
            }
        }
    });

    // call objc about images that use objc
    if ( _objcNotifyMapped != nullptr ) {
        const char*         pathsBuffer[count];
        const mach_header*  mhBuffer[count];
        uint32_t            imagesWithObjC = 0;
        for (const LoadedImage& li : newImages) {
            const closure::Image* image = li.image();
            if ( image->hasObjC() ) {
                pathsBuffer[imagesWithObjC] = imagePath(image);
                mhBuffer[imagesWithObjC]    = li.loadedAddress();
               ++imagesWithObjC;
            }
        }
        if ( imagesWithObjC != 0 ) {
            dyld3::ScopedTimer timer(DBG_DYLD_TIMING_OBJC_MAP, 0, 0, 0);
            (*_objcNotifyMapped)(imagesWithObjC, pathsBuffer, mhBuffer);
            if ( log_notifications("dyld: objc-mapped-notifier called with %d images:\n", imagesWithObjC) ) {
                for (uint32_t i=0; i < imagesWithObjC; ++i) {
                    log_notifications("dyld:  objc-mapped: %p %s\n",  mhBuffer[i], pathsBuffer[i]);
                }
            }
        }
    }

    // notify any processes tracking loads in this process
    notifyMonitorLoads(newImages);
}

void AllImages::removeImages(const Array<LoadedImage>& unloadImages)
{
    // call each _dyld_register_func_for_remove_image function with each image
    withNotifiersLock(^{
        for (NotifyFunc func : _unloadNotifiers) {
            for (const LoadedImage& li : unloadImages) {
                dyld3::ScopedTimer timer(DBG_DYLD_TIMING_FUNC_FOR_REMOVE_IMAGE, (uint64_t)li.loadedAddress(), (uint64_t)func, 0);
                log_notifications("dyld: remove notifier %p called with mh=%p\n", func, li.loadedAddress());
                if ( li.image()->inDyldCache() )
                    func(li.loadedAddress(), (uintptr_t)_dyldCacheSlide);
                else
                    func(li.loadedAddress(), li.loadedAddress()->getSlide());
            }
        }
    });

    // call objc about images going away
    if ( _objcNotifyUnmapped != nullptr ) {
        for (const LoadedImage& li : unloadImages) {
            if ( li.image()->hasObjC() ) {
                (*_objcNotifyUnmapped)(imagePath(li.image()), li.loadedAddress());
                log_notifications("dyld: objc-unmapped-notifier called with image %p %s\n", li.loadedAddress(), imagePath(li.image()));
            }
        }
    }

#if !TARGET_IPHONE_SIMULATOR
    // call kdebug trace for each image
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A))) {
        for (const LoadedImage& li : unloadImages) {
            const closure::Image* image = li.image();
            struct stat  stat_buf;
            fsid_t       fsid = {{ 0, 0 }};
            fsobj_id_t   fsobjid = { 0, 0 };
            if ( stat(imagePath(image), &stat_buf) == 0 ) {
                fsobjid = *(fsobj_id_t*)&stat_buf.st_ino;
                fsid    = {{ stat_buf.st_dev, 0 }};
            }
            uuid_t uuid;
            image->getUuid(uuid);
            kdebug_trace_dyld_image(DBG_DYLD_UUID_UNMAP_A, &uuid, fsobjid, fsid, li.loadedAddress());
        }
    }
#endif

    // remove each from _loadedImages
    withWriteLock(^(){
        for (const LoadedImage& uli : unloadImages) {
            for (LoadedImage& li : _loadedImages) {
                if ( uli.loadedAddress() == li.loadedAddress() ) {
                    _loadedImages.erase(li);
                    break;
                }
            }
        }
        recomputeBounds();
    });

    // sync to old all image infos struct
    mirrorToOldAllImageInfos();

    // tell debugger about removed images
    STACK_ALLOC_ARRAY(dyld_image_info, oldDyldInfo, unloadImages.count());
    for (const LoadedImage& li : unloadImages) {
        oldDyldInfo.push_back({li.loadedAddress(), li.image()->path(), 0});
    }
    _oldAllImageInfos->notification(dyld_image_removing, (uint32_t)oldDyldInfo.count(), &oldDyldInfo[0]);

    // notify any processes tracking loads in this process
    notifyMonitorUnloads(unloadImages);

    // finally, unmap images
	for (const LoadedImage& li : unloadImages) {
        if ( li.leaveMapped() ) {
            log_loads("dyld: unloaded but left mmapped %s\n", imagePath(li.image()));
        }
        else {
            // unmapImage() modifies parameter, so use copy
            LoadedImage copy = li;
            Loader::unmapImage(copy);
            log_loads("dyld: unloaded %s\n", imagePath(li.image()));
        }
    }
}

// must be called with writeLock held
void AllImages::recomputeBounds()
{
    _lowestNonCached  = UINTPTR_MAX;
    _highestNonCached = 0;
    for (const LoadedImage& li : _loadedImages) {
        const MachOLoaded* ml = li.loadedAddress();
        uintptr_t start = (uintptr_t)ml;
        if ( !((MachOAnalyzer*)ml)->inDyldCache() ) {
            if ( start < _lowestNonCached )
                _lowestNonCached = start;
            uintptr_t end = start + (uintptr_t)(li.image()->vmSizeToMap());
            if ( end > _highestNonCached )
                _highestNonCached = end;
        }
    }
}

uint32_t AllImages::count() const
{
    return (uint32_t)_loadedImages.count();
}

bool AllImages::dyldCacheHasPath(const char* path) const
{
    uint32_t dyldCacheImageIndex;
    if ( _dyldCacheAddress != nullptr )
        return _dyldCacheAddress->hasImagePath(path, dyldCacheImageIndex);
    return false;
}

const char* AllImages::imagePathByIndex(uint32_t index) const
{
    if ( index < _loadedImages.count() )
        return imagePath(_loadedImages[index].image());
    return nullptr;
}

const mach_header* AllImages::imageLoadAddressByIndex(uint32_t index) const
{
    if ( index < _loadedImages.count() )
        return _loadedImages[index].loadedAddress();
    return nullptr;
}

bool AllImages::findImage(const mach_header* loadAddress, LoadedImage& foundImage) const
{
    __block bool result = false;
    withReadLock(^(){
        for (const LoadedImage& li : _loadedImages) {
            if ( li.loadedAddress() == loadAddress ) {
                foundImage = li;
                result = true;
                break;
            }
        }
    });
    return result;
}

void AllImages::forEachImage(void (^handler)(const LoadedImage& loadedImage, bool& stop)) const
{
    withReadLock(^{
        bool stop = false;
        for (const LoadedImage& li : _loadedImages) {
           handler(li, stop);
           if ( stop )
               break;
        }
    });
}


const char* AllImages::pathForImageMappedAt(const void* addr) const
{
    if ( _initialImages != nullptr ) {
        // being called during libSystem initialization, so _loadedImages not allocated yet
        for (const LoadedImage& li : *_initialImages) {
            uint8_t permissions;
            if ( li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                return li.image()->path();
            }
        }
        return nullptr;
    }

    // if address is in cache, do fast search of TEXT segments in cache
    __block const char* result = nullptr;
    if ( (_dyldCacheAddress != nullptr) && (addr > _dyldCacheAddress) ) {
        if ( addr < (void*)((uint8_t*)_dyldCacheAddress+_dyldCacheAddress->mappedSize()) ) {
            uint64_t cacheSlide        = (uint64_t)_dyldCacheAddress - _dyldCacheAddress->unslidLoadAddress();
            uint64_t unslidTargetAddr  = (uint64_t)addr - cacheSlide;
            _dyldCacheAddress->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const unsigned char* dylibUUID, const char* installName, bool& stop) {
                if ( (loadAddressUnslid <= unslidTargetAddr) && (unslidTargetAddr < loadAddressUnslid+textSegmentSize) ) {
                    result = installName;
                    stop   = true;
                }
            });
            if ( result != nullptr )
                return result;
        }
    }

    // slow path - search image list
    infoForImageMappedAt(addr, ^(const LoadedImage& foundImage, uint8_t permissions) {
        result = foundImage.image()->path();
    });

    return result;
}

void AllImages::infoForImageMappedAt(const void* addr, void (^handler)(const LoadedImage& foundImage, uint8_t permissions)) const
{
    __block uint8_t permissions;
    if ( _initialImages != nullptr ) {
        // being called during libSystem initialization, so _loadedImages not allocated yet
        for (const LoadedImage& li : *_initialImages) {
            if ( li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                handler(li, permissions);
                break;
            }
        }
        return;
    }

    withReadLock(^{
        for (const LoadedImage& li : _loadedImages) {
            if ( li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                handler(li, permissions);
                break;
            }
        }
    });
}


bool AllImages::infoForImageMappedAt(const void* addr, const MachOLoaded** ml, uint64_t* textSize, const char** path) const
{
    if ( _initialImages != nullptr ) {
        // being called during libSystem initialization, so _loadedImages not allocated yet
        for (const LoadedImage& li : *_initialImages) {
            uint8_t permissions;
            if ( li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                if ( ml != nullptr )
                    *ml = li.loadedAddress();
                if ( path != nullptr )
                    *path = li.image()->path();
                if ( textSize != nullptr ) {
                    *textSize = li.image()->textSize();
                }
                return true;
            }
        }
        return false;
    }

    // if address is in cache, do fast search of TEXT segments in cache
    __block bool result = false;
    if ( (_dyldCacheAddress != nullptr) && (addr > _dyldCacheAddress) ) {
        if ( addr < (void*)((uint8_t*)_dyldCacheAddress+_dyldCacheAddress->mappedSize()) ) {
            uint64_t cacheSlide        = (uint64_t)_dyldCacheAddress - _dyldCacheAddress->unslidLoadAddress();
            uint64_t unslidTargetAddr  = (uint64_t)addr - cacheSlide;
            _dyldCacheAddress->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const unsigned char* dylibUUID, const char* installName, bool& stop) {
                if ( (loadAddressUnslid <= unslidTargetAddr) && (unslidTargetAddr < loadAddressUnslid+textSegmentSize) ) {
                    if ( ml != nullptr )
                        *ml = (MachOLoaded*)(loadAddressUnslid + cacheSlide);
                    if ( path != nullptr )
                        *path = installName;
                    if ( textSize != nullptr )
                        *textSize = textSegmentSize;
                    stop = true;
                    result = true;
                }
            });
            if ( result )
                return result;
        }
    }

    // slow path - search image list
    infoForImageMappedAt(addr, ^(const LoadedImage& foundImage, uint8_t permissions) {
        if ( ml != nullptr )
            *ml = foundImage.loadedAddress();
        if ( path != nullptr )
            *path = foundImage.image()->path();
        if ( textSize != nullptr )
            *textSize = foundImage.image()->textSize();
        result = true;
    });

    return result;
}

// same as infoForImageMappedAt(), but only look at images not in the dyld cache
void AllImages::infoForNonCachedImageMappedAt(const void* addr, void (^handler)(const LoadedImage& foundImage, uint8_t permissions)) const
{
    __block uint8_t permissions;
    if ( _initialImages != nullptr ) {
        // being called during libSystem initialization, so _loadedImages not allocated yet
        for (const LoadedImage& li : *_initialImages) {
            if ( !((MachOAnalyzer*)li.loadedAddress())->inDyldCache() ) {
                if (  li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                    handler(li, permissions);
                    break;
                }
            }
        }
        return;
    }

    withReadLock(^{
        for (const LoadedImage& li : _loadedImages) {
            if ( !((MachOAnalyzer*)li.loadedAddress())->inDyldCache() ) {
                if ( li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                    handler(li, permissions);
                    break;
                }
            }
        }
    });
}

bool AllImages::immutableMemory(const void* addr, size_t length) const
{
    // quick check to see if in shared cache
    if ( _dyldCacheAddress != nullptr ) {
        bool readOnly;
        if ( _dyldCacheAddress->inCache(addr, length, readOnly) ) {
            return readOnly;
        }
    }

    __block bool result = false;
    withReadLock(^() {
        // quick check to see if it is not any non-cached image loaded
        if ( ((uintptr_t)addr < _lowestNonCached) || ((uintptr_t)addr+length > _highestNonCached) ) {
            result = false;
            return;
        }
        // slow walk through all images, only look at images not in dyld cache
        for (const LoadedImage& li : _loadedImages) {
            if ( !((MachOAnalyzer*)li.loadedAddress())->inDyldCache() ) {
                uint8_t permissions;
                if ( li.image()->containsAddress(addr, li.loadedAddress(), &permissions) ) {
                    result = ((permissions & VM_PROT_WRITE) == 0) && li.image()->neverUnload();
                    break;
                }
            }
        }
    });

    return result;
}

void AllImages::infoForImageWithLoadAddress(const MachOLoaded* mh, void (^handler)(const LoadedImage& foundImage)) const
{
    withReadLock(^{
        for (const LoadedImage& li : _loadedImages) {
            if ( li.loadedAddress() == mh ) {
                handler(li);
                break;
            }
        }
    });
}

bool AllImages::findImageNum(closure::ImageNum imageNum, LoadedImage& foundImage) const
{
   if ( _initialImages != nullptr ) {
        // being called during libSystem initialization, so _loadedImages not allocated yet
        for (const LoadedImage& li : *_initialImages) {
            if ( li.image()->representsImageNum(imageNum) ) {
                foundImage = li;
                return true;
            }
        }
        return false;
    }

    bool result = false;
    for (const LoadedImage& li : _loadedImages) {
        if ( li.image()->representsImageNum(imageNum) ) {
            foundImage = li;
            result = true;
            break;
        }
    }

    return result;
}

const MachOLoaded* AllImages::findDependent(const MachOLoaded* mh, uint32_t depIndex)
{
    __block const MachOLoaded* result = nullptr;
    withReadLock(^{
        for (const LoadedImage& li : _loadedImages) {
            if ( li.loadedAddress() == mh ) {
                closure::ImageNum depImageNum = li.image()->dependentImageNum(depIndex);
                LoadedImage depLi;
                if ( findImageNum(depImageNum, depLi) )
                    result = depLi.loadedAddress();
                break;
            }
        }
    });
    return result;
}


void AllImages::breadthFirstRecurseDependents(Array<closure::ImageNum>& visited, const LoadedImage& nodeLi, bool& stopped, void (^handler)(const LoadedImage& aLoadedImage, bool& stop)) const
{
    // call handler on all direct dependents (unless already visited)
    STACK_ALLOC_ARRAY(LoadedImage, dependentsToRecurse, 256);
    nodeLi.image()->forEachDependentImage(^(uint32_t depIndex, closure::Image::LinkKind kind, closure::ImageNum depImageNum, bool& depStop) {
        if ( kind == closure::Image::LinkKind::upward )
            return;
        if ( visited.contains(depImageNum) )
            return;
        LoadedImage depLi;
        if ( !findImageNum(depImageNum, depLi) )
            return;
        handler(depLi, depStop);
        visited.push_back(depImageNum);
        if ( depStop ) {
            stopped = true;
            return;
        }
        dependentsToRecurse.push_back(depLi);
    });
    if ( stopped )
        return;
    // recurse on all dependents just visited
    for (LoadedImage& depLi : dependentsToRecurse) {
       breadthFirstRecurseDependents(visited, depLi, stopped, handler);
    }
}

void AllImages::visitDependentsTopDown(const LoadedImage& start, void (^handler)(const LoadedImage& aLoadedImage, bool& stop)) const
{
    withReadLock(^{
        STACK_ALLOC_ARRAY(closure::ImageNum, visited, count());
        bool stop = false;
        handler(start, stop);
        if ( stop )
            return;
        visited.push_back(start.image()->imageNum());
        breadthFirstRecurseDependents(visited, start, stop, handler);
    });
}

const MachOLoaded* AllImages::mainExecutable() const
{
    assert(_programVars != nullptr);
    return (const MachOLoaded*)_programVars->mh;
}

const closure::Image* AllImages::mainExecutableImage() const
{
    assert(_mainClosure != nullptr);
    return _mainClosure->images()->imageForNum(_mainClosure->topImage());
}

void AllImages::setMainPath(const char* path )
{
    _mainExeOverridePath = path;
}

const char* AllImages::imagePath(const closure::Image* image) const
{
#if __IPHONE_OS_VERSION_MIN_REQUIRED
    // on iOS and watchOS, apps may be moved on device after closure built
	if ( _mainExeOverridePath != nullptr ) {
        if ( image == mainExecutableImage() )
            return _mainExeOverridePath;
    }
#endif
    return image->path();
}

dyld_platform_t AllImages::platform() const {
    return _platform;
}

void AllImages::incRefCount(const mach_header* loadAddress)
{
    for (DlopenCount& entry : _dlopenRefCounts) {
        if ( entry.loadAddress == loadAddress ) {
            // found existing DlopenCount entry, bump counter
            entry.refCount += 1;
            return;
        }
    }

    // no existing DlopenCount, add new one
    _dlopenRefCounts.push_back({ loadAddress, 1 });
}

void AllImages::decRefCount(const mach_header* loadAddress)
{
    bool doCollect = false;
    for (DlopenCount& entry : _dlopenRefCounts) {
        if ( entry.loadAddress == loadAddress ) {
            // found existing DlopenCount entry, bump counter
            entry.refCount -= 1;
            if ( entry.refCount == 0 ) {
                _dlopenRefCounts.erase(entry);
                doCollect = true;
                break;
            }
            return;
        }
    }
    if ( doCollect )
        garbageCollectImages();
}


#if __MAC_OS_X_VERSION_MIN_REQUIRED
NSObjectFileImage AllImages::addNSObjectFileImage(const OFIInfo& image)
{
    __block uint64_t imageNum = 0;
    withWriteLock(^{
        imageNum = ++_nextObjectFileImageNum;
        _objectFileImages.push_back(image);
        _objectFileImages.back().imageNum = imageNum;
    });
    return (NSObjectFileImage)imageNum;
}

bool AllImages::forNSObjectFileImage(NSObjectFileImage imageHandle,
                                     void (^handler)(OFIInfo& image)) {
    uint64_t imageNum = (uint64_t)imageHandle;
    bool __block foundImage = false;
    withReadLock(^{
        for (OFIInfo& ofi : _objectFileImages) {
            if ( ofi.imageNum == imageNum ) {
                handler(ofi);
                foundImage = true;
                return;
            }
        }
    });

    return foundImage;
}

void AllImages::removeNSObjectFileImage(NSObjectFileImage imageHandle)
{
    uint64_t imageNum = (uint64_t)imageHandle;
    withWriteLock(^{
        for (OFIInfo& ofi : _objectFileImages) {
            if ( ofi.imageNum == imageNum ) {
                _objectFileImages.erase(ofi);
                return;
            }
        }
    });
}
#endif


class VIS_HIDDEN Reaper
{
public:
    struct ImageAndUse
    {
        const LoadedImage*  li;
        bool                inUse;
    };
                        Reaper(Array<ImageAndUse>& unloadables, AllImages*);
    void                garbageCollect();
    void                finalizeDeadImages();
private:

    void                markDirectlyDlopenedImagesAsUsed();
    void                markDependentOfInUseImages();
    void                markDependentsOf(const LoadedImage*);
    uint32_t            inUseCount();
    void                dump(const char* msg);

    Array<ImageAndUse>& _unloadables;
    AllImages*          _allImages;
    uint32_t            _deadCount;
};

Reaper::Reaper(Array<ImageAndUse>& unloadables, AllImages* all)
 : _unloadables(unloadables), _allImages(all), _deadCount(0)
{
}

void Reaper::markDirectlyDlopenedImagesAsUsed()
{
    for (AllImages::DlopenCount& entry : _allImages->_dlopenRefCounts) {
        if ( entry.refCount != 0 ) {
            for (ImageAndUse& iu : _unloadables) {
                if ( iu.li->loadedAddress() == entry.loadAddress ) {
                    iu.inUse = true;
                    break;
                }
            }
        }
	}
}

uint32_t Reaper::inUseCount()
{
    uint32_t count = 0;
	for (ImageAndUse& iu : _unloadables) {
        if ( iu.inUse )
            ++count;
    }
    return count;
}

void Reaper::markDependentsOf(const LoadedImage* li)
{
    li->image()->forEachDependentImage(^(uint32_t depIndex, closure::Image::LinkKind kind, closure::ImageNum depImageNum, bool& stop) {
        for (ImageAndUse& iu : _unloadables) {
            if ( !iu.inUse && iu.li->image()->representsImageNum(depImageNum) ) {
                iu.inUse = true;
                break;
            }
        }
    });
}

void Reaper::markDependentOfInUseImages()
{
	for (ImageAndUse& iu : _unloadables) {
        if ( iu.inUse )
            markDependentsOf(iu.li);
    }
}

void Reaper::dump(const char* msg)
{
    //log("%s:\n", msg);
    //for (ImageAndUse& iu : _unloadables) {
    //    log("  in-used=%d  %s\n", iu.inUse, iu.li->image()->path());
    //}
}

void Reaper::garbageCollect()
{
    //dump("all unloadable images");

    // mark all dylibs directly dlopen'ed as in use
    markDirectlyDlopenedImagesAsUsed();

    //dump("directly dlopen()'ed marked");

    // iteratively mark dependents of in-use dylibs as in-use until in-use count stops changing
    uint32_t lastCount = inUseCount();
    bool countChanged = false;
    do {
        markDependentOfInUseImages();
        //dump("dependents marked");
        uint32_t newCount = inUseCount();
        countChanged = (newCount != lastCount);
        lastCount = newCount;
    } while (countChanged);

    _deadCount = (uint32_t)_unloadables.count() - inUseCount();
}

void Reaper::finalizeDeadImages()
{
    if ( _deadCount == 0 )
        return;
    __cxa_range_t ranges[_deadCount];
    __cxa_range_t* rangesArray = ranges;
    __block unsigned int rangesCount = 0;
	for (ImageAndUse& iu : _unloadables) {
        if ( iu.inUse )
            continue;
        iu.li->image()->forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool &stop) {
            if ( permissions & VM_PROT_EXECUTE ) {
                rangesArray[rangesCount].addr   = (char*)(iu.li->loadedAddress()) + vmOffset;
                rangesArray[rangesCount].length = (size_t)vmSize;
                ++rangesCount;
           }
        });
    }
    __cxa_finalize_ranges(ranges, rangesCount);
}


// This function is called at the end of dlclose() when the reference count goes to zero.
// The dylib being unloaded may have brought in other dependent dylibs when it was loaded.
// Those dependent dylibs need to be unloaded, but only if they are not referenced by
// something else.  We use a standard mark and sweep garbage collection.
//
// The tricky part is that when a dylib is unloaded it may have a termination function that
// can run and itself call dlclose() on yet another dylib.  The problem is that this
// sort of gabage collection is not re-entrant.  Instead a terminator's call to dlclose()
// which calls garbageCollectImages() will just set a flag to re-do the garbage collection
// when the current pass is done.
//
// Also note that this is done within the _loadedImages writer lock, so any dlopen/dlclose
// on other threads are blocked while this garbage collections runs
//
void AllImages::garbageCollectImages()
{
    // if some other thread is currently GC'ing images, let other thread do the work
    int32_t newCount = OSAtomicIncrement32(&_gcCount);
    if ( newCount != 1 )
        return;

    do {
        STACK_ALLOC_ARRAY(Reaper::ImageAndUse, unloadables, _loadedImages.count());
        withReadLock(^{
            for (const LoadedImage& li : _loadedImages) {
                if ( !li.image()->neverUnload() /*&& !li.neverUnload()*/ ) {
                    unloadables.push_back({&li, false});
                    //fprintf(stderr, "unloadable[%lu] %p %s\n", unloadables.count(), li.loadedAddress(), li.image()->path());
                }
            }
        });
        // make reaper object to do garbage collection and notifications
        Reaper reaper(unloadables, this);
        reaper.garbageCollect();

        // FIXME: we should sort dead images so higher level ones are terminated first

        // call cxa_finalize_ranges of dead images
        reaper.finalizeDeadImages();

        // FIXME: call static terminators of dead images

        // FIXME: DOF unregister

        //fprintf(stderr, "_loadedImages before GC removals:\n");
        //for (const LoadedImage& li : _loadedImages) {
        //    fprintf(stderr, "   loadAddr=%p, path=%s\n", li.loadedAddress(), li.image()->path());
        //}

        // make copy of LoadedImages we want to remove
        // because unloadables[] points into LoadedImage we are shrinking
        STACK_ALLOC_ARRAY(LoadedImage, unloadImages, _loadedImages.count());
        for (const Reaper::ImageAndUse& iu : unloadables) {
            if ( !iu.inUse )
                unloadImages.push_back(*iu.li);
        }
        // remove entries from _loadedImages
        if ( !unloadImages.empty() ) {
            removeImages(unloadImages);

            //fprintf(stderr, "_loadedImages after GC removals:\n");
            //for (const LoadedImage& li : _loadedImages) {
            //   fprintf(stderr, "   loadAddr=%p, path=%s\n", li.loadedAddress(), li.image()->path());
            //}
        }

        // if some other thread called GC during our work, redo GC on its behalf
        newCount = OSAtomicDecrement32(&_gcCount);
    }
    while (newCount > 0);
}



void AllImages::addLoadNotifier(NotifyFunc func)
{
    // callback about already loaded images
    withReadLock(^{
        for (const LoadedImage& li : _loadedImages) {
            dyld3::ScopedTimer timer(DBG_DYLD_TIMING_FUNC_FOR_ADD_IMAGE, (uint64_t)li.loadedAddress(), (uint64_t)func, 0);
            log_notifications("dyld: add notifier %p called with mh=%p\n", func, li.loadedAddress());
            if ( li.image()->inDyldCache() )
                func(li.loadedAddress(), (uintptr_t)_dyldCacheSlide);
            else
                func(li.loadedAddress(), li.loadedAddress()->getSlide());
        }
    });

    // add to list of functions to call about future loads
    withNotifiersLock(^{
        _loadNotifiers.push_back(func);
    });
}

void AllImages::addUnloadNotifier(NotifyFunc func)
{
    // add to list of functions to call about future unloads
    withNotifiersLock(^{
        _unloadNotifiers.push_back(func);
    });
}

void AllImages::addLoadNotifier(LoadNotifyFunc func)
{
    // callback about already loaded images
    withReadLock(^{
        for (const LoadedImage& li : _loadedImages) {
            dyld3::ScopedTimer timer(DBG_DYLD_TIMING_FUNC_FOR_ADD_IMAGE, (uint64_t)li.loadedAddress(), (uint64_t)func, 0);
            log_notifications("dyld: add notifier %p called with mh=%p\n", func, li.loadedAddress());
            func(li.loadedAddress(), li.image()->path(), !li.image()->neverUnload());
        }
    });

    // add to list of functions to call about future loads
    withNotifiersLock(^{
        _loadNotifiers2.push_back(func);
    });
}


void AllImages::setObjCNotifiers(_dyld_objc_notify_mapped map, _dyld_objc_notify_init init, _dyld_objc_notify_unmapped unmap)
{
    _objcNotifyMapped   = map;
    _objcNotifyInit     = init;
    _objcNotifyUnmapped = unmap;

    // callback about already loaded images
    uint32_t maxCount = count();
    STACK_ALLOC_ARRAY(const mach_header*, mhs,   maxCount);
    STACK_ALLOC_ARRAY(const char*,        paths, maxCount);
    // don't need _mutex here because this is called when process is still single threaded
    for (const LoadedImage& li : _loadedImages) {
        if ( li.image()->hasObjC() ) {
            paths.push_back(imagePath(li.image()));
            mhs.push_back(li.loadedAddress());
        }
    }
    if ( !mhs.empty() ) {
        (*map)((uint32_t)mhs.count(), &paths[0], &mhs[0]);
        if ( log_notifications("dyld: objc-mapped-notifier called with %ld images:\n", mhs.count()) ) {
            for (uintptr_t i=0; i < mhs.count(); ++i) {
                log_notifications("dyld:  objc-mapped: %p %s\n",  mhs[i], paths[i]);
            }
        }
    }
}

void AllImages::applyInterposingToDyldCache(const closure::Closure* closure)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_APPLY_INTERPOSING, 0, 0, 0);
    const uintptr_t                 cacheStart              = (uintptr_t)_dyldCacheAddress;
    __block closure::ImageNum       lastCachedDylibImageNum = 0;
    __block const closure::Image*   lastCachedDylibImage    = nullptr;
    __block bool                    suspendedAccounting     = false;
    closure->forEachPatchEntry(^(const closure::Closure::PatchEntry& entry) {
        if ( entry.overriddenDylibInCache != lastCachedDylibImageNum ) {
            lastCachedDylibImage    = closure::ImageArray::findImage(imagesArrays(), entry.overriddenDylibInCache);
            assert(lastCachedDylibImage != nullptr);
            lastCachedDylibImageNum = entry.overriddenDylibInCache;
        }
        if ( !suspendedAccounting ) {
            Loader::vmAccountingSetSuspended(true, log_fixups);
            suspendedAccounting = true;
        }
        uintptr_t newValue = 0;
        LoadedImage foundImage;
        switch ( entry.replacement.image.kind ) {
            case closure::Image::ResolvedSymbolTarget::kindImage:
                assert(findImageNum(entry.replacement.image.imageNum, foundImage));
                newValue = (uintptr_t)(foundImage.loadedAddress()) + (uintptr_t)entry.replacement.image.offset;
                break;
            case closure::Image::ResolvedSymbolTarget::kindSharedCache:
                newValue = (uintptr_t)_dyldCacheAddress + (uintptr_t)entry.replacement.sharedCache.offset;
                break;
            case closure::Image::ResolvedSymbolTarget::kindAbsolute:
                // this means the symbol was missing in the cache override dylib, so set any uses to NULL
                newValue = (uintptr_t)entry.replacement.absolute.value;
                break;
            default:
                assert(0 && "bad replacement kind");
        }
        lastCachedDylibImage->forEachPatchableUseOfExport(entry.exportCacheOffset, ^(closure::Image::PatchableExport::PatchLocation patchLocation) {
            uintptr_t* loc = (uintptr_t*)(cacheStart+patchLocation.cacheOffset);
 #if __has_feature(ptrauth_calls)
            if ( patchLocation.authenticated ) {
                MachOLoaded::ChainedFixupPointerOnDisk fixupInfo;
                fixupInfo.authRebase.auth      = true;
                fixupInfo.authRebase.addrDiv   = patchLocation.usesAddressDiversity;
                fixupInfo.authRebase.diversity = patchLocation.discriminator;
                fixupInfo.authRebase.key       = patchLocation.key;
                *loc = fixupInfo.signPointer(loc, newValue + patchLocation.getAddend());
                log_fixups("dyld: cache fixup: *%p = %p (JOP: diversity 0x%04X, addr-div=%d, key=%s)\n",
                           loc, (void*)*loc, patchLocation.discriminator, patchLocation.usesAddressDiversity, patchLocation.keyName());
                return;
            }
#endif
            log_fixups("dyld: cache fixup: *%p = 0x%0lX (dyld cache patch)\n", loc, newValue + (uintptr_t)patchLocation.getAddend());
            *loc = newValue + (uintptr_t)patchLocation.getAddend();
        });
    });
    if ( suspendedAccounting )
        Loader::vmAccountingSetSuspended(false, log_fixups);
}

void AllImages::runStartupInitialzers()
{
    __block bool mainExecutableInitializerNeedsToRun = true;
    __block uint32_t imageIndex = 0;
    while ( mainExecutableInitializerNeedsToRun ) {
        __block const closure::Image* image = nullptr;
        withReadLock(^{
            image = _loadedImages[imageIndex].image();
            if ( _loadedImages[imageIndex].loadedAddress()->isMainExecutable() )
                mainExecutableInitializerNeedsToRun = false;
        });
       runInitialzersBottomUp(image);
        ++imageIndex;
    }
}


// Find image in _loadedImages which has ImageNum == num.
// Try indexHint first, if hint is wrong, updated it, so next use is faster.
LoadedImage AllImages::findImageNum(closure::ImageNum num, uint32_t& indexHint)
{
    __block LoadedImage copy;
    withReadLock(^{
        if ( (indexHint >= _loadedImages.count()) || !_loadedImages[indexHint].image()->representsImageNum(num) ) {
            indexHint = 0;
            for (indexHint=0; indexHint < _loadedImages.count(); ++indexHint) {
                if ( _loadedImages[indexHint].image()->representsImageNum(num) )
                    break;
            }
            assert(indexHint < _loadedImages.count());
        }
        copy = _loadedImages[indexHint];
    });
    return copy;
}


// Change the state of the LoadedImage in _loadedImages which has ImageNum == num.
// Only change state if current state is expectedCurrentState (atomic swap).
bool AllImages::swapImageState(closure::ImageNum num, uint32_t& indexHint, LoadedImage::State expectedCurrentState, LoadedImage::State newState)
{
    __block bool result = false;
    withWriteLock(^{
        if ( (indexHint >= _loadedImages.count()) || !_loadedImages[indexHint].image()->representsImageNum(num) ) {
            indexHint = 0;
            for (indexHint=0; indexHint < _loadedImages.count(); ++indexHint) {
                if ( _loadedImages[indexHint].image()->representsImageNum(num) )
                    break;
            }
            assert(indexHint < _loadedImages.count());
        }
        if ( _loadedImages[indexHint].state() == expectedCurrentState ) {
           _loadedImages[indexHint].setState(newState);
            result = true;
        }
    });
    return result;
}

// dyld3 pre-builds the order initializers need to be run (bottom up) in a list in the closure.
// This method uses that list to run all initializers.
// Because an initializer may call dlopen() and/or create threads, the _loadedImages array
// may move under us. So, never keep a pointer into it. Always reference images by ImageNum
// and use hint to make that faster in the case where the _loadedImages does not move.
void AllImages::runInitialzersBottomUp(const closure::Image* topImage)
{
    // walk closure specified initializer list, already ordered bottom up
    topImage->forEachImageToInitBefore(^(closure::ImageNum imageToInit, bool& stop) {
        // get copy of LoadedImage about imageToInit, but don't keep reference into _loadedImages, because it may move if initialzers call dlopen()
        uint32_t    indexHint = 0;
        LoadedImage loadedImageCopy = findImageNum(imageToInit, indexHint);
        // skip if the image is already inited, or in process of being inited (dependency cycle)
        if ( (loadedImageCopy.state() == LoadedImage::State::fixedUp) && swapImageState(imageToInit, indexHint, LoadedImage::State::fixedUp, LoadedImage::State::beingInited) ) {
            // tell objc to run any +load methods in image
            if ( (_objcNotifyInit != nullptr) && loadedImageCopy.image()->mayHavePlusLoads() ) {
                dyld3::ScopedTimer timer(DBG_DYLD_TIMING_OBJC_INIT, (uint64_t)loadedImageCopy.loadedAddress(), 0, 0);
                const char* path = imagePath(loadedImageCopy.image());
                log_notifications("dyld: objc-init-notifier called with mh=%p, path=%s\n", loadedImageCopy.loadedAddress(), path);
                (*_objcNotifyInit)(path, loadedImageCopy.loadedAddress());
            }

            // run all initializers in image
            runAllInitializersInImage(loadedImageCopy.image(), loadedImageCopy.loadedAddress());

            // advance state to inited
            swapImageState(imageToInit, indexHint, LoadedImage::State::beingInited, LoadedImage::State::inited);
        }
    });
}


void AllImages::runLibSystemInitializer(const LoadedImage& libSystem)
{
    // run all initializers in libSystem.dylib
    runAllInitializersInImage(libSystem.image(), libSystem.loadedAddress());

    // Note: during libSystem's initialization, libdyld_initializer() is called which copies _initialImages to _loadedImages

    // mark libSystem.dylib as being inited, so later recursive-init would re-run it
    for (LoadedImage& li : _loadedImages) {
        if ( li.loadedAddress() == libSystem.loadedAddress() ) {
            li.setState(LoadedImage::State::inited);
            break;
        }
    }
}

void AllImages::runAllInitializersInImage(const closure::Image* image, const MachOLoaded* ml)
{
    image->forEachInitializer(ml, ^(const void* func) {
        Initializer initFunc = (Initializer)func;
#if __has_feature(ptrauth_calls)
        initFunc = (Initializer)__builtin_ptrauth_sign_unauthenticated((void*)initFunc, 0, 0);
#endif
        {
            ScopedTimer(DBG_DYLD_TIMING_STATIC_INITIALIZER, (uint64_t)ml, (uint64_t)func, 0);
            initFunc(NXArgc, NXArgv, environ, appleParams, _programVars);

        }
        log_initializers("dyld: called initialzer %p in %s\n", initFunc, image->path());
    });
}

const MachOLoaded* AllImages::dlopen(Diagnostics& diag, const char* path, bool rtldNoLoad, bool rtldLocal, bool rtldNoDelete, bool fromOFI, const void* callerAddress)
{
    // quick check if path is in shared cache and already loaded
    if ( _dyldCacheAddress != nullptr ) {
        uint32_t dyldCacheImageIndex;
        if ( _dyldCacheAddress->hasImagePath(path, dyldCacheImageIndex) ) {
            uint64_t mTime;
            uint64_t inode;
            const MachOLoaded* mh = (MachOLoaded*)_dyldCacheAddress->getIndexedImageEntry(dyldCacheImageIndex, mTime, inode);
            // Note: we do not need readLock because this is within global dlopen lock
            for (const LoadedImage& li : _loadedImages) {
                if ( li.loadedAddress() == mh ) {
                    return mh;
                }
            }
        }
    }

    __block closure::ImageNum callerImageNum = 0;
    STACK_ALLOC_ARRAY(LoadedImage, loadedList, 1024);
    for (const LoadedImage& li : _loadedImages) {
        loadedList.push_back(li);
        uint8_t permissions;
        if ( (callerImageNum == 0) && li.image()->containsAddress(callerAddress, li.loadedAddress(), &permissions) ) {
            callerImageNum = li.image()->imageNum();
        }
        //fprintf(stderr, "mh=%p, image=%p, imageNum=0x%04X, path=%s\n", li.loadedAddress(), li.image(), li.image()->imageNum(), li.image()->path());
    }
    uintptr_t alreadyLoadedCount = loadedList.count();

    // make closure
    closure::ImageNum topImageNum = 0;
    const closure::DlopenClosure* newClosure;

    // First try with closures from the shared cache permitted.
    // Then try again with forcing a new closure
    for (bool canUseSharedCacheClosure : { true, false }) {
        closure::FileSystemPhysical fileSystem;
        closure::ClosureBuilder::AtPath atPathHanding = (_allowAtPaths ? closure::ClosureBuilder::AtPath::all : closure::ClosureBuilder::AtPath::onlyInRPaths);
        closure::ClosureBuilder cb(_nextImageNum, fileSystem, _dyldCacheAddress, true, closure::gPathOverrides, atPathHanding);
        newClosure = cb.makeDlopenClosure(path, _mainClosure, loadedList, callerImageNum, rtldNoLoad, canUseSharedCacheClosure, &topImageNum);
        if ( newClosure == closure::ClosureBuilder::sRetryDlopenClosure ) {
            log_apis("   dlopen: closure builder needs to retry: %s\n", path);
            assert(canUseSharedCacheClosure);
            continue;
        }
        if ( (newClosure == nullptr) && (topImageNum == 0) ) {
            if ( cb.diagnostics().hasError())
                diag.error("%s", cb.diagnostics().errorMessage());
            else if ( !rtldNoLoad )
                diag.error("dlopen(): file not found: %s", path);
            return nullptr;
        }
        // save off next available ImageNum for use by next call to dlopen()
        _nextImageNum = cb.nextFreeImageNum();
        break;
    }

    if ( newClosure != nullptr ) {
        // if new closure contains an ImageArray, add it to list
        if ( const closure::ImageArray* newArray = newClosure->images() ) {
            appendToImagesArray(newArray);
        }
        log_apis("   dlopen: made closure: %p\n", newClosure);
    }

    // if already loaded, just bump refCount and return
    if ( (newClosure == nullptr) && (topImageNum != 0) ) {
        for (LoadedImage& li : _loadedImages) {
            if ( li.image()->imageNum() == topImageNum ) {
                // is already loaded
                const MachOLoaded* topLoadAddress = li.loadedAddress();
                if ( !li.image()->inDyldCache() )
                    incRefCount(topLoadAddress);
                log_apis("   dlopen: already loaded as '%s'\n", li.image()->path());
                // if previously opened with RTLD_LOCAL, but now opened with RTLD_GLOBAL, unhide it
                if ( !rtldLocal && li.hideFromFlatSearch() )
                    li.setHideFromFlatSearch(false);
                // if called with RTLD_NODELETE, mark it as never-unload
                if ( rtldNoDelete )
                    li.markLeaveMapped();
                return topLoadAddress;
            }
        }
    }

    // run loader to load all new images
	Loader loader(loadedList, _dyldCacheAddress, imagesArrays(), &dyld3::log_loads, &dyld3::log_segments, &dyld3::log_fixups, &dyld3::log_dofs);
	const closure::Image* topImage = closure::ImageArray::findImage(imagesArrays(), topImageNum);
    if ( newClosure == nullptr ) {
        if ( topImageNum < dyld3::closure::kLastDyldCacheImageNum )
            log_apis("   dlopen: using image in dyld shared cache %p\n", topImage);
        else
            log_apis("   dlopen: using pre-built dlopen closure %p\n", topImage);
    }
    uintptr_t topIndex = loadedList.count();
    LoadedImage topLoadedImage = LoadedImage::make(topImage);
    if ( rtldLocal && !topImage->inDyldCache() )
        topLoadedImage.setHideFromFlatSearch(true);
    if ( rtldNoDelete && !topImage->inDyldCache() )
        topLoadedImage.markLeaveMapped();
    loader.addImage(topLoadedImage);


    // recursively load all dependents and fill in allImages array
    loader.completeAllDependents(diag, topIndex);
    if ( diag.hasError() )
         return nullptr;
    loader.mapAndFixupAllImages(diag, _processDOFs, fromOFI, topIndex);
    if ( diag.hasError() )
         return nullptr;

    const MachOLoaded* topLoadAddress = loadedList[topIndex].loadedAddress();

    // bump dlopen refcount of image directly loaded
    if ( !topImage->inDyldCache() )
        incRefCount(topLoadAddress);

    // tell gAllImages about new images
    const uint32_t newImageCount = (uint32_t)(loadedList.count() - alreadyLoadedCount);
    addImages(loadedList.subArray(alreadyLoadedCount, newImageCount));

    // if closure adds images that override dyld cache, patch cache
    if ( newClosure != nullptr )
        applyInterposingToDyldCache(newClosure);

    runImageNotifiers(loadedList.subArray(alreadyLoadedCount, newImageCount));

    // run initializers
    runInitialzersBottomUp(topImage);

    return topLoadAddress;
}

void AllImages::appendToImagesArray(const closure::ImageArray* newArray)
{
    _imagesArrays.push_back(newArray);
}

const Array<const closure::ImageArray*>& AllImages::imagesArrays()
{
    return _imagesArrays.array();
}

bool AllImages::isRestricted() const
{
    return !_allowEnvPaths;
}




} // namespace dyld3







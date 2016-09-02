/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2009 Apple Inc. All rights reserved.
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

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>

#include <vector>

#include "mach-o/dyld_gdb.h"
#include "mach-o/dyld_images.h"
#include "ImageLoader.h"

#if __IPHONE_OS_VERSION_MIN_REQUIRED
	#define INITIAL_UUID_IMAGE_COUNT 4
#else
	#define INITIAL_UUID_IMAGE_COUNT 32
#endif

static std::vector<dyld_image_info> sImageInfos;
static std::vector<dyld_uuid_info>  sImageUUIDs;

void addImagesToAllImages(uint32_t infoCount, const dyld_image_info info[])
{
	// make initial size large enough that we probably won't need to re-alloc it
	if ( sImageInfos.size() == 0 )
		sImageInfos.reserve(INITIAL_IMAGE_COUNT);
	if ( sImageUUIDs.capacity() == 0 )
		sImageUUIDs.reserve(4);
	// set infoArray to NULL to denote it is in-use
	dyld_all_image_infos.infoArray = NULL;
	
	// append all new images
	for (uint32_t i=0; i < infoCount; ++i)
		sImageInfos.push_back(info[i]);
	dyld_all_image_infos.infoArrayCount = sImageInfos.size();
	
	// set infoArray back to base address of vector (other process can now read)
	dyld_all_image_infos.infoArray = &sImageInfos[0];
}


const char* notifyGDB(enum dyld_image_states state, uint32_t infoCount, const dyld_image_info info[])
{
	// tell gdb that about the new images
	dyld_all_image_infos.notification(dyld_image_adding, infoCount, info);
	// <rdar://problem/7739489> record initial count of images  
	// so CrashReporter can note which images were dynamically loaded
	if ( dyld_all_image_infos.initialImageCount == 0 )
		dyld_all_image_infos.initialImageCount = infoCount;
	return NULL;
}



void addNonSharedCacheImageUUID(const dyld_uuid_info& info)
{
	// set uuidArray to NULL to denote it is in-use
	dyld_all_image_infos.uuidArray = NULL;
	
	// append all new images
	sImageUUIDs.push_back(info);
	dyld_all_image_infos.uuidArrayCount = sImageUUIDs.size();
	
	// set uuidArray back to base address of vector (other process can now read)
	dyld_all_image_infos.uuidArray = &sImageUUIDs[0];
}

void removeImageFromAllImages(const struct mach_header* loadAddress)
{
	dyld_image_info goingAway;
	
	// set infoArray to NULL to denote it is in-use
	dyld_all_image_infos.infoArray = NULL;
	
	// remove image from infoArray
	for (std::vector<dyld_image_info>::iterator it=sImageInfos.begin(); it != sImageInfos.end(); it++) {
		if ( it->imageLoadAddress == loadAddress ) {
			goingAway = *it;
			sImageInfos.erase(it);
			break;
		}
	}
	dyld_all_image_infos.infoArrayCount = sImageInfos.size();
	
	// set infoArray back to base address of vector
	dyld_all_image_infos.infoArray = &sImageInfos[0];


	// set uuidArrayCount to NULL to denote it is in-use
	dyld_all_image_infos.uuidArray = NULL;
	
	// remove image from infoArray
	for (std::vector<dyld_uuid_info>::iterator it=sImageUUIDs.begin(); it != sImageUUIDs.end(); it++) {
		if ( it->imageLoadAddress == loadAddress ) {
			sImageUUIDs.erase(it);
			break;
		}
	}
	dyld_all_image_infos.uuidArrayCount = sImageUUIDs.size();
	
	// set infoArray back to base address of vector
	dyld_all_image_infos.uuidArray = &sImageUUIDs[0];

	// tell gdb that about the new images
	dyld_all_image_infos.notification(dyld_image_removing, 1, &goingAway);
}

#if __arm__
// work around for:  <rdar://problem/6530727> gdb-1109: notifier in dyld does not work if it is in thumb
extern "C" void gdb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[]);
#else
static void gdb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[])
{
	// do nothing
	// gdb sets a break point here to catch notifications
	//dyld::log("dyld: gdb_image_notifier(%s, %d, ...)\n", mode ? "dyld_image_removing" : "dyld_image_adding", infoCount);
	//for (uint32_t i=0; i < infoCount; ++i)
	//	dyld::log("dyld: %d loading at %p %s\n", i, info[i].imageLoadAddress, info[i].imageFilePath);
	//for (uint32_t i=0; i < dyld_all_image_infos.infoArrayCount; ++i)
	//	dyld::log("dyld: %d loading at %p %s\n", i, dyld_all_image_infos.infoArray[i].imageLoadAddress, dyld_all_image_infos.infoArray[i].imageFilePath);
}
#endif

void setAlImageInfosHalt(const char* message, uintptr_t flags)
{
	dyld_all_image_infos.errorMessage = message;
	dyld_all_image_infos.terminationFlags = flags;
}


extern void* __dso_handle;
#define STR(s) # s
#define XSTR(s) STR(s)

struct dyld_all_image_infos  dyld_all_image_infos __attribute__ ((section ("__DATA,__all_image_info"))) 
							= { 
								12, 0, NULL, &gdb_image_notifier, false, false, (const mach_header*)&__dso_handle, NULL, 
								XSTR(DYLD_VERSION), NULL, 0, NULL, 0, 0, NULL, &dyld_all_image_infos, 
								0, dyld_error_kind_none, NULL, NULL, NULL, 0
								};

struct dyld_shared_cache_ranges dyld_shared_cache_ranges;


 

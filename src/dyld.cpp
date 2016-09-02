/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2008 Apple Inc. All rights reserved.
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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/param.h>
#include <mach/mach_time.h> // mach_absolute_time()
#include <sys/types.h>
#include <sys/stat.h> 
#include <mach-o/fat.h> 
#include <mach-o/loader.h> 
#include <mach-o/ldsyms.h> 
#include <libkern/OSByteOrder.h> 
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <sys/dtrace.h>
#include <libkern/OSAtomic.h>

#ifndef CPU_SUBTYPE_ARM_V5TEJ
	#define CPU_SUBTYPE_ARM_V5TEJ		((cpu_subtype_t) 7)
#endif
#ifndef CPU_SUBTYPE_ARM_XSCALE
	#define CPU_SUBTYPE_ARM_XSCALE		((cpu_subtype_t) 8)
#endif
#ifndef CPU_SUBTYPE_ARM_V7
	#define CPU_SUBTYPE_ARM_V7			((cpu_subtype_t) 9)
#endif

#include <vector>
#include <algorithm>

#include "mach-o/dyld_gdb.h"

#include "dyld.h"
#include "ImageLoader.h"
#include "ImageLoaderMachO.h"
#include "dyldLibSystemInterface.h"
#if DYLD_SHARED_CACHE_SUPPORT
#include "dyld_cache_format.h"
#endif
#if CORESYMBOLICATION_SUPPORT
#include "coreSymbolicationDyldSupport.hpp"
#endif

// from _simple.h in libc
typedef struct _SIMPLE*		_SIMPLE_STRING;
extern "C" void				_simple_vdprintf(int __fd, const char *__fmt, va_list __ap);
extern "C" void				_simple_dprintf(int __fd, const char *__fmt, ...);
extern "C" _SIMPLE_STRING	_simple_salloc(void);
extern "C" int				_simple_vsprintf(_SIMPLE_STRING __b, const char *__fmt, va_list __ap);
extern "C" void				_simple_sfree(_SIMPLE_STRING __b);
extern "C" char *			_simple_string(_SIMPLE_STRING __b);



// 32-bit ppc and ARM are the only architecture that use cpu-sub-types
#define CPU_SUBTYPES_SUPPORTED __ppc__ || __arm__



#define CPU_TYPE_MASK 0x00FFFFFF	/* complement of CPU_ARCH_MASK */


/* implemented in dyld_gdb.cpp */
extern void addImagesToAllImages(uint32_t infoCount, const dyld_image_info info[]);
extern void removeImageFromAllImages(const mach_header* mh);
extern void setAlImageInfosHalt(const char* message, uintptr_t flags);

// magic so CrashReporter logs message
extern "C" {
	char error_string[1024];
}
// implemented in dyldStartup.s for CrashReporter
extern "C" void dyld_fatal_error(const char* errString) __attribute__((noreturn));



//
// The file contains the core of dyld used to get a process to main().  
// The API's that dyld supports are implemented in dyldAPIs.cpp.
//
//
//
//
//


namespace dyld {


// 
// state of all environment variables dyld uses
//
struct EnvironmentVariables {
	const char* const *			DYLD_FRAMEWORK_PATH;
	const char* const *			DYLD_FALLBACK_FRAMEWORK_PATH;
	const char* const *			DYLD_LIBRARY_PATH;
	const char* const *			DYLD_FALLBACK_LIBRARY_PATH;
	const char* const *			DYLD_INSERT_LIBRARIES;
	const char* const *			LD_LIBRARY_PATH;			// for unix conformance
	bool						DYLD_PRINT_LIBRARIES;
	bool						DYLD_PRINT_LIBRARIES_POST_LAUNCH;
	bool						DYLD_BIND_AT_LAUNCH;
	bool						DYLD_PRINT_STATISTICS;
	bool						DYLD_PRINT_OPTS;
	bool						DYLD_PRINT_ENV;
	bool						DYLD_DISABLE_DOFS;
                            //  DYLD_SHARED_CACHE_DONT_VALIDATE ==> sSharedCacheIgnoreInodeAndTimeStamp
                            //  DYLD_SHARED_CACHE_DIR           ==> sSharedCacheDir
							//	DYLD_ROOT_PATH					==> gLinkContext.rootPaths
							//	DYLD_IMAGE_SUFFIX				==> gLinkContext.imageSuffix
							//	DYLD_PRINT_OPTS					==> gLinkContext.verboseOpts
							//	DYLD_PRINT_ENV					==> gLinkContext.verboseEnv
							//	DYLD_FORCE_FLAT_NAMESPACE		==> gLinkContext.bindFlat
							//	DYLD_PRINT_INITIALIZERS			==> gLinkContext.verboseInit
							//	DYLD_PRINT_SEGMENTS				==> gLinkContext.verboseMapping
							//	DYLD_PRINT_BINDINGS				==> gLinkContext.verboseBind
							//  DYLD_PRINT_WEAK_BINDINGS		==> gLinkContext.verboseWeakBind
							//	DYLD_PRINT_REBASINGS			==> gLinkContext.verboseRebase
							//	DYLD_PRINT_DOFS					==> gLinkContext.verboseDOF
							//	DYLD_PRINT_APIS					==> gLogAPIs
							//	DYLD_IGNORE_PREBINDING			==> gLinkContext.prebindUsage
							//	DYLD_PREBIND_DEBUG				==> gLinkContext.verbosePrebinding
							//	DYLD_NEW_LOCAL_SHARED_REGIONS	==> gLinkContext.sharedRegionMode
							//	DYLD_SHARED_REGION				==> gLinkContext.sharedRegionMode
							//	DYLD_PRINT_WARNINGS				==> gLinkContext.verboseWarnings
};
        
typedef std::vector<dyld_image_state_change_handler> StateHandlers;
struct RegisteredDOF { const mach_header* mh; int registrationID; };

// all global state
static const char*					sExecPath = NULL;
static const macho_header*			sMainExecutableMachHeader = NULL;
static cpu_type_t					sHostCPU;
static cpu_subtype_t				sHostCPUsubtype;
static ImageLoader*					sMainExecutable = NULL;
static bool							sProcessIsRestricted = false;
static unsigned int					sInsertedDylibCount = 0;
static std::vector<ImageLoader*>	sAllImages;
static std::vector<ImageLoader*>	sImageRoots;
static std::vector<ImageLoader*>	sImageFilesNeedingTermination;
static std::vector<RegisteredDOF>	sImageFilesNeedingDOFUnregistration;
static std::vector<ImageCallback>   sAddImageCallbacks;
static std::vector<ImageCallback>   sRemoveImageCallbacks;
static StateHandlers				sSingleHandlers[7];
static StateHandlers				sBatchHandlers[7];
static ImageLoader*					sLastImageByAddressCache;
static EnvironmentVariables			sEnv;
static const char*					sFrameworkFallbackPaths[] = { "$HOME/Library/Frameworks", "/Library/Frameworks", "/Network/Library/Frameworks", "/System/Library/Frameworks", NULL };
static const char*					sLibraryFallbackPaths[] = { "$HOME/lib", "/usr/local/lib", "/usr/lib", NULL };
static UndefinedHandler				sUndefinedHandler = NULL;
static ImageLoader*					sBundleBeingLoaded = NULL;	// hack until OFI is reworked
#if DYLD_SHARED_CACHE_SUPPORT
static const dyld_cache_header*		sSharedCache = NULL;
static bool							sSharedCacheIgnoreInodeAndTimeStamp = false;
static const char*					sSharedCacheDir = DYLD_SHARED_CACHE_DIR;
#endif
ImageLoader::LinkContext			gLinkContext;
bool								gLogAPIs = false;
const struct LibSystemHelpers*		gLibSystemHelpers = NULL;
#if SUPPORT_OLD_CRT_INITIALIZATION
bool								gRunInitializersOldWay = false;
#endif


//
// The MappedRanges structure is used for fast address->image lookups.
// The table is only updated when the dyld lock is held, so we don't
// need to worry about multiple writers.  But readers may look at this
// data without holding the lock. Therefore, all updates must be done
// in an order that will never cause readers to see inconsistent data.
// The general rule is that if the image field is non-NULL then
// the other fields are valid.
//
struct MappedRanges
{
	enum { count=400 };
	struct {
		ImageLoader*	image;
		uintptr_t		start;
		uintptr_t		end;
	} array[count];
	MappedRanges*		next;
};

static MappedRanges			sMappedRangesStart;

void addMappedRange(ImageLoader* image, uintptr_t start, uintptr_t end)
{
	//dyld::log("addMappedRange(0x%lX->0x%lX) for %s\n", start, end, image->getShortName());
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		for (int i=0; i < MappedRanges::count; ++i) {
			if ( p->array[i].image == NULL ) {
				p->array[i].start = start;
				p->array[i].end = end;
				// add image field last with a barrier so that any reader will see consistent records
				OSMemoryBarrier();
				p->array[i].image = image;
				return;
			}
		}
	}
	// table must be full, chain another
	MappedRanges* newRanges = (MappedRanges*)malloc(sizeof(MappedRanges));
	bzero(newRanges, sizeof(MappedRanges));
	newRanges->array[0].start = start;
	newRanges->array[0].end = end;
	newRanges->array[0].image = image;
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		if ( p->next == NULL ) {
			OSMemoryBarrier();
			p->next = newRanges;
			break;
		}
	}
}

void removedMappedRanges(ImageLoader* image)
{
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		for (int i=0; i < MappedRanges::count; ++i) {
			if ( p->array[i].image == image ) {
				// clear with a barrier so that any reader will see consistent records
				OSMemoryBarrier();
				p->array[i].image = NULL;
			}
		}
	}
}

ImageLoader* findMappedRange(uintptr_t target)
{
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		for (int i=0; i < MappedRanges::count; ++i) {
			if ( p->array[i].image != NULL ) {
				if ( (p->array[i].start <= target) && (target < p->array[i].end) )
					return p->array[i].image;
			}
		}
	}
	return NULL;
}



const char* mkstringf(const char* format, ...)
{
	_SIMPLE_STRING buf = _simple_salloc();
	if ( buf != NULL ) {
		va_list	list;
		va_start(list, format);
		_simple_vsprintf(buf, format, list);
		va_end(list);
		const char*	t = strdup(_simple_string(buf));
		_simple_sfree(buf);
		if ( t != NULL )
			return t;
	}
	return "mkstringf, out of memory error";
}


void throwf(const char* format, ...) 
{
	_SIMPLE_STRING buf = _simple_salloc();
	if ( buf != NULL ) {
		va_list	list;
		va_start(list, format);
		_simple_vsprintf(buf, format, list);
		va_end(list);
		const char*	t = strdup(_simple_string(buf));
		_simple_sfree(buf);
		if ( t != NULL )
			throw t;
	}
	throw "throwf, out of memory error";
}


//#define ALTERNATIVE_LOGFILE "/dev/console"
static int sLogfile = STDERR_FILENO;

void log(const char* format, ...) 
{
	va_list	list;
	va_start(list, format);
	_simple_vdprintf(sLogfile, format, list);
	va_end(list);
}

void warn(const char* format, ...) 
{
	_simple_dprintf(sLogfile, "dyld: warning, ");
	va_list	list;
	va_start(list, format);
	_simple_vdprintf(sLogfile, format, list);
	va_end(list);
}


// utility class to assure files are closed when an exception is thrown
class FileOpener {
public:
	FileOpener(const char* path);
	~FileOpener();
	int getFileDescriptor() { return fd; }
private:
	int fd;
};

FileOpener::FileOpener(const char* path)
 : fd(-1)
{
	fd = open(path, O_RDONLY, 0);
}

FileOpener::~FileOpener()
{
	if ( fd != -1 )
		close(fd);
}


// forward declaration
#if __ppc__ || __i386__
bool isRosetta();
#endif


static void	registerDOFs(const std::vector<ImageLoader::DOFInfo>& dofs)
{
#if __ppc__
	// can't dtrace a program running emulated under rosetta rdar://problem/5179640
	if ( isRosetta() )
		return;
#endif
	const unsigned int dofSectionCount = dofs.size();
	if ( !sEnv.DYLD_DISABLE_DOFS && (dofSectionCount != 0) ) {
		int fd = open("/dev/" DTRACEMNR_HELPER, O_RDWR);
		if ( fd < 0 ) {
			//dyld::warn("can't open /dev/" DTRACEMNR_HELPER " to register dtrace DOF sections\n");
		}
		else {
			// allocate a buffer on the stack for the variable length dof_ioctl_data_t type
			uint8_t buffer[sizeof(dof_ioctl_data_t) + dofSectionCount*sizeof(dof_helper_t)];
			dof_ioctl_data_t* ioctlData = (dof_ioctl_data_t*)buffer;
			
			// fill in buffer with one dof_helper_t per DOF section
			ioctlData->dofiod_count = dofSectionCount;
			for (unsigned int i=0; i < dofSectionCount; ++i) {
				strlcpy(ioctlData->dofiod_helpers[i].dofhp_mod, dofs[i].imageShortName, DTRACE_MODNAMELEN);
				ioctlData->dofiod_helpers[i].dofhp_dof = (uintptr_t)(dofs[i].dof);
				ioctlData->dofiod_helpers[i].dofhp_addr = (uintptr_t)(dofs[i].dof);
			}
			
			// tell kernel about all DOF sections en mas
			// pass pointer to ioctlData because ioctl() only copies a fixed size amount of data into kernel
			user_addr_t val = (user_addr_t)(unsigned long)ioctlData;
			if ( ioctl(fd, DTRACEHIOC_ADDDOF, &val) != -1 ) {
				// kernel returns a unique identifier for each section in the dofiod_helpers[].dofhp_dof field.
				for (unsigned int i=0; i < dofSectionCount; ++i) {
					RegisteredDOF info;
					info.mh = dofs[i].imageHeader;
					info.registrationID = (int)(ioctlData->dofiod_helpers[i].dofhp_dof);
					sImageFilesNeedingDOFUnregistration.push_back(info);
					if ( gLinkContext.verboseDOF ) {
						dyld::log("dyld: registering DOF section 0x%p in %s with dtrace, ID=0x%08X\n", 
							dofs[i].dof, dofs[i].imageShortName, info.registrationID);
					}
				}
			}
			else {
				dyld::log( "dyld: ioctl to register dtrace DOF section failed\n");
			}
			close(fd);
		}
	}
}

static void	unregisterDOF(int registrationID)
{
	int fd = open("/dev/" DTRACEMNR_HELPER, O_RDWR);
	if ( fd < 0 ) {
		dyld::warn("can't open /dev/" DTRACEMNR_HELPER " to unregister dtrace DOF section\n");
	}
	else {
		ioctl(fd, DTRACEHIOC_REMOVE, registrationID);
		close(fd);
		if ( gLinkContext.verboseInit )
			dyld::warn("unregistering DOF section ID=0x%08X with dtrace\n", registrationID);
	}
}


//
// _dyld_register_func_for_add_image() is implemented as part of the general image state change notification
//
static void notifyAddImageCallbacks(ImageLoader* image)
{
	// use guard so that we cannot notify about the same image twice
	if ( ! image->addFuncNotified() ) {
		for (std::vector<ImageCallback>::iterator it=sAddImageCallbacks.begin(); it != sAddImageCallbacks.end(); it++)
			(*it)(image->machHeader(), image->getSlide());
		image->setAddFuncNotified();
	}
}


// notify gdb about these new images
static const char* notifyGDB(enum dyld_image_states state, uint32_t infoCount, const struct dyld_image_info info[])
{
	addImagesToAllImages(infoCount, info);
	return NULL;
}


static StateHandlers* stateToHandlers(dyld_image_states state, StateHandlers handlersArray[8]) 
{
	switch ( state ) {
		case dyld_image_state_mapped:
			return &handlersArray[0];
			
		case dyld_image_state_dependents_mapped:
			return &handlersArray[1];
			
		case dyld_image_state_rebased:
			return &handlersArray[2];
			
		case dyld_image_state_bound:
			return &handlersArray[3];
			
		case dyld_image_state_dependents_initialized:
			return &handlersArray[4];

		case dyld_image_state_initialized:
			return &handlersArray[5];
			
		case dyld_image_state_terminated:
			return &handlersArray[6];
	}
	return NULL;
}


static void notifySingle(dyld_image_states state, const ImageLoader* image)
{
	//dyld::log("notifySingle(state=%d, image=%s)\n", state, image->getPath());
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sSingleHandlers);
	if ( handlers != NULL ) {
		dyld_image_info info;
		info.imageLoadAddress	= image->machHeader();
		info.imageFilePath		= image->getPath();
		info.imageFileModDate	= image->lastModified();
		for (std::vector<dyld_image_state_change_handler>::iterator it = handlers->begin(); it != handlers->end(); ++it) {
			const char* result = (*it)(state, 1, &info);
			if ( (result != NULL) && (state == dyld_image_state_mapped) ) {
				//fprintf(stderr, "  image rejected by handler=%p\n", *it);
				// make copy of thrown string so that later catch clauses can free it
				const char* str = strdup(result);
				throw str;
			}
		}
	}
#if CORESYMBOLICATION_SUPPORT
    // mach message csdlc about dynamically loaded images 
    if ( dyld_all_image_infos.coreSymbolicationShmPage != NULL) {
		CSCppDyldSharedMemoryPage* connection = (CSCppDyldSharedMemoryPage*)dyld_all_image_infos.coreSymbolicationShmPage;
		if ( connection->is_valid_version() ) {
			if ( state == dyld_image_state_terminated ) {
				coresymbolication_unload_image(connection, image);
			}
		}
	}
#endif	
}


static int imageSorter(const void* l, const void* r)
{
	const ImageLoader* left = *((ImageLoader**)l);
	const ImageLoader* right= *((ImageLoader**)r);
	return left->compare(right);
}

static void notifyBatchPartial(dyld_image_states state, bool orLater, dyld_image_state_change_handler onlyHandler)
{
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sBatchHandlers);
	if ( handlers != NULL ) {
		// don't use a vector because it will use malloc/free and we want notifcation to be low cost
		ImageLoader* images[sAllImages.size()+1];
		ImageLoader** end = images;
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			dyld_image_states imageState = (*it)->getState();
			if ( (imageState == state) || (orLater && (imageState > state)) )
				*end++ = *it;
		}
		if ( sBundleBeingLoaded != NULL ) {
			dyld_image_states imageState = sBundleBeingLoaded->getState();
			if ( (imageState == state) || (orLater && (imageState > state)) )
				*end++ = sBundleBeingLoaded;
		}
		unsigned int count = end-images;
		if ( end != images ) {
			// sort bottom up
			qsort(images, count, sizeof(ImageLoader*), &imageSorter);
			// build info array
			dyld_image_info	infos[count];
			for (unsigned int i=0; i < count; ++i) {
				dyld_image_info* p = &infos[i];
				ImageLoader* image = images[i];
				//dyld::log("  state=%d, name=%s\n", state, image->getPath());
				p->imageLoadAddress = image->machHeader();
				p->imageFilePath = image->getPath();
				p->imageFileModDate = image->lastModified();
				// special case for add_image hook
				if ( state == dyld_image_state_bound )
					notifyAddImageCallbacks(image);
			}
			
			if ( onlyHandler != NULL ) {
				const char* result = (*onlyHandler)(state, count, infos);
				if ( (result != NULL) && (state == dyld_image_state_dependents_mapped) ) {
					//fprintf(stderr, "  images rejected by handler=%p\n", onlyHandler);
					// make copy of thrown string so that later catch clauses can free it
					const char* str = strdup(result);
					throw str;
				}
			}
			else {
				// call each handler with whole array 
				for (std::vector<dyld_image_state_change_handler>::iterator it = handlers->begin(); it != handlers->end(); ++it) {
					const char* result = (*it)(state, count, infos);
					if ( (result != NULL) && (state == dyld_image_state_dependents_mapped) ) {
						//fprintf(stderr, "  images rejected by handler=%p\n", *it);
						// make copy of thrown string so that later catch clauses can free it
						const char* str = strdup(result);
						throw str;
					}
				}
			}
		}
	}
#if CORESYMBOLICATION_SUPPORT
	if ( dyld_all_image_infos.coreSymbolicationShmPage != NULL) {
		CSCppDyldSharedMemoryPage* connection = (CSCppDyldSharedMemoryPage*)dyld_all_image_infos.coreSymbolicationShmPage;
		if ( connection->is_valid_version() ) {
			if ( state == dyld_image_state_rebased ) {
				// This needs to be captured now
				uint64_t load_timestamp = mach_absolute_time();
				for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
					dyld_image_states imageState = (*it)->getState();
					if ( (imageState == state) || (orLater && (imageState > state)) )
						coresymbolication_load_image(connection, *it, load_timestamp);
				}
			}
		}
	}
#endif
}

static void notifyBatch(dyld_image_states state)
{
	notifyBatchPartial(state, false, NULL);
}

// In order for register_func_for_add_image() callbacks to to be called bottom up,
// we need to maintain a list of root images. The main executable is usally the
// first root. Any images dynamically added are also roots (unless already loaded).
// If DYLD_INSERT_LIBRARIES is used, those libraries are first.
static void addRootImage(ImageLoader* image)
{
	//dyld::log("addRootImage(%p, %s)\n", image, image->getPath());
	// add to list of roots
	sImageRoots.push_back(image);
}


static void clearAllDepths()
{
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++)
		(*it)->clearDepth();
}

static unsigned int imageCount()
{
	return sAllImages.size();
}


static void setNewProgramVars(const ProgramVars& newVars)
{
	// make a copy of the pointers to program variables
	gLinkContext.programVars = newVars;
	
	// now set each program global to their initial value
	*gLinkContext.programVars.NXArgcPtr = gLinkContext.argc;
	*gLinkContext.programVars.NXArgvPtr = gLinkContext.argv;
	*gLinkContext.programVars.environPtr = gLinkContext.envp;
	*gLinkContext.programVars.__prognamePtr = gLinkContext.progname;
}

#if SUPPORT_OLD_CRT_INITIALIZATION
static void setRunInitialzersOldWay()
{
	gRunInitializersOldWay = true;		
}
#endif

static void addImage(ImageLoader* image)
{
	// add to master list
	sAllImages.push_back(image);
	
	// update mapped ranges
	uintptr_t lastSegStart = 0;
	uintptr_t lastSegEnd = 0;
	for(unsigned int i=0, e=image->segmentCount(); i < e; ++i) {
		if ( image->segUnaccessible(i) ) 
			continue;
		uintptr_t start = image->segActualLoadAddress(i);
		uintptr_t end = image->segActualEndAddress(i);
		if ( start == lastSegEnd ) {
			// two segments are contiguous, just record combined segments
			lastSegEnd = end;
		}
		else {
			// non-contiguous segments, record last (if any)
			if ( lastSegEnd != 0 )
				addMappedRange(image, lastSegStart, lastSegEnd);
			lastSegStart = start;
			lastSegEnd = end;
		}		
	}
	if ( lastSegEnd != 0 )
		addMappedRange(image, lastSegStart, lastSegEnd);

	
	if ( sEnv.DYLD_PRINT_LIBRARIES || (sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH && (sMainExecutable!=NULL) && sMainExecutable->isLinked()) ) {
		dyld::log("dyld: loaded: %s\n", image->getPath());
	}
	
}

void removeImage(ImageLoader* image)
{
	// if in termination list, pull it out and run terminator
	for (std::vector<ImageLoader*>::iterator it=sImageFilesNeedingTermination.begin(); it != sImageFilesNeedingTermination.end(); it++) {
		if ( *it == image ) {
			sImageFilesNeedingTermination.erase(it);
			image->doTermination(gLinkContext);
			break;
		}
	}
	
	// if has dtrace DOF section, tell dtrace it is going away, then remove from sImageFilesNeedingDOFUnregistration
	for (std::vector<RegisteredDOF>::iterator it=sImageFilesNeedingDOFUnregistration.begin(); it != sImageFilesNeedingDOFUnregistration.end(); ) {
		if ( it->mh == image->machHeader() ) {
			unregisterDOF(it->registrationID);
			sImageFilesNeedingDOFUnregistration.erase(it);
			// don't increment iterator, the erase caused next element to be copied to where this iterator points
		}
		else {
			++it;
		}
	}
	
	// tell all registered remove image handlers about this
	// do this before removing image from internal data structures so that the callback can query dyld about the image
	if ( image->getState() >= dyld_image_state_bound ) {
		for (std::vector<ImageCallback>::iterator it=sRemoveImageCallbacks.begin(); it != sRemoveImageCallbacks.end(); it++) {
			(*it)(image->machHeader(), image->getSlide());
		}
	}
	
	// notify 
	notifySingle(dyld_image_state_terminated, image);
	
	// remove from mapped images table
	removedMappedRanges(image);

	// remove from master list
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		if ( *it == image ) {
			sAllImages.erase(it);
			break;
		}
	}
	
	// flush find-by-address cache (do this after removed from master list, so there is no chance it can come back)
	if ( sLastImageByAddressCache == image )
		sLastImageByAddressCache = NULL;

	// if in root list, pull it out 
	for (std::vector<ImageLoader*>::iterator it=sImageRoots.begin(); it != sImageRoots.end(); it++) {
		if ( *it == image ) {
			sImageRoots.erase(it);
			break;
		}
	}

	// log if requested
	if ( sEnv.DYLD_PRINT_LIBRARIES || (sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH && (sMainExecutable!=NULL) && sMainExecutable->isLinked()) ) {
		dyld::log("dyld: unloaded: %s\n", image->getPath());
	}

	// tell gdb, new way
	removeImageFromAllImages(image->machHeader());
}


static void terminationRecorder(ImageLoader* image)
{
	sImageFilesNeedingTermination.push_back(image);
}

const char* getExecutablePath()
{
	return sExecPath;
}


void initializeMainExecutable()
{

	// record that we've reached this step
	gLinkContext.startedInitializingMainExecutable = true;

	// run initialzers for any inserted dylibs
	const int rootCount = sImageRoots.size();
	if ( rootCount > 1 ) {
		for(int i=1; i < rootCount; ++i)
			sImageRoots[i]->runInitializers(gLinkContext);
	}
	
	// run initializers for main executable and everything it brings up 
	sMainExecutable->runInitializers(gLinkContext);
	
	// register atexit() handler to run terminators in all loaded images when this process exits
	if ( gLibSystemHelpers != NULL ) 
		(*gLibSystemHelpers->cxa_atexit)(&runTerminators, NULL, NULL);

	// dump info if requested
	if ( sEnv.DYLD_PRINT_STATISTICS )
		ImageLoaderMachO::printStatistics(sAllImages.size());
}

bool mainExecutablePrebound()
{
	return sMainExecutable->usablePrebinding(gLinkContext);
}

ImageLoader* mainExecutable()
{
	return sMainExecutable;
}


void runTerminators(void* extra)
{
	const unsigned int imageCount = sImageFilesNeedingTermination.size();
	for(unsigned int i=imageCount; i > 0; --i){
		ImageLoader* image = sImageFilesNeedingTermination[i-1];
		image->doTermination(gLinkContext);
	}
	sImageFilesNeedingTermination.clear();
	notifyBatch(dyld_image_state_terminated);
}


//
// Turns a colon separated list of strings
// into a NULL terminated array of string 
// pointers.
//
static const char** parseColonList(const char* list)
{
	static const char* sEmptyList[] = { NULL };

	if ( list[0] == '\0' ) 
		return sEmptyList;
	
	int colonCount = 0;
	for(const char* s=list; *s != '\0'; ++s) {
		if (*s == ':') 
			++colonCount;
	}
	
	int index = 0;
	const char* start = list;
	char** result = new char*[colonCount+2];
	for(const char* s=list; *s != '\0'; ++s) {
		if (*s == ':') {
			int len = s-start;
			char* str = new char[len+1];
			strncpy(str, start, len);
			str[len] = '\0';
			start = &s[1];
			result[index++] = str;
		}
	}
	int len = strlen(start);
	char* str = new char[len+1];
	strcpy(str, start);
	result[index++] = str;
	result[index] = NULL;
	
	return (const char**)result;
}

 
static void paths_expand_roots(const char **paths, const char *key, const char *val)
{
// 	assert(val != NULL);
// 	assert(paths != NULL);
	if(NULL != key) {
		size_t keyLen = strlen(key);
		for(int i=0; paths[i] != NULL; ++i) {
			if ( strncmp(paths[i], key, keyLen) == 0 ) {
				char* newPath = new char[strlen(val) + (strlen(paths[i]) - keyLen) + 1];
				strcpy(newPath, val);
				strcat(newPath, &paths[i][keyLen]);
				paths[i] = newPath;
			}
		}
	}
	return;
}

static void removePathWithPrefix(const char* paths[], const char* prefix)
{
    size_t prefixLen = strlen(prefix);
    int skip = 0;
    int i;
    for(i = 0; paths[i] != NULL; ++i) {
        if ( strncmp(paths[i], prefix, prefixLen) == 0 )
            ++skip;
        else
            paths[i-skip] = paths[i];
    }
    paths[i-skip] = NULL;
}


#if 0
static void paths_dump(const char **paths)
{
//   assert(paths != NULL);
  const char **strs = paths;
  while(*strs != NULL)
  {
    dyld::log("\"%s\"\n", *strs);
    strs++;
  }
  return;
}
#endif

static void printOptions(const char* argv[])
{
	uint32_t i = 0;
	while ( NULL != argv[i] ) {
		dyld::log("opt[%i] = \"%s\"\n", i, argv[i]);
		i++;
	}
}

static void printEnvironmentVariables(const char* envp[])
{
	while ( NULL != *envp ) {
		dyld::log("%s\n", *envp);
		envp++;
	}
}

void processDyldEnvironmentVarible(const char* key, const char* value)
{
	if ( strcmp(key, "DYLD_FRAMEWORK_PATH") == 0 ) {
		sEnv.DYLD_FRAMEWORK_PATH = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_FALLBACK_FRAMEWORK_PATH") == 0 ) {
		sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_LIBRARY_PATH") == 0 ) {
		sEnv.DYLD_LIBRARY_PATH = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_FALLBACK_LIBRARY_PATH") == 0 ) {
		sEnv.DYLD_FALLBACK_LIBRARY_PATH = parseColonList(value);
	}
	else if ( (strcmp(key, "DYLD_ROOT_PATH") == 0) || (strcmp(key, "DYLD_PATHS_ROOT") == 0) ) {
		if ( strcmp(value, "/") != 0 ) {
			gLinkContext.rootPaths = parseColonList(value);
			for (int i=0; gLinkContext.rootPaths[i] != NULL; ++i) {
				if ( gLinkContext.rootPaths[i][0] != '/' ) {
					dyld::warn("DYLD_ROOT_PATH not used because it contains a non-absolute path\n");
					gLinkContext.rootPaths = NULL;
					break;
				}
			}
		}
	} 
	else if ( strcmp(key, "DYLD_IMAGE_SUFFIX") == 0 ) {
		gLinkContext.imageSuffix = value;
	}
	else if ( strcmp(key, "DYLD_INSERT_LIBRARIES") == 0 ) {
		sEnv.DYLD_INSERT_LIBRARIES = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_PRINT_OPTS") == 0 ) {
		sEnv.DYLD_PRINT_OPTS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_ENV") == 0 ) {
		sEnv.DYLD_PRINT_ENV = true;
	}
	else if ( strcmp(key, "DYLD_DISABLE_DOFS") == 0 ) {
		sEnv.DYLD_DISABLE_DOFS = true;
	}
	else if ( strcmp(key, "DYLD_DISABLE_PREFETCH") == 0 ) {
		gLinkContext.preFetchDisabled = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_LIBRARIES") == 0 ) {
		sEnv.DYLD_PRINT_LIBRARIES = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_LIBRARIES_POST_LAUNCH") == 0 ) {
		sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH = true;
	}
	else if ( strcmp(key, "DYLD_BIND_AT_LAUNCH") == 0 ) {
		sEnv.DYLD_BIND_AT_LAUNCH = true;
	}
	else if ( strcmp(key, "DYLD_FORCE_FLAT_NAMESPACE") == 0 ) {
		gLinkContext.bindFlat = true;
	}
	else if ( strcmp(key, "DYLD_NEW_LOCAL_SHARED_REGIONS") == 0 ) {
		// ignore, no longer relevant but some scripts still set it
	}
	else if ( strcmp(key, "DYLD_NO_FIX_PREBINDING") == 0 ) {
	}
	else if ( strcmp(key, "DYLD_PREBIND_DEBUG") == 0 ) {
		gLinkContext.verbosePrebinding = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_INITIALIZERS") == 0 ) {
		gLinkContext.verboseInit = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_DOFS") == 0 ) {
		gLinkContext.verboseDOF = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_STATISTICS") == 0 ) {
		sEnv.DYLD_PRINT_STATISTICS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_SEGMENTS") == 0 ) {
		gLinkContext.verboseMapping = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_BINDINGS") == 0 ) {
		gLinkContext.verboseBind = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_WEAK_BINDINGS") == 0 ) {
		gLinkContext.verboseWeakBind = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_REBASINGS") == 0 ) {
		gLinkContext.verboseRebase = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_APIS") == 0 ) {
		gLogAPIs = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_WARNINGS") == 0 ) {
		gLinkContext.verboseWarnings = true;
	}
	else if ( strcmp(key, "DYLD_NO_PIE") == 0 ) {
		gLinkContext.noPIE = true;
	}
	else if ( strcmp(key, "DYLD_SHARED_REGION") == 0 ) {
		if ( strcmp(value, "private") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
		}
		else if ( strcmp(value, "avoid") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		}
		else if ( strcmp(value, "use") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUseSharedRegion;
		}
		else if ( value[0] == '\0' ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUseSharedRegion;
		}
		else {
			dyld::warn("unknown option to DYLD_SHARED_REGION.  Valid options are: use, private, avoid\n");
		}
	}
#if DYLD_SHARED_CACHE_SUPPORT
	else if ( strcmp(key, "DYLD_SHARED_CACHE_DIR") == 0 ) {
        sSharedCacheDir = value;
	}
	else if ( strcmp(key, "DYLD_SHARED_CACHE_DONT_VALIDATE") == 0 ) {
		sSharedCacheIgnoreInodeAndTimeStamp = true;
	}
#endif
	else if ( strcmp(key, "DYLD_IGNORE_PREBINDING") == 0 ) {
		if ( strcmp(value, "all") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseNoPrebinding;
		}
		else if ( strcmp(value, "app") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseAllButAppPredbinding;
		}
		else if ( strcmp(value, "nonsplit") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseSplitSegPrebinding;
		}
		else if ( value[0] == '\0' ) {
			gLinkContext.prebindUsage = ImageLoader::kUseSplitSegPrebinding;
		}
		else {
			dyld::warn("unknown option to DYLD_IGNORE_PREBINDING.  Valid options are: all, app, nonsplit\n");
		}
	}
	else {
		dyld::warn("unknown environment variable: %s\n", key);
	}
}


//
// For security, setuid programs ignore DYLD_* environment variables.
// Additionally, the DYLD_* enviroment variables are removed
// from the environment, so that any child processes don't see them.
//
static void pruneEnvironmentVariables(const char* envp[], const char*** applep)
{
	// delete all DYLD_* and LD_LIBRARY_PATH environment variables
	int removedCount = 0;
	const char** d = envp;
	for(const char** s = envp; *s != NULL; s++) {
	    if ( (strncmp(*s, "DYLD_", 5) != 0) && (strncmp(*s, "LD_LIBRARY_PATH=", 16) != 0) ) {
			*d++ = *s;
		}
		else {
			++removedCount;
		}
	}
	*d++ = NULL;
	
	// slide apple parameters
	if ( removedCount > 0 ) {
		*applep = d;
		do {
			*d = d[removedCount];
		} while ( *d++ != NULL );
	}
	
	// disable framework and library fallback paths for setuid binaries rdar://problem/4589305
	sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = NULL;
	sEnv.DYLD_FALLBACK_LIBRARY_PATH = NULL;
}


static void checkEnvironmentVariables(const char* envp[], bool ignoreEnviron)
{
	const char* home = NULL;
	const char** p;
	for(p = envp; *p != NULL; p++) {
		const char* keyEqualsValue = *p;
	    if ( strncmp(keyEqualsValue, "DYLD_", 5) == 0 ) {
			const char* equals = strchr(keyEqualsValue, '=');
			if ( (equals != NULL) && !ignoreEnviron ) {
				const char* value = &equals[1];
				const int keyLen = equals-keyEqualsValue;
				char key[keyLen+1];
				strncpy(key, keyEqualsValue, keyLen);
				key[keyLen] = '\0';
				processDyldEnvironmentVarible(key, value);
			}
		}
	    else if ( strncmp(keyEqualsValue, "HOME=", 5) == 0 ) {
			home = &keyEqualsValue[5];
		}
		else if ( strncmp(keyEqualsValue, "LD_LIBRARY_PATH=", 16) == 0 ) {
			const char* path = &keyEqualsValue[16];
			sEnv.LD_LIBRARY_PATH = parseColonList(path);
		}
	}
	
	// default value for DYLD_FALLBACK_FRAMEWORK_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_FRAMEWORK_PATH == NULL ) {
		const char** paths = sFrameworkFallbackPaths;
		if ( home == NULL )
			removePathWithPrefix(paths, "$HOME");
		else
			paths_expand_roots(paths, "$HOME", home);
		sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = paths;
	}

	// default value for DYLD_FALLBACK_LIBRARY_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_LIBRARY_PATH == NULL ) {
		const char** paths = sLibraryFallbackPaths;
		if ( home == NULL ) 
			removePathWithPrefix(paths, "$HOME");
		else
			paths_expand_roots(paths, "$HOME", home);
		sEnv.DYLD_FALLBACK_LIBRARY_PATH = paths;
	}
}


static void getHostInfo()
{
#if 1
	struct host_basic_info info;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	mach_port_t hostPort = mach_host_self();
	kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
	if ( result != KERN_SUCCESS )
		throw "host_info() failed";
	
	sHostCPU		= info.cpu_type;
	sHostCPUsubtype = info.cpu_subtype;
#else
	size_t valSize = sizeof(sHostCPU);
	if (sysctlbyname ("hw.cputype", &sHostCPU, &valSize, NULL, 0) != 0) 
		throw "sysctlbyname(hw.cputype) failed";
	valSize = sizeof(sHostCPUsubtype);
	if (sysctlbyname ("hw.cpusubtype", &sHostCPUsubtype, &valSize, NULL, 0) != 0) 
		throw "sysctlbyname(hw.cpusubtype) failed";
#endif
}

static void checkSharedRegionDisable()
{
	#if !__LP64__
	// if main executable has segments that overlap the shared region, 
	// then disable using the shared region
	if ( sMainExecutable->overlapsWithAddressRange((void*)(uintptr_t)SHARED_REGION_BASE, (void*)(uintptr_t)(SHARED_REGION_BASE + SHARED_REGION_SIZE)) ) {
		gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		if ( gLinkContext.verboseMapping )
			dyld::warn("disabling shared region because main executable overlaps\n");
	}
	#endif
}

bool validImage(const ImageLoader* possibleImage)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		if ( possibleImage == sAllImages[i] ) {
			return true;
		}
	}
	return false;
}

uint32_t getImageCount()
{
	return sAllImages.size();
}

ImageLoader* getIndexedImage(unsigned int index)
{
	if ( index < sAllImages.size() )
		return sAllImages[index];
	return NULL;
}

ImageLoader* findImageByMachHeader(const struct mach_header* target)
{
	return findMappedRange((uintptr_t)target);
}


ImageLoader* findImageContainingAddress(const void* addr)
{
	return findMappedRange((uintptr_t)addr);
}


ImageLoader* findImageContainingSymbol(const void* symbol)
{
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* anImage = *it;
		if ( anImage->containsSymbol(symbol) )
			return anImage;
	}
	return NULL;
}



void forEachImageDo( void (*callback)(ImageLoader*, void* userData), void* userData)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		(*callback)(anImage, userData);
	}
}

ImageLoader* findLoadedImage(const struct stat& stat_buf)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		if ( anImage->statMatch(stat_buf) )
			return anImage;
	}
	return NULL;
}

// based on ANSI-C strstr()
static const char* strrstr(const char* str, const char* sub) 
{
	const int sublen = strlen(sub);
	for(const char* p = &str[strlen(str)]; p != str; --p) {
		if ( strncmp(p, sub, sublen) == 0 )
			return p;
	}
	return NULL;
}


//
// Find framework path
//
//  /path/foo.framework/foo								=>   foo.framework/foo	
//  /path/foo.framework/Versions/A/foo					=>   foo.framework/Versions/A/foo
//  /path/foo.framework/Frameworks/bar.framework/bar	=>   bar.framework/bar
//  /path/foo.framework/Libraries/bar.dylb				=>   NULL
//  /path/foo.framework/bar								=>   NULL
//
// Returns NULL if not a framework path
//
static const char* getFrameworkPartialPath(const char* path)
{
	const char* dirDot = strrstr(path, ".framework/");
	if ( dirDot != NULL ) {
		const char* dirStart = dirDot;
		for ( ; dirStart >= path; --dirStart) {
			if ( (*dirStart == '/') || (dirStart == path) ) {
				const char* frameworkStart = &dirStart[1];
				if ( dirStart == path )
					--frameworkStart;
				int len = dirDot - frameworkStart;
				char framework[len+1];
				strncpy(framework, frameworkStart, len);
				framework[len] = '\0';
				const char* leaf = strrchr(path, '/');
				if ( leaf != NULL ) {
					if ( strcmp(framework, &leaf[1]) == 0 ) {
						return frameworkStart;
					}
					if (  gLinkContext.imageSuffix != NULL ) {
						// some debug frameworks have install names that end in _debug
						if ( strncmp(framework, &leaf[1], len) == 0 ) {
							if ( strcmp( gLinkContext.imageSuffix, &leaf[len+1]) == 0 )
								return frameworkStart;
						}
					}
				}
			}
		}
	}
	return NULL;
}


static const char* getLibraryLeafName(const char* path)
{
	const char* start = strrchr(path, '/');
	if ( start != NULL )
		return &start[1];
	else
		return path;
}


// only for architectures that use cpu-sub-types
#if CPU_SUBTYPES_SUPPORTED 

const cpu_subtype_t CPU_SUBTYPE_END_OF_LIST = -1;


//
//	A fat file may contain multiple sub-images for the same CPU type.
//	In that case, dyld picks which sub-image to use by scanning a table
//	of preferred cpu-sub-types for the running cpu.  
//	
//	There is one row in the table for each cpu-sub-type on which dyld might run.
//  The first entry in a row is that cpu-sub-type.  It is followed by all
//	cpu-sub-types that can run on that cpu, if preferred order.  Each row ends with 
//	a "SUBTYPE_ALL" (to denote that images written to run on any cpu-sub-type are usable), 
//  followed by one or more CPU_SUBTYPE_END_OF_LIST to pad out this row.
//


#if __ppc__
//	 
//	32-bit PowerPC sub-type lists
//
const int kPPC_RowCount = 4;
static const cpu_subtype_t kPPC32[kPPC_RowCount][6] = { 
	// G5 can run any code
	{  CPU_SUBTYPE_POWERPC_970, CPU_SUBTYPE_POWERPC_7450,  CPU_SUBTYPE_POWERPC_7400, CPU_SUBTYPE_POWERPC_750, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST },
	
	// G4 can run all but G5 code
	{  CPU_SUBTYPE_POWERPC_7450,  CPU_SUBTYPE_POWERPC_7400,  CPU_SUBTYPE_POWERPC_750, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
	{  CPU_SUBTYPE_POWERPC_7400,  CPU_SUBTYPE_POWERPC_7450,  CPU_SUBTYPE_POWERPC_750, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },

	// G3 cannot run G4 or G5 code
	{ CPU_SUBTYPE_POWERPC_750,  CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST,  CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST }
};
#endif


#if __arm__
//      
//     ARM sub-type lists
//
const int kARM_RowCount = 5;
static const cpu_subtype_t kARM[kARM_RowCount][6] = { 
	// armv7 can run: v7, v6, v5, and v4
	{  CPU_SUBTYPE_ARM_V7, CPU_SUBTYPE_ARM_V6, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST },
	
	// armv6 can run: v6, v5, and v4
	{  CPU_SUBTYPE_ARM_V6, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
	
	// xscale can run: xscale, v5, and v4
	{  CPU_SUBTYPE_ARM_XSCALE, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
	
	// armv5 can run: v5 and v4
	{  CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },

	// armv4 can run: v4
	{  CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
};
#endif


// scan the tables above to find the cpu-sub-type-list for this machine
static const cpu_subtype_t* findCPUSubtypeList(cpu_type_t cpu, cpu_subtype_t subtype)
{
	switch (cpu) {
#if __ppc__
		case CPU_TYPE_POWERPC:
			for (int i=0; i < kPPC_RowCount ; ++i) {
				if ( kPPC32[i][0] == subtype )
					return kPPC32[i];
			}
			break;
#endif
#if __arm__
		case CPU_TYPE_ARM:
			for (int i=0; i < kARM_RowCount ; ++i) {
				if ( kARM[i][0] == subtype )
					return kARM[i];
			}
			break;
#endif
	}
	return NULL;
}




// scan fat table-of-contents for best most preferred subtype
static bool fatFindBestFromOrderedList(cpu_type_t cpu, const cpu_subtype_t list[], const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* const archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for (uint32_t subTypeIndex=0; list[subTypeIndex] != CPU_SUBTYPE_END_OF_LIST; ++subTypeIndex) {
		for(uint32_t fatIndex=0; fatIndex < OSSwapBigToHostInt32(fh->nfat_arch); ++fatIndex) {
			if ( ((cpu_type_t)OSSwapBigToHostInt32(archs[fatIndex].cputype) == cpu) 
				&& (list[subTypeIndex] == (cpu_subtype_t)OSSwapBigToHostInt32(archs[fatIndex].cpusubtype)) ) {
				*offset = OSSwapBigToHostInt32(archs[fatIndex].offset);
				*len = OSSwapBigToHostInt32(archs[fatIndex].size);
				return true;
			}
		}
	}
	return false;
}

// scan fat table-of-contents for exact match of cpu and cpu-sub-type
static bool fatFindExactMatch(cpu_type_t cpu, cpu_subtype_t subtype, const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( ((cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == cpu)
			&& ((cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == subtype) ) {
			*offset = OSSwapBigToHostInt32(archs[i].offset);
			*len = OSSwapBigToHostInt32(archs[i].size);
			return true;
		}
	}
	return false;
}

// scan fat table-of-contents for image with matching cpu-type and runs-on-all-sub-types
static bool fatFindRunsOnAllCPUs(cpu_type_t cpu, const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( (cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == cpu) {
			switch (cpu) {
#if __ppc__
				case CPU_TYPE_POWERPC:
					if ( (cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == CPU_SUBTYPE_POWERPC_ALL ) {
						*offset = OSSwapBigToHostInt32(archs[i].offset);
						*len = OSSwapBigToHostInt32(archs[i].size);
						return true;
					}
					break;
#endif
#if __arm__
				case CPU_TYPE_ARM:
					if ( (cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == CPU_SUBTYPE_ARM_ALL ) {
						*offset = OSSwapBigToHostInt32(archs[i].offset);
						*len = OSSwapBigToHostInt32(archs[i].size);
						return true;
					}
					break;
#endif
			}
		}
	}
	return false;
}

#endif // CPU_SUBTYPES_SUPPORTED

//
// A fat file may contain multiple sub-images for the same cpu-type,
// each optimized for a different cpu-sub-type (e.g G3 or G5).
// This routine picks the optimal sub-image.
//
static bool fatFindBest(const fat_header* fh, uint64_t* offset, uint64_t* len)
{
#if CPU_SUBTYPES_SUPPORTED
	// assume all dylibs loaded must have same cpu type as main executable
	const cpu_type_t cpu = sMainExecutableMachHeader->cputype;

	// We only know the subtype to use if the main executable cpu type matches the host
	if ( (cpu & CPU_TYPE_MASK) == sHostCPU ) {
		// get preference ordered list of subtypes
		const cpu_subtype_t* subTypePreferenceList = findCPUSubtypeList(cpu, sHostCPUsubtype);
	
		// use ordered list to find best sub-image in fat file
		if ( subTypePreferenceList != NULL ) 
			return fatFindBestFromOrderedList(cpu, subTypePreferenceList, fh, offset, len);
		
		// if running cpu is not in list, try for an exact match
		if ( fatFindExactMatch(cpu, sHostCPUsubtype, fh, offset, len) )
			return true;
	}
	
	// running on an uknown cpu, can only load generic code
	return fatFindRunsOnAllCPUs(cpu, fh, offset, len);
#else
	// just find first slice with matching architecture
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( (cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == sMainExecutableMachHeader->cputype) {
			*offset = OSSwapBigToHostInt32(archs[i].offset);
			*len = OSSwapBigToHostInt32(archs[i].size);
			return true;
		}
	}
	return false;
#endif
}



//
// This is used to validate if a non-fat (aka thin or raw) mach-o file can be used
// on the current processor. //
bool isCompatibleMachO(const uint8_t* firstPage, const char* path)
{
#if CPU_SUBTYPES_SUPPORTED
	// It is deemed compatible if any of the following are true:
	//  1) mach_header subtype is in list of compatible subtypes for running processor
	//  2) mach_header subtype is same as running processor subtype
	//  3) mach_header subtype runs on all processor variants
	const mach_header* mh = (mach_header*)firstPage;
	if ( mh->magic == sMainExecutableMachHeader->magic ) {
		if ( mh->cputype == sMainExecutableMachHeader->cputype ) {
			if ( (mh->cputype & CPU_TYPE_MASK) == sHostCPU ) {
				// get preference ordered list of subtypes that this machine can use
				const cpu_subtype_t* subTypePreferenceList = findCPUSubtypeList(mh->cputype, sHostCPUsubtype);
				if ( subTypePreferenceList != NULL ) {
					// if image's subtype is in the list, it is compatible
					for (const cpu_subtype_t* p = subTypePreferenceList; *p != CPU_SUBTYPE_END_OF_LIST; ++p) {
						if ( *p == mh->cpusubtype )
							return true;
					}
					// have list and not in list, so not compatible
					throwf("incompatible cpu-subtype: 0x%08X in %s", mh->cpusubtype, path);
				}
				// unknown cpu sub-type, but if exact match for current subtype then ok to use
				if ( mh->cpusubtype == sHostCPUsubtype ) 
					return true;
			}
			
			// cpu type has no ordered list of subtypes
			switch (mh->cputype) {
				case CPU_TYPE_POWERPC:
					// allow _ALL to be used by any client
					if ( mh->cpusubtype == CPU_SUBTYPE_POWERPC_ALL ) 
						return true;
					break;
				case CPU_TYPE_POWERPC64:
				case CPU_TYPE_I386:
				case CPU_TYPE_X86_64:
					// subtypes are not used or these architectures
					return true;
			}
		}
	}
#else
	// For architectures that don't support cpu-sub-types
	// this just check the cpu type.
	const mach_header* mh = (mach_header*)firstPage;
	if ( mh->magic == sMainExecutableMachHeader->magic ) {
		if ( mh->cputype == sMainExecutableMachHeader->cputype ) {
			return true;
		}
	}
#endif
	return false;
}




// The kernel maps in main executable before dyld gets control.  We need to 
// make an ImageLoader* for the already mapped in main executable.
static ImageLoader* instantiateFromLoadedImage(const macho_header* mh, uintptr_t slide, const char* path)
{
	// try mach-o loader
	if ( isCompatibleMachO((const uint8_t*)mh, path) ) {
		ImageLoader* image = ImageLoaderMachO::instantiateMainExecutable(mh, slide, path, gLinkContext);
		addImage(image);
		return image;
	}
	
	throw "main executable not a known format";
}

#if DYLD_SHARED_CACHE_SUPPORT
bool inSharedCache(const char* path)
{
	if ( sSharedCache != NULL ) {
		struct stat stat_buf;
		if ( stat(path, &stat_buf) == -1 )
			return false;
		
		// walk shared cache to see if there is a cached image that matches the inode/mtime/path desired
		const dyld_cache_image_info* const start = (dyld_cache_image_info*)((uint8_t*)sSharedCache + sSharedCache->imagesOffset);
		const dyld_cache_image_info* const end = &start[sSharedCache->imagesCount];
		for( const dyld_cache_image_info* p = start; p != end; ++p) {
			// check mtime and inode first because it is fast
			if ( sSharedCacheIgnoreInodeAndTimeStamp 
				|| ( ((time_t)p->modTime == stat_buf.st_mtime) && ((ino_t)p->inode == stat_buf.st_ino) ) ) {
				// mod-time and inode match an image in the shared cache, now check path
				const char* pathInCache = (char*)sSharedCache + p->pathFileOffset;
				bool cacheHit = (strcmp(path, pathInCache) == 0);
				if ( ! cacheHit ) {
					// path does not match install name of dylib in cache, but inode and mtime does match
					// perhaps path is a symlink to the cached dylib
					struct stat pathInCacheStatBuf;
					if ( stat(pathInCache, &pathInCacheStatBuf) != -1 )
						cacheHit = ( (pathInCacheStatBuf.st_dev == stat_buf.st_dev) && (pathInCacheStatBuf.st_ino == stat_buf.st_ino) );	
				}
				if ( cacheHit ) {
					// found image in cache
					return true;
				}
			}
		}	
	}
	return false;
}

static ImageLoader* findSharedCacheImage(const struct stat& stat_buf, const char* path)
{
	if ( sSharedCache != NULL ) {
		// walk shared cache to see if there is a cached image that matches the inode/mtime/path desired
		const dyld_cache_image_info* const start = (dyld_cache_image_info*)((uint8_t*)sSharedCache + sSharedCache->imagesOffset);
		const dyld_cache_image_info* const end = &start[sSharedCache->imagesCount];
		for( const dyld_cache_image_info* p = start; p != end; ++p) {
			// check mtime and inode first because it is fast
			if ( sSharedCacheIgnoreInodeAndTimeStamp 
				|| ( ((time_t)p->modTime == stat_buf.st_mtime) && ((ino_t)p->inode == stat_buf.st_ino) ) ) {
				// mod-time and inode match an image in the shared cache, now check path
				const char* pathInCache = (char*)sSharedCache + p->pathFileOffset;
				bool cacheHit = (strcmp(path, pathInCache) == 0);
				if ( ! cacheHit ) {
					// path does not match install name of dylib in cache, but inode and mtime does match
					// perhaps path is a symlink to the cached dylib
					struct stat pathInCacheStatBuf;
					if ( stat(pathInCache, &pathInCacheStatBuf) != -1 )
						cacheHit = ( (pathInCacheStatBuf.st_dev == stat_buf.st_dev) && (pathInCacheStatBuf.st_ino == stat_buf.st_ino) );	
				}
				if ( cacheHit ) {
					// found image in cache, instantiate an ImageLoader with it
					return ImageLoaderMachO::instantiateFromCache((macho_header*)(p->address), pathInCache, stat_buf, gLinkContext);
				}
			}
		}	
	}
	return NULL;
}
#endif

static ImageLoader* checkandAddImage(ImageLoader* image, const LoadContext& context)
{
	// now sanity check that this loaded image does not have the same install path as any existing image
	const char* loadedImageInstallPath = image->getInstallPath();
	if ( image->isDylib() && (loadedImageInstallPath != NULL) && (loadedImageInstallPath[0] == '/') ) {
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* anImage = *it;
			const char* installPath = anImage->getInstallPath();
			if ( installPath != NULL) {
				if ( strcmp(loadedImageInstallPath, installPath) == 0 ) {
					//dyld::log("duplicate(%s) => %p\n", installPath, anImage);
					ImageLoader::deleteImage(image);
					return anImage;
				}
			}
		}
	}

	// some API's restrict what they can load
	if ( context.mustBeBundle && !image->isBundle() )
		throw "not a bundle";
	if ( context.mustBeDylib && !image->isDylib() )
		throw "not a dylib";

	// regular main executables cannot be loaded 
	if ( image->isExecutable() ) {
		if ( !context.canBePIE || !image->isPositionIndependentExecutable() )
			throw "can't load a main executable";
	}
	
	// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
	if ( ! image->isBundle() ) 
		addImage(image);
	
	return image;
}

// map in file and instantiate an ImageLoader
static ImageLoader* loadPhase6(int fd, struct stat& stat_buf, const char* path, const LoadContext& context)
{
	//dyld::log("%s(%s)\n", __func__ , path);
	uint64_t fileOffset = 0;
	uint64_t fileLength = stat_buf.st_size;

	// validate it is a file (not directory)
	if ( (stat_buf.st_mode & S_IFMT) != S_IFREG ) 
		throw "not a file";

	uint8_t firstPage[4096];
	bool shortPage = false;
	
	// min mach-o file is 4K
	if ( fileLength < 4096 ) {
		if ( pread(fd, firstPage, fileLength, 0) != (ssize_t)fileLength )
			throwf("pread of short file failed: %d", errno);
		shortPage = true;
	} 
	else {
		if ( pread(fd, firstPage, 4096,0) != 4096 )
			throwf("pread of first 4K failed: %d", errno);
	}
	
	// if fat wrapper, find usable sub-file
	const fat_header* fileStartAsFat = (fat_header*)firstPage;
	if ( fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( fatFindBest(fileStartAsFat, &fileOffset, &fileLength) ) {
			if ( (fileOffset+fileLength) > (uint64_t)(stat_buf.st_size) )
				throwf("truncated fat file.  file length=%llu, but needed slice goes to %llu", stat_buf.st_size, fileOffset+fileLength);
			if (pread(fd, firstPage, 4096, fileOffset) != 4096)
				throwf("pread of fat file failed: %d", errno);
		}
		else {
			throw "no matching architecture in universal wrapper";
		}
	}
	
	// try mach-o loader
	if ( isCompatibleMachO(firstPage, path) ) {
		if ( shortPage ) 
			throw "file too short";

		// instantiate an image
		ImageLoader* image = ImageLoaderMachO::instantiateFromFile(path, fd, firstPage, fileOffset, fileLength, stat_buf, gLinkContext);
		
		// validate
		return checkandAddImage(image, context);
	}
	
	// try other file formats here...
	
	
	// throw error about what was found
	switch (*(uint32_t*)firstPage) {
		case MH_MAGIC:
		case MH_CIGAM:
		case MH_MAGIC_64:
		case MH_CIGAM_64:
			throw "mach-o, but wrong architecture";
		default:
		throwf("unknown file type, first eight bytes: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
			firstPage[0], firstPage[1], firstPage[2], firstPage[3], firstPage[4], firstPage[5], firstPage[6],firstPage[7]);
	}
}


// try to open file
static ImageLoader* loadPhase5open(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;

	// just return NULL if file not found, but record any other errors
	struct stat stat_buf;
	if ( stat(path, &stat_buf) == -1 ) {
		int err = errno;
		if ( err != ENOENT ) {
			exceptions->push_back(dyld::mkstringf("%s: stat() failed with errno=%d", path, err));
		}
		return NULL;
	}
	
	// in case image was renamed or found via symlinks, check for inode match
	image = findLoadedImage(stat_buf);
	if ( image != NULL )
		return image;
	
	// do nothing if not already loaded and if RTLD_NOLOAD or NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED
	if ( context.dontLoad )
		return NULL;

#if DYLD_SHARED_CACHE_SUPPORT
	// see if this image is in shared cache
	image = findSharedCacheImage(stat_buf, path);
	if ( image != NULL ) {
		return checkandAddImage(image, context);
	}
#endif
	
	// open file (automagically closed when this function exits)
	FileOpener file(path);
		
	// just return NULL if file not found, but record any other errors
	if ( file.getFileDescriptor() == -1 ) {
		int err = errno;
		if ( err != ENOENT ) {
			const char* newMsg = dyld::mkstringf("%s: open() failed with errno=%d", path, err);
			exceptions->push_back(newMsg);
		}
		return NULL;
	}

	try {
		return loadPhase6(file.getFileDescriptor(), stat_buf, path, context);
	}
	catch (const char* msg) {
		const char* newMsg = dyld::mkstringf("%s: %s", path, msg);
		exceptions->push_back(newMsg);
		free((void*)msg);
		return NULL;
	}
}

// look for path match with existing loaded images
static ImageLoader* loadPhase5check(const char* path, const LoadContext& context)
{
	//dyld::log("%s(%s)\n", __func__ , path);
	// search path against load-path and install-path of all already loaded images
	uint32_t hash = ImageLoader::hash(path);
	//dyld::log("check() hash=%d, path=%s\n", hash, path);
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* anImage = *it;
		// check hash first to cut down on strcmp calls
		//dyld::log("    check() hash=%d, path=%s\n", anImage->getPathHash(), anImage->getPath());
		if ( anImage->getPathHash() == hash )
			if ( strcmp(path, anImage->getPath()) == 0 ) {
				// if we are looking for a dylib don't return something else
				if ( !context.mustBeDylib || anImage->isDylib() )
					return anImage;
			}
		if ( context.matchByInstallName || anImage->matchInstallPath() ) {
			const char* installPath = anImage->getInstallPath();
			if ( installPath != NULL) {
				if ( strcmp(path, installPath) == 0 ) {
					// if we are looking for a dylib don't return something else
					if ( !context.mustBeDylib || anImage->isDylib() )
						return anImage;
				}
			}
		}
	}
	
	//dyld::log("%s(%s) => NULL\n", __func__,   path);
	return NULL;
}


// open or check existing
static ImageLoader* loadPhase5(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	if ( exceptions != NULL ) 
		return loadPhase5open(path, context, exceptions);
	else
		return loadPhase5check(path, context);
}

// try with and without image suffix
static ImageLoader* loadPhase4(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;
	if (  gLinkContext.imageSuffix != NULL ) {
		char pathWithSuffix[strlen(path)+strlen( gLinkContext.imageSuffix)+2];
		ImageLoader::addSuffix(path,  gLinkContext.imageSuffix, pathWithSuffix);
		image = loadPhase5(pathWithSuffix, context, exceptions);
	}
	if ( image == NULL )
		image = loadPhase5(path, context, exceptions);
	return image;
}

static ImageLoader* loadPhase2(const char* path, const LoadContext& context, 
							   const char* const frameworkPaths[], const char* const libraryPaths[], 
							   std::vector<const char*>* exceptions); // forward reference


// expand @ variables
static ImageLoader* loadPhase3(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;
	if ( strncmp(path, "@executable_path/", 17) == 0 ) {
		// executable_path cannot be in used in any binary in a setuid process rdar://problem/4589305
		if ( sProcessIsRestricted ) 
			throwf("unsafe use of @executable_path in %s with restricted binary", context.origin);
		// handle @executable_path path prefix
		const char* executablePath = sExecPath;
		char newPath[strlen(executablePath) + strlen(path)];
		strcpy(newPath, executablePath);
		char* addPoint = strrchr(newPath,'/');
		if ( addPoint != NULL )
			strcpy(&addPoint[1], &path[17]);
		else
			strcpy(newPath, &path[17]);
		image = loadPhase4(newPath, context, exceptions);
		if ( image != NULL ) 
			return image;

		// perhaps main executable path is a sym link, find realpath and retry
		char resolvedPath[PATH_MAX];
		if ( realpath(sExecPath, resolvedPath) != NULL ) {
			char newRealPath[strlen(resolvedPath) + strlen(path)];
			strcpy(newRealPath, resolvedPath);
			char* addPoint = strrchr(newRealPath,'/');
			if ( addPoint != NULL )
				strcpy(&addPoint[1], &path[17]);
			else
				strcpy(newRealPath, &path[17]);
			image = loadPhase4(newRealPath, context, exceptions);
			if ( image != NULL ) 
				return image;
		}
	}
	else if ( (strncmp(path, "@loader_path/", 13) == 0) && (context.origin != NULL) ) {
		// @loader_path cannot be used from the main executable of a setuid process rdar://problem/4589305
		if ( sProcessIsRestricted && (strcmp(context.origin, sExecPath) == 0) )
			throwf("unsafe use of @loader_path in %s with restricted binary", context.origin);
		// handle @loader_path path prefix
		char newPath[strlen(context.origin) + strlen(path)];
		strcpy(newPath, context.origin);
		char* addPoint = strrchr(newPath,'/');
		if ( addPoint != NULL )
			strcpy(&addPoint[1], &path[13]);
		else
			strcpy(newPath, &path[13]);
		image = loadPhase4(newPath, context, exceptions);
		if ( image != NULL ) 
			return image;
		
		// perhaps loader path is a sym link, find realpath and retry
		char resolvedPath[PATH_MAX];
		if ( realpath(context.origin, resolvedPath) != NULL ) {
			char newRealPath[strlen(resolvedPath) + strlen(path)];
			strcpy(newRealPath, resolvedPath);
			char* addPoint = strrchr(newRealPath,'/');
			if ( addPoint != NULL )
				strcpy(&addPoint[1], &path[13]);
			else
				strcpy(newRealPath, &path[13]);
			image = loadPhase4(newRealPath, context, exceptions);
			if ( image != NULL ) 
				return image;
		}
	}
	else if ( context.implicitRPath || (strncmp(path, "@rpath/", 7) == 0) ) {
		const char* trailingPath = (strncmp(path, "@rpath/", 7) == 0) ? &path[7] : path;
		// substitute @rpath with all -rpath paths up the load chain
		for(const ImageLoader::RPathChain* rp=context.rpath; rp != NULL; rp=rp->next) {
			if (rp->paths != NULL ) {
				for(std::vector<const char*>::iterator it=rp->paths->begin(); it != rp->paths->end(); ++it) {
					const char* anRPath = *it;
					char newPath[strlen(anRPath) + strlen(trailingPath)+2];
					strcpy(newPath, anRPath);
					strcat(newPath, "/"); 
					strcat(newPath, trailingPath); 
					image = loadPhase4(newPath, context, exceptions);
					if ( image != NULL ) 
						return image;
				}
			}
		}
		
		// substitute @rpath with LD_LIBRARY_PATH
		if ( sEnv.LD_LIBRARY_PATH != NULL ) {
			image = loadPhase2(trailingPath, context, NULL, sEnv.LD_LIBRARY_PATH, exceptions);
			if ( image != NULL )
				return image;
		}
		
		// if this is the "open" pass, don't try to open @rpath/... as a relative path
		if ( (exceptions != NULL) && (trailingPath != path) )
			return NULL;
	}
	else if (sProcessIsRestricted && (path[0] != '/' )) {
		throwf("unsafe use of relative rpath %s in %s with restricted binary", path, context.origin);
	}
	
	return loadPhase4(path, context, exceptions);
}


// try search paths
static ImageLoader* loadPhase2(const char* path, const LoadContext& context, 
							   const char* const frameworkPaths[], const char* const libraryPaths[], 
							   std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;
	const char* frameworkPartialPath = getFrameworkPartialPath(path);
	if ( frameworkPaths != NULL ) {
		if ( frameworkPartialPath != NULL ) {
			const int frameworkPartialPathLen = strlen(frameworkPartialPath);
			for(const char* const* fp = frameworkPaths; *fp != NULL; ++fp) {
				char npath[strlen(*fp)+frameworkPartialPathLen+8];
				strcpy(npath, *fp);
				strcat(npath, "/");
				strcat(npath, frameworkPartialPath);
				//dyld::log("dyld: fallback framework path used: %s() -> loadPhase4(\"%s\", ...)\n", __func__, npath);
				image = loadPhase4(npath, context, exceptions);
				if ( image != NULL )
					return image;
			}
		}
	}
	if ( libraryPaths != NULL ) {
		const char* libraryLeafName = getLibraryLeafName(path);
		const int libraryLeafNameLen = strlen(libraryLeafName);
		for(const char* const* lp = libraryPaths; *lp != NULL; ++lp) {
			char libpath[strlen(*lp)+libraryLeafNameLen+8];
			strcpy(libpath, *lp);
			strcat(libpath, "/");
			strcat(libpath, libraryLeafName);
			//dyld::log("dyld: fallback library path used: %s() -> loadPhase4(\"%s\", ...)\n", __func__, libpath);
			image = loadPhase4(libpath, context, exceptions);
			if ( image != NULL )
				return image;
		}
	}
	return NULL;
}

// try search overrides and fallbacks
static ImageLoader* loadPhase1(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;

	// handle LD_LIBRARY_PATH environment variables that force searching
	if ( context.useLdLibraryPath && (sEnv.LD_LIBRARY_PATH != NULL) ) {
		image = loadPhase2(path, context, NULL, sEnv.LD_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}

	// handle DYLD_ environment variables that force searching
	if ( context.useSearchPaths && ((sEnv.DYLD_FRAMEWORK_PATH != NULL) || (sEnv.DYLD_LIBRARY_PATH != NULL)) ) {
		image = loadPhase2(path, context, sEnv.DYLD_FRAMEWORK_PATH, sEnv.DYLD_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}
	
	// try raw path
	image = loadPhase3(path, context, exceptions);
	if ( image != NULL )
		return image;
	
	// try fallback paths during second time (will open file)
	const char* const* fallbackLibraryPaths = sEnv.DYLD_FALLBACK_LIBRARY_PATH;
	if ( (fallbackLibraryPaths != NULL) && !context.useFallbackPaths )
		fallbackLibraryPaths = NULL;
	if ( !context.dontLoad  && (exceptions != NULL) && ((sEnv.DYLD_FALLBACK_FRAMEWORK_PATH != NULL) || (fallbackLibraryPaths != NULL)) ) {
		image = loadPhase2(path, context, sEnv.DYLD_FALLBACK_FRAMEWORK_PATH, fallbackLibraryPaths, exceptions);
		if ( image != NULL )
			return image;
	}
		
	return NULL;
}

// try root substitutions
static ImageLoader* loadPhase0(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);

	// handle DYLD_ROOT_PATH which forces absolute paths to use a new root
	if ( (gLinkContext.rootPaths != NULL) && (path[0] == '/') ) {
		for(const char* const* rootPath = gLinkContext.rootPaths ; *rootPath != NULL; ++rootPath) {
			char newPath[strlen(*rootPath) + strlen(path)+2];
			strcpy(newPath, *rootPath);
			strcat(newPath, path);
			ImageLoader* image = loadPhase1(newPath, context, exceptions);
			if ( image != NULL )
				return image;
		}
	}

	// try raw path
	return loadPhase1(path, context, exceptions);
}

//
// Given all the DYLD_ environment variables, the general case for loading libraries
// is that any given path expands into a list of possible locations to load.  We
// also must take care to ensure two copies of the "same" library are never loaded.
//
// The algorithm used here is that there is a separate function for each "phase" of the
// path expansion.  Each phase function calls the next phase with each possible expansion
// of that phase.  The result is the last phase is called with all possible paths.  
//
// To catch duplicates the algorithm is run twice.  The first time, the last phase checks
// the path against all loaded images.  The second time, the last phase calls open() on 
// the path.  Either time, if an image is found, the phases all unwind without checking
// for other paths.
//
ImageLoader* load(const char* path, const LoadContext& context)
{
	//dyld::log("%s(%s)\n", __func__ , path);
	char realPath[PATH_MAX];
	// when DYLD_IMAGE_SUFFIX is in used, do a realpath(), otherwise a load of "Foo.framework/Foo" will not match
	if ( context.useSearchPaths && ( gLinkContext.imageSuffix != NULL) ) {
		if ( realpath(path, realPath) != NULL )
			path = realPath;
	}
	
	// try all path permutations and check against existing loaded images
	ImageLoader* image = loadPhase0(path, context, NULL);
	if ( image != NULL )
		return image;

	// try all path permutations and try open() until first sucesss
	std::vector<const char*> exceptions;
	image = loadPhase0(path, context, &exceptions);
	if ( image != NULL )
		return image;
	else if ( exceptions.size() == 0 ) {
		if ( context.dontLoad )
			return NULL;
		else
			throw "image not found";
	}
	else {
		const char* msgStart = "no suitable image found.  Did find:";
		const char* delim = "\n\t";
		size_t allsizes = strlen(msgStart)+8;
		for (unsigned int i=0; i < exceptions.size(); ++i) 
			allsizes += (strlen(exceptions[i]) + strlen(delim));
		char* fullMsg = new char[allsizes];
		strcpy(fullMsg, msgStart);
		for (unsigned int i=0; i < exceptions.size(); ++i) {
			strcat(fullMsg, delim);
			strcat(fullMsg, exceptions[i]);
			free((void*)exceptions[i]);
		}
		throw (const char*)fullMsg;
	}
}




#if DYLD_SHARED_CACHE_SUPPORT


// hack until dyld no longer needs to run on Leopard kernels that don't have new shared region syscall
static bool newSharedRegionSyscallAvailable()
{
	int shreg_version;
	size_t buffer_size = sizeof(shreg_version);
	if ( sysctlbyname("vm.shared_region_version", &shreg_version, &buffer_size, NULL, 0) == 0 ) {
	   if ( shreg_version == 3 ) 
			return true;
	}
	return false;
}


static int __attribute__((noinline)) _shared_region_check_np(uint64_t* start_address)
{
	if ( (gLinkContext.sharedRegionMode == ImageLoader::kUseSharedRegion) && newSharedRegionSyscallAvailable() ) 
		return syscall(294, start_address);
	return -1;
}


static int __attribute__((noinline)) _shared_region_map_np(int fd, uint32_t count, const shared_file_mapping_np mappings[])
{
	int result;
	if ( (gLinkContext.sharedRegionMode == ImageLoader::kUseSharedRegion) && newSharedRegionSyscallAvailable() ) {
		return syscall(295, fd, count, mappings);
	}

	// remove the shared region sub-map
	vm_deallocate(mach_task_self(), (vm_address_t)SHARED_REGION_BASE, SHARED_REGION_SIZE);
	
	// map cache just for this process with mmap()
	bool failed = false;
	const shared_file_mapping_np* start = mappings;
	const shared_file_mapping_np* end = &mappings[count];
	for (const shared_file_mapping_np* p = start; p < end; ++p ) {
		void* mmapAddress = (void*)(uintptr_t)(p->sfm_address);
		size_t size = p->sfm_size;
		int protection = 0;
		if ( p->sfm_init_prot & VM_PROT_EXECUTE )
			protection   |= PROT_EXEC;
		if ( p->sfm_init_prot & VM_PROT_READ )
			protection   |= PROT_READ;
		if ( p->sfm_init_prot & VM_PROT_WRITE )
			protection   |= PROT_WRITE;
		off_t offset = p->sfm_file_offset;
		mmapAddress = mmap(mmapAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, offset);
		if ( mmap(mmapAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, offset) != mmapAddress )
			failed = true;
	}
	if ( !failed ) {
		result = 0;
		gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
	}
	else {
		result = -1;
		gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		if ( gLinkContext.verboseMapping ) 
			dyld::log("dyld: shared cached cannot be mapped\n");
	}

	return result;
}



#if __ppc__
	#define ARCH_NAME			"ppc"
	#define ARCH_NAME_ROSETTA	"rosetta"
	#define ARCH_VALUE			CPU_TYPE_POWERPC
	#define ARCH_CACHE_MAGIC	"dyld_v1     ppc"
#elif __ppc64__
	#define ARCH_NAME			"ppc64"
	#define ARCH_VALUE			CPU_TYPE_POWERPC64
	#define ARCH_CACHE_MAGIC	"dyld_v1   ppc64"
#elif __i386__
	#define ARCH_NAME			"i386"
	#define ARCH_VALUE			CPU_TYPE_I386
	#define ARCH_CACHE_MAGIC	"dyld_v1    i386"
#elif __x86_64__
	#define ARCH_NAME			"x86_64"
	#define ARCH_VALUE			CPU_TYPE_X86_64
	#define ARCH_CACHE_MAGIC	"dyld_v1  x86_64"
#endif

const void*	imMemorySharedCacheHeader()
{
	return sSharedCache;
}

int openSharedCacheFile()
{
	char path[1024];
	strcpy(path, sSharedCacheDir);
	strcat(path, "/");
#if __ppc__
	// rosetta cannot handle optimized _ppc cache, so it use _rosetta cache instead, rdar://problem/5495438
    if ( isRosetta() )
		strcat(path, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME_ROSETTA);
    else
#endif
		strcat(path, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME);
	return ::open(path, O_RDONLY);
}

static void mapSharedCache()
{
	uint64_t cacheBaseAddress;
	// quick check if a cache is alreay mapped into shared region
	if ( _shared_region_check_np(&cacheBaseAddress) == 0 ) {
		sSharedCache = (dyld_cache_header*)cacheBaseAddress;
		// if we don't understand the currently mapped shared cache, then ignore
		if ( strcmp(sSharedCache->magic, ARCH_CACHE_MAGIC) != 0 ) {
			sSharedCache = NULL;
			if ( gLinkContext.verboseMapping ) 
				dyld::log("dyld: existing shared cached in memory is not compatible\n");
		}
	}
	else {
#if __i386__ || __x86_64__
		// <rdar://problem/5925940> Safe Boot should disable dyld shared cache
		// if we are in safe-boot mode and the cache was not made during this boot cycle,
		// delete the cache file
		uint32_t	safeBootValue = 0;
		size_t		safeBootValueSize = sizeof(safeBootValue);
		if ( (sysctlbyname("kern.safeboot", &safeBootValue, &safeBootValueSize, NULL, 0) == 0) && (safeBootValue != 0) ) {
			// user booted machine in safe-boot mode
			struct stat dyldCacheStatInfo;
			//  Don't use custom DYLD_SHARED_CACHE_DIR if provided, use standard path
			if ( ::stat(DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME, &dyldCacheStatInfo) == 0 ) {
				struct timeval bootTimeValue;
				size_t bootTimeValueSize = sizeof(bootTimeValue);
				if ( (sysctlbyname("kern.boottime", &bootTimeValue, &bootTimeValueSize, NULL, 0) == 0) && (bootTimeValue.tv_sec != 0) ) {
					// if the cache file was created before this boot, then throw it away and let it rebuild itself
					if ( dyldCacheStatInfo.st_mtime < bootTimeValue.tv_sec ) {
						::unlink(DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME);
						gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
						return;
					}
				}
			}
		}
#endif
		// map in shared cache to shared region
		int fd = openSharedCacheFile();
		if ( fd != -1 ) {
			uint8_t firstPages[8192];
			if ( ::read(fd, firstPages, 8192) == 8192 ) {
				dyld_cache_header* header = (dyld_cache_header*)firstPages;
				if ( strcmp(header->magic, ARCH_CACHE_MAGIC) == 0 ) {
					const shared_file_mapping_np* mappings = (shared_file_mapping_np*)&firstPages[header->mappingOffset];
					const shared_file_mapping_np* const end = &mappings[header->mappingCount];
					// validate that the cache file has not been truncated
					bool goodCache = false;
					struct stat stat_buf;
					if ( fstat(fd, &stat_buf) == 0 ) {
						goodCache = true;
						for (const shared_file_mapping_np* p = mappings; p < end; ++p) {
							// rdar://problem/5694507 old update_dyld_shared_cache tool could make a cache file
							// that is not page aligned, but otherwise ok.
							if ( p->sfm_file_offset+p->sfm_size > (uint64_t)(stat_buf.st_size+4095 & (-4096)) ) {
								dyld::log("dyld: shared cached file is corrupt: %s" DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME "\n", sSharedCacheDir);
								goodCache = false;
							}
						}
					}
					// sanity check that /usr/lib/libSystem.B.dylib stat() info matches cache
					if ( header->imagesCount * sizeof(dyld_cache_image_info) + header->imagesOffset < 8192 ) {
						bool foundLibSystem = false;
						if ( stat("/usr/lib/libSystem.B.dylib", &stat_buf) == 0 ) {
							const dyld_cache_image_info* images = (dyld_cache_image_info*)&firstPages[header->imagesOffset];
							const dyld_cache_image_info* const imagesEnd = &images[header->imagesCount];
							for (const dyld_cache_image_info* p = images; p < imagesEnd; ++p) {
 								if ( ((time_t)p->modTime == stat_buf.st_mtime) && ((ino_t)p->inode == stat_buf.st_ino) ) {
									foundLibSystem = true;
									break;
								}
							}					
						}
						if ( !sSharedCacheIgnoreInodeAndTimeStamp && !foundLibSystem ) {
							dyld::log("dyld: shared cached file was build against a different libSystem.dylib, ignoring cache\n");
							goodCache = false;
						}
					}
										
					if ( goodCache ) {
						const shared_file_mapping_np* mappings = (shared_file_mapping_np*)&firstPages[header->mappingOffset];
						if (_shared_region_map_np(fd, header->mappingCount, mappings) == 0) {
							// sucessfully mapped cache into shared region
							sSharedCache = (dyld_cache_header*)mappings[0].sfm_address;
						}
					}
				}
				else {
					if ( gLinkContext.verboseMapping ) 
						dyld::log("dyld: shared cached file is invalid\n");
				}
			}
			else {
				if ( gLinkContext.verboseMapping ) 
					dyld::log("dyld: shared cached file cannot be read\n");
			}
			close(fd);
		}
		else {
			if ( gLinkContext.verboseMapping ) 
				dyld::log("dyld: shared cached file cannot be opened\n");
		}
	}
	
	// remember if dyld loaded at same address as when cache built
	if ( sSharedCache != NULL ) {
		gLinkContext.dyldLoadedAtSameAddressNeededBySharedCache = ((uintptr_t)(sSharedCache->dyldBaseAddress) == (uintptr_t)&_mh_dylinker_header);
	}
	
	// tell gdb where the shared cache is
	if ( sSharedCache != NULL ) {
		const shared_file_mapping_np* const start = (shared_file_mapping_np*)((uint8_t*)sSharedCache + sSharedCache->mappingOffset);
		dyld_shared_cache_ranges.sharedRegionsCount = sSharedCache->mappingCount;
		// only room to tell gdb about first four regions
		if ( dyld_shared_cache_ranges.sharedRegionsCount > 4 )
			dyld_shared_cache_ranges.sharedRegionsCount = 4;
		if ( gLinkContext.verboseMapping ) {
			if ( gLinkContext.sharedRegionMode == ImageLoader::kUseSharedRegion )
				dyld::log("dyld: Mapping shared cache from %s" DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME "\n", sSharedCacheDir);
			else if ( gLinkContext.sharedRegionMode == ImageLoader::kUsePrivateSharedRegion )
				dyld::log("dyld: Mapping private shared cache from %s" DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME "\n", sSharedCacheDir);
		}
		const shared_file_mapping_np* const end = &start[dyld_shared_cache_ranges.sharedRegionsCount];
		int index = 0;
		for (const shared_file_mapping_np* p = start; p < end; ++p, ++index ) {
			dyld_shared_cache_ranges.ranges[index].start = p->sfm_address;
			dyld_shared_cache_ranges.ranges[index].length = p->sfm_size;
			if ( gLinkContext.verboseMapping ) {
				dyld::log("        0x%08llX->0x%08llX %s%s%s init=%x, max=%x\n", p->sfm_address, p->sfm_address+p->sfm_size-1,
					((p->sfm_init_prot & VM_PROT_READ) ? "read " : ""),
					((p->sfm_init_prot & VM_PROT_WRITE) ? "write " : ""),
					((p->sfm_init_prot & VM_PROT_EXECUTE) ? "execute " : ""),  p->sfm_init_prot, p->sfm_max_prot);
			}
		#if __i386__
			// If a non-writable and executable region is found in the R/W shared region, then this is __IMPORT segments
			// This is an old cache.  Make writable.  dyld no longer supports turn W on and off as it binds
			if ( (p->sfm_init_prot == (VM_PROT_READ|VM_PROT_EXECUTE)) && ((p->sfm_address & 0xF0000000) == 0xA0000000) ) {
				if ( p->sfm_size != 0 ) {
					vm_prot_t prot = VM_PROT_EXECUTE | PROT_READ | VM_PROT_WRITE;
					vm_protect(mach_task_self(), p->sfm_address, p->sfm_size, false, prot);
					if ( gLinkContext.verboseMapping ) {
						dyld::log("%18s at 0x%08llX->0x%08llX altered permissions to %c%c%c\n", "", p->sfm_address, 
							p->sfm_address+p->sfm_size-1,
							(prot & PROT_READ) ? 'r' : '.',  (prot & PROT_WRITE) ? 'w' : '.',  (prot & PROT_EXEC) ? 'x' : '.' );
					}
				}
			}
		#endif
		}

	}
}
#endif // #if DYLD_SHARED_CACHE_SUPPORT



// create when NSLinkModule is called for a second time on a bundle
ImageLoader* cloneImage(ImageLoader* image)
{
	// open file (automagically closed when this function exits)
	FileOpener file(image->getPath());
	
	struct stat stat_buf;
	if ( fstat(file.getFileDescriptor(), &stat_buf) == -1)
		throw "stat error";
	
	dyld::LoadContext context;
	context.useSearchPaths		= false;
	context.useFallbackPaths	= false;
	context.useLdLibraryPath	= false;
	context.implicitRPath		= false;
	context.matchByInstallName	= false;
	context.dontLoad			= false;
	context.mustBeBundle		= true;
	context.mustBeDylib			= false;
	context.canBePIE			= false;
	context.origin				= false;
	context.rpath				= false;
	return loadPhase6(file.getFileDescriptor(), stat_buf, image->getPath(), context);
}


ImageLoader* loadFromMemory(const uint8_t* mem, uint64_t len, const char* moduleName)
{
	// if fat wrapper, find usable sub-file
	const fat_header* memStartAsFat = (fat_header*)mem;
	uint64_t fileOffset = 0;
	uint64_t fileLength = len;
	if ( memStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( fatFindBest(memStartAsFat, &fileOffset, &fileLength) ) {
			mem = &mem[fileOffset];
			len = fileLength;
		}
		else {
			throw "no matching architecture in universal wrapper";
		}
	}

	// try each loader
	if ( isCompatibleMachO(mem, moduleName) ) {
		ImageLoader* image = ImageLoaderMachO::instantiateFromMemory(moduleName, (macho_header*)mem, len, gLinkContext);
		// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
		if ( ! image->isBundle() ) 
			addImage(image);
		return image;
	}
	
	// try other file formats here...
	
	// throw error about what was found
	switch (*(uint32_t*)mem) {
		case MH_MAGIC:
		case MH_CIGAM:
		case MH_MAGIC_64:
		case MH_CIGAM_64:
			throw "mach-o, but wrong architecture";
		default:
		throwf("unknown file type, first eight bytes: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
			mem[0], mem[1], mem[2], mem[3], mem[4], mem[5], mem[6],mem[7]);
	}
}


void registerAddCallback(ImageCallback func)
{
	// now add to list to get notified when any more images are added
	sAddImageCallbacks.push_back(func);
	
	// call callback with all existing images
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* image = *it;
		if ( image->getState() >= dyld_image_state_bound && image->getState() < dyld_image_state_terminated )
			(*func)(image->machHeader(), image->getSlide());
	}
}

void registerRemoveCallback(ImageCallback func)
{
	sRemoveImageCallbacks.push_back(func);
}

void clearErrorMessage()
{
	error_string[0] = '\0';
}

void setErrorMessage(const char* message)
{
	// save off error message in global buffer for CrashReporter to find
	strlcpy(error_string, message, sizeof(error_string));
}

const char* getErrorMessage()
{
	return error_string;
}


void halt(const char* message)
{
	dyld::log("dyld: %s\n", message);
	setErrorMessage(message);
	uintptr_t terminationFlags = 0;
	if ( !gLinkContext.startedInitializingMainExecutable )
		terminationFlags = 1;
	setAlImageInfosHalt(error_string, terminationFlags);
	dyld_fatal_error(error_string);
}


uintptr_t bindLazySymbol(const mach_header* mh, uintptr_t* lazyPointer)
{
	uintptr_t result = 0;
	// acquire read-lock on dyld's data structures
#if 0 // rdar://problem/3811777 turn off locking until deadlock is resolved
	if ( gLibSystemHelpers != NULL ) 
		(*gLibSystemHelpers->lockForReading)();
#endif
	// lookup and bind lazy pointer and get target address
	try {
		ImageLoader* target;
	#if __i386__
		// fast stubs pass NULL for mh and image is instead found via the location of stub (aka lazyPointer)
		if ( mh == NULL )
			target = dyld::findImageContainingAddress(lazyPointer);
		else
			target = dyld::findImageByMachHeader(mh);
	#else
		// note, target should always be mach-o, because only mach-o lazy handler wired up to this
		target = dyld::findImageByMachHeader(mh);
	#endif
		if ( target == NULL )
			throwf("image not found for lazy pointer at %p", lazyPointer);
		result = target->doBindLazySymbol(lazyPointer, gLinkContext);
	}
	catch (const char* message) {
		dyld::log("dyld: lazy symbol binding failed: %s\n", message);
		halt(message);
	}
	// release read-lock on dyld's data structures
#if 0
	if ( gLibSystemHelpers != NULL ) 
		(*gLibSystemHelpers->unlockForReading)();
#endif
	// return target address to glue which jumps to it with real parameters restored
	return result;
}


#if COMPRESSED_DYLD_INFO_SUPPORT
uintptr_t fastBindLazySymbol(ImageLoader** imageLoaderCache, uintptr_t lazyBindingInfoOffset)
{
	uintptr_t result = 0;
	// get image 
	if ( *imageLoaderCache == NULL ) {
		// save in cache
		*imageLoaderCache = dyld::findMappedRange((uintptr_t)imageLoaderCache);
		if ( *imageLoaderCache == NULL ) {
			const char* message = "fast lazy binding from unknown image";
			dyld::log("dyld: %s\n", message);
			halt(message);
		}
	}
	
	// bind lazy pointer and return it
	try {
		result = (*imageLoaderCache)->doBindFastLazySymbol(lazyBindingInfoOffset, gLinkContext);
	}
	catch (const char* message) {
		dyld::log("dyld: lazy symbol binding failed: %s\n", message);
		halt(message);
	}

	// return target address to glue which jumps to it with real parameters restored
	return result;
}
#endif // COMPRESSED_DYLD_INFO_SUPPORT



void registerUndefinedHandler(UndefinedHandler handler)
{
	sUndefinedHandler = handler;
}

static void undefinedHandler(const char* symboName)
{
	if ( sUndefinedHandler != NULL ) {
		(*sUndefinedHandler)(symboName);
	}
}

static bool findExportedSymbol(const char* name, bool onlyInCoalesced, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	// search all images in order
	const ImageLoader* firstWeakImage = NULL;
	const ImageLoader::Symbol* firstWeakSym = NULL;
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		// the use of inserted libraries alters search order
		// so that inserted libraries are found before the main executable
		if ( sInsertedDylibCount > 0 ) {
			if ( i < sInsertedDylibCount )
				anImage = sAllImages[i+1];
			else if ( i == sInsertedDylibCount )
				anImage = sAllImages[0];
		}
		if ( ! anImage->hasHiddenExports() && (!onlyInCoalesced || anImage->hasCoalescedExports()) ) {
			*sym = anImage->findExportedSymbol(name, false, image);
			if ( *sym != NULL ) {
				// if weak definition found, record first one found
				if ( ((*image)->getExportedSymbolInfo(*sym) & ImageLoader::kWeakDefinition) != 0 ) {
					if ( firstWeakImage == NULL ) {
						firstWeakImage = *image;
						firstWeakSym = *sym;
					}
				}
				else {
					// found non-weak, so immediately return with it
					return true;
				}
			}
		}
	}
	if ( firstWeakSym != NULL ) {
		// found a weak definition, but no non-weak, so return first weak found
		*sym = firstWeakSym;
		*image = firstWeakImage;
		return true;
	}
	
	return false;
}

bool flatFindExportedSymbol(const char* name, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	return findExportedSymbol(name, false, sym, image);
}

bool findCoalescedExportedSymbol(const char* name, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	return findExportedSymbol(name, true, sym, image);
}


bool flatFindExportedSymbolWithHint(const char* name, const char* librarySubstring, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	// search all images in order
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		// only look at images whose paths contain the hint string (NULL hint string is wildcard)
		if ( ! anImage->isBundle() && ((librarySubstring==NULL) || (strstr(anImage->getPath(), librarySubstring) != NULL)) ) {
			*sym = anImage->findExportedSymbol(name, false, image);
			if ( *sym != NULL ) {
				return true;
			}
		}
	}
	return false;
}

unsigned int getCoalescedImages(ImageLoader* images[])
{
	unsigned int count = 0;
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* image = *it;
		if ( image->participatesInCoalescing() ) {
			*images++ = *it;
			++count;
		}
	}
	return count;
}


static ImageLoader::MappedRegion* getMappedRegions(ImageLoader::MappedRegion* regions)
{
	ImageLoader::MappedRegion* end = regions;
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		(*it)->getMappedRegions(end);
	}
	return end;
}

void registerImageStateSingleChangeHandler(dyld_image_states state, dyld_image_state_change_handler handler)
{
	// mark the image that the handler is in as never-unload because dyld has a reference into it
	ImageLoader* handlerImage = findImageContainingAddress((void*)handler);
	if ( handlerImage != NULL )
		handlerImage->setNeverUnload();

	// add to list of handlers
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sSingleHandlers);
	if ( handlers != NULL ) {
		handlers->push_back(handler);

		// call callback with all existing images
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* image = *it;
			dyld_image_info	 info;
			info.imageLoadAddress	= image->machHeader();
			info.imageFilePath		= image->getPath();
			info.imageFileModDate	= image->lastModified();
			// should only call handler if state == image->state
			if ( image->getState() == state )
				(*handler)(state, 1, &info);
			// ignore returned string, too late to do anything
		}
	}
}

void registerImageStateBatchChangeHandler(dyld_image_states state, dyld_image_state_change_handler handler)
{
	// mark the image that the handler is in as never-unload because dyld has a reference into it
	ImageLoader* handlerImage = findImageContainingAddress((void*)handler);
	if ( handlerImage != NULL )
		handlerImage->setNeverUnload();

	// add to list of handlers
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sBatchHandlers);
	if ( handlers != NULL ) {
		// insert at front, so that gdb handler is always last
		handlers->insert(handlers->begin(), handler);
		
		// call callback with all existing images
		try {
			notifyBatchPartial(state, true, handler);
		}
		catch (const char* msg) {
			// ignore request to abort during registration
		}
	}
}

static ImageLoader* libraryLocator(const char* libraryName, bool search, const char* origin, const ImageLoader::RPathChain* rpaths)
{
	dyld::LoadContext context;
	context.useSearchPaths		= search;
	context.useFallbackPaths	= search;
	context.useLdLibraryPath	= false;
	context.implicitRPath		= false;
	context.matchByInstallName	= false;
	context.dontLoad			= false;
	context.mustBeBundle		= false;
	context.mustBeDylib			= true;
	context.canBePIE			= false;
	context.origin				= origin;
	context.rpath				= rpaths;
	return load(libraryName, context);
}

static const char* basename(const char* path)
{
    const char* last = path;
    for (const char* s = path; *s != '\0'; s++) {
        if (*s == '/') 
			last = s+1;
    }
    return last;
}

static void setContext(const macho_header* mainExecutableMH, int argc, const char* argv[], const char* envp[], const char* apple[])
{
	gLinkContext.loadLibrary			= &libraryLocator;
	gLinkContext.terminationRecorder	= &terminationRecorder;
	gLinkContext.flatExportFinder		= &flatFindExportedSymbol;
	gLinkContext.coalescedExportFinder	= &findCoalescedExportedSymbol;
	gLinkContext.getCoalescedImages		= &getCoalescedImages;
	gLinkContext.undefinedHandler		= &undefinedHandler;
	gLinkContext.getAllMappedRegions	= &getMappedRegions;
	gLinkContext.bindingHandler			= NULL;
	gLinkContext.notifySingle			= &notifySingle;
	gLinkContext.notifyBatch			= &notifyBatch;
	gLinkContext.removeImage			= &removeImage;
	gLinkContext.registerDOFs			= &registerDOFs;
	gLinkContext.clearAllDepths			= &clearAllDepths;
	gLinkContext.imageCount				= &imageCount;
	gLinkContext.setNewProgramVars		= &setNewProgramVars;
#if DYLD_SHARED_CACHE_SUPPORT
	gLinkContext.inSharedCache			= &inSharedCache;
#endif
#if SUPPORT_OLD_CRT_INITIALIZATION
	gLinkContext.setRunInitialzersOldWay= &setRunInitialzersOldWay;
#endif
	gLinkContext.bindingOptions			= ImageLoader::kBindingNone;
	gLinkContext.argc					= argc;
	gLinkContext.argv					= argv;
	gLinkContext.envp					= envp;
	gLinkContext.apple					= apple;
	gLinkContext.progname				= (argv[0] != NULL) ? basename(argv[0]) : "";
	gLinkContext.programVars.mh			= mainExecutableMH;
	gLinkContext.programVars.NXArgcPtr	= &gLinkContext.argc;
	gLinkContext.programVars.NXArgvPtr	= &gLinkContext.argv;
	gLinkContext.programVars.environPtr	= &gLinkContext.envp;
	gLinkContext.programVars.__prognamePtr=&gLinkContext.progname;
	gLinkContext.mainExecutable			= NULL;
	gLinkContext.imageSuffix			= NULL;
	gLinkContext.prebindUsage			= ImageLoader::kUseAllPrebinding;
	gLinkContext.sharedRegionMode		= ImageLoader::kUseSharedRegion;
}

#if __ppc__ || __i386__
bool isRosetta()
{
	int mib[] = { CTL_KERN, KERN_CLASSIC, getpid() };
	int is_classic = 0;
	size_t len = sizeof(int);
	int ret = sysctl(mib, 3, &is_classic, &len, NULL, 0);
	if ((ret != -1) && is_classic) {
		// we're running under Rosetta 
		return true;
	}
	return false;
}
#endif


#if __LP64__
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define macho_segment_command	segment_command_64
	#define macho_section			section_64
#else
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define macho_segment_command	segment_command
	#define macho_section			section
#endif


//
// Look for a special segment in the mach header. 
// Its presences means that the binary wants to have DYLD ignore
// DYLD_ environment variables.
//
static bool hasRestrictedSegment(const macho_header* mh)
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
			{
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				
				//dyld::log("seg name: %s\n", seg->segname);
				if (strcmp(seg->segname, "__RESTRICT") == 0) {
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if (strcmp(sect->sectname, "__restrict") == 0) 
							return true;
					}
				}
			}
			break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
		
	return false;
}
	
											
#if 0
static void printAllImages()
{
	dyld::log("printAllImages()\n");
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* image = *it;
		dyld_image_states imageState = image->getState();
		dyld::log("  state=%d, refcount=%d, name=%s\n", imageState, image->referenceCount(), image->getShortName());
		image->printReferenceCounts();
	}
}
#endif


void link(ImageLoader* image, bool forceLazysBound, const ImageLoader::RPathChain& loaderRPaths)
{
	// add to list of known images.  This did not happen at creation time for bundles
	if ( image->isBundle() && !image->isLinked() )
		addImage(image);

	// we detect root images as those not linked in yet 
	if ( !image->isLinked() )
		addRootImage(image);
	
	// process images
	try {
		image->link(gLinkContext, forceLazysBound, false, loaderRPaths);
	}
	catch (const char* msg) {
		garbageCollectImages();
		throw;
	}
}


void runInitializers(ImageLoader* image)
{
	// do bottom up initialization
	image->runInitializers(gLinkContext);
}

void garbageCollectImages()
{
	// keep scanning list of images until entire list is scanned with no unreferenced images
	bool mightBeUnreferencedImages = true;
	while ( mightBeUnreferencedImages ) {
		mightBeUnreferencedImages = false;
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* image = *it;
			if ( (image->referenceCount() == 0) && !image->neverUnload() && !image->isBeingRemoved() ) {
				try {
					//dyld::log("garbageCollectImages: deleting %s\n", image->getPath());
					image->setBeingRemoved();
					removeImage(image);
					ImageLoader::deleteImage(image);
				}
				catch (const char* msg) {
					dyld::warn("problem deleting image: %s\n", msg);
				}
				mightBeUnreferencedImages = true;
				break;
			}
		}
	}
	//printAllImages();
}


static void preflight_finally(ImageLoader* image)
{
	if ( image->isBundle() ) {
		removeImageFromAllImages(image->machHeader());
		ImageLoader::deleteImage(image);
	}
	sBundleBeingLoaded = NULL;
	dyld::garbageCollectImages();
}


void preflight(ImageLoader* image, const ImageLoader::RPathChain& loaderRPaths)
{
	try {
		if ( image->isBundle() ) 
			sBundleBeingLoaded = image;	// hack
		image->link(gLinkContext, false, true, loaderRPaths);
	}
	catch (const char* msg) {	
		preflight_finally(image);
		throw;
	}
	preflight_finally(image);
}

static void loadInsertedDylib(const char* path)
{
	ImageLoader* image = NULL;
	try {
		LoadContext context;
		context.useSearchPaths		= false;
		context.useFallbackPaths	= false;
		context.useLdLibraryPath	= false;
		context.implicitRPath		= false;
		context.matchByInstallName	= false;
		context.dontLoad			= false;
		context.mustBeBundle		= false;
		context.mustBeDylib			= true;
		context.canBePIE			= false;
		context.origin				= NULL;	// can't use @loader_path with DYLD_INSERT_LIBRARIES
		context.rpath				= NULL;
		image = load(path, context);
		image->setNeverUnload();
	}
	catch (...) {
		halt(dyld::mkstringf("could not load inserted library: %s\n", path));
	}
}

//
// Entry point for dyld.  The kernel loads dyld and jumps to __dyld_start which
// sets up some registers and call this function.
//
// Returns address of main() in target program which __dyld_start jumps to
//
uintptr_t
_main(const macho_header* mainExecutableMH, uintptr_t mainExecutableSlide, int argc, const char* argv[], const char* envp[], const char* apple[])
{	
#ifdef ALTERNATIVE_LOGFILE
	sLogfile = open(ALTERNATIVE_LOGFILE, O_WRONLY | O_CREAT | O_APPEND);
	if ( sLogfile == -1 ) {
		sLogfile = STDERR_FILENO;
		dyld::log("error opening alternate log file %s, errno = %d\n", ALTERNATIVE_LOGFILE, errno);
	}
#endif
	
	setContext(mainExecutableMH, argc, argv, envp, apple);

	// Pickup the pointer to the exec path.
	sExecPath = apple[0];
	bool ignoreEnvironmentVariables = false;
#if __i386__
	if ( isRosetta() ) {
		// under Rosetta (x86 side)
		// When a 32-bit ppc program is run under emulation on an Intel processor,
		// we want any i386 dylibs (e.g. any used by Rosetta) to not load in the shared region
		// because the shared region is being used by ppc dylibs
		gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		ignoreEnvironmentVariables = true;
	}
#endif
	if ( sExecPath[0] != '/' ) {
		// have relative path, use cwd to make absolute
		char cwdbuff[MAXPATHLEN];
	    if ( getcwd(cwdbuff, MAXPATHLEN) != NULL ) {
			// maybe use static buffer to avoid calling malloc so early...
			char* s = new char[strlen(cwdbuff) + strlen(sExecPath) + 2];
			strcpy(s, cwdbuff);
			strcat(s, "/");
			strcat(s, sExecPath);
			sExecPath = s;
		}
	}
	uintptr_t result = 0;
	sMainExecutableMachHeader = mainExecutableMH;
	sProcessIsRestricted = issetugid();
	if ( geteuid() != 0 ) {
		// if we are not root, see if the binary is requesting restricting the use of DYLD_ env vars.
		sProcessIsRestricted |= hasRestrictedSegment(mainExecutableMH);
	}
	if ( sProcessIsRestricted )
		pruneEnvironmentVariables(envp, &apple);
	else
		checkEnvironmentVariables(envp, ignoreEnvironmentVariables);
	if ( sEnv.DYLD_PRINT_OPTS ) 
		printOptions(argv);
	if ( sEnv.DYLD_PRINT_ENV ) 
		printEnvironmentVariables(envp);
	getHostInfo();
	// install gdb notifier
	stateToHandlers(dyld_image_state_dependents_mapped, sBatchHandlers)->push_back(notifyGDB);
	// make initial allocations large enough that it is unlikely to need to be re-alloced
	sAllImages.reserve(INITIAL_IMAGE_COUNT);
	sImageRoots.reserve(16);
	sAddImageCallbacks.reserve(4);
	sRemoveImageCallbacks.reserve(4);
	sImageFilesNeedingTermination.reserve(16);
	sImageFilesNeedingDOFUnregistration.reserve(8);
	
#ifdef WAIT_FOR_SYSTEM_ORDER_HANDSHAKE
	// <rdar://problem/6849505> Add gating mechanism to dyld support system order file generation process
	WAIT_FOR_SYSTEM_ORDER_HANDSHAKE(dyld_all_image_infos.systemOrderFlag);
#endif
	
	try {
		// instantiate ImageLoader for main executable
		sMainExecutable = instantiateFromLoadedImage(mainExecutableMH, mainExecutableSlide, sExecPath);
		sMainExecutable->setNeverUnload();
		gLinkContext.mainExecutable = sMainExecutable;
		gLinkContext.processIsRestricted = sProcessIsRestricted;
		// load shared cache
		checkSharedRegionDisable();
	#if DYLD_SHARED_CACHE_SUPPORT
		if ( gLinkContext.sharedRegionMode != ImageLoader::kDontUseSharedRegion )
			mapSharedCache();
	#endif
		// load any inserted libraries
		if	( sEnv.DYLD_INSERT_LIBRARIES != NULL ) {
			for (const char* const* lib = sEnv.DYLD_INSERT_LIBRARIES; *lib != NULL; ++lib) 
				loadInsertedDylib(*lib);
		}
		// record count of inserted libraries so that a flat search will look at 
		// inserted libraries, then main, then others.
		sInsertedDylibCount = sAllImages.size()-1;

		// link main executable
		gLinkContext.linkingMainExecutable = true;
		link(sMainExecutable, sEnv.DYLD_BIND_AT_LAUNCH, ImageLoader::RPathChain(NULL, NULL));
		gLinkContext.linkingMainExecutable = false;
		if ( sMainExecutable->forceFlat() ) {
			gLinkContext.bindFlat = true;
			gLinkContext.prebindUsage = ImageLoader::kUseNoPrebinding;
		}
		result = (uintptr_t)sMainExecutable->getMain();

		// link any inserted libraries
		// do this after linking main executable so that any dylibs pulled in by inserted 
		// dylibs (e.g. libSystem) will not be in front of dylibs the program uses
		if ( sInsertedDylibCount > 0 ) {
			for(unsigned int i=0; i < sInsertedDylibCount; ++i) {
				ImageLoader* image = sAllImages[i+1];
				link(image, sEnv.DYLD_BIND_AT_LAUNCH, ImageLoader::RPathChain(NULL, NULL));
			}
		}
		
	#if SUPPORT_OLD_CRT_INITIALIZATION
		// Old way is to run initializers via a callback from crt1.o
		if ( ! gRunInitializersOldWay ) 
	#endif
		initializeMainExecutable(); // run all initializers
	}
	catch(const char* message) {
		halt(message);
	}
	catch(...) {
		dyld::log("dyld: launch failed\n");
	}

#ifdef ALTERNATIVE_LOGFILE
	// only use alternate log during launch, otherwise file is open forever
	if ( sLogfile != STDERR_FILENO ) {
		close(sLogfile);
		sLogfile = STDERR_FILENO;
	}
#endif
	
	return result;
}




}; // namespace




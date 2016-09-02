/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2010 Apple Inc. All rights reserved.
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

#define __STDC_LIMIT_MACROS
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach-o/fat.h> 
#include <sys/types.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <libkern/OSAtomic.h>

#include "ImageLoader.h"


uint32_t								ImageLoader::fgImagesUsedFromSharedCache = 0;
uint32_t								ImageLoader::fgImagesWithUsedPrebinding = 0;
uint32_t								ImageLoader::fgImagesRequiringCoalescing = 0;
uint32_t								ImageLoader::fgImagesHasWeakDefinitions = 0;
uint32_t								ImageLoader::fgTotalRebaseFixups = 0;
uint32_t								ImageLoader::fgTotalBindFixups = 0;
uint32_t								ImageLoader::fgTotalBindSymbolsResolved = 0;
uint32_t								ImageLoader::fgTotalBindImageSearches = 0;
uint32_t								ImageLoader::fgTotalLazyBindFixups = 0;
uint32_t								ImageLoader::fgTotalPossibleLazyBindFixups = 0;
uint32_t								ImageLoader::fgTotalSegmentsMapped = 0;
uint64_t								ImageLoader::fgTotalBytesMapped = 0;
uint64_t								ImageLoader::fgTotalBytesPreFetched = 0;
uint64_t								ImageLoader::fgTotalLoadLibrariesTime;
uint64_t								ImageLoader::fgTotalRebaseTime;
uint64_t								ImageLoader::fgTotalBindTime;
uint64_t								ImageLoader::fgTotalWeakBindTime;
uint64_t								ImageLoader::fgTotalDOF;
uint64_t								ImageLoader::fgTotalInitTime;
uint16_t								ImageLoader::fgLoadOrdinal = 0;
std::vector<ImageLoader::InterposeTuple>ImageLoader::fgInterposingTuples;
uintptr_t								ImageLoader::fgNextPIEDylibAddress = 0;



ImageLoader::ImageLoader(const char* path, unsigned int libCount)
	: fPath(path), fRealPath(NULL), fDevice(0), fInode(0), fLastModified(0), 
	fPathHash(0), fDlopenReferenceCount(0), fStaticReferenceCount(0),
	fDynamicReferenceCount(0), fDynamicReferences(NULL), fInitializerRecursiveLock(NULL), 
	fDepth(0), fLoadOrder(fgLoadOrdinal++), fState(0), fLibraryCount(libCount), 
	fAllLibraryChecksumsAndLoadAddressesMatch(false), fLeaveMapped(false), fNeverUnload(false),
	fHideSymbols(false), fMatchByInstallName(false),
	fInterposed(false), fRegisteredDOF(false), fAllLazyPointersBound(false), 
    fBeingRemoved(false), fAddFuncNotified(false),
	fPathOwnedByImage(false), fIsReferencedDownward(false), 
	fIsReferencedUpward(false), fWeakSymbolsBound(false)
{
	if ( fPath != NULL )
		fPathHash = hash(fPath);
}


void ImageLoader::deleteImage(ImageLoader* image)
{
	// this cannot be done in destructor because libImage() is implemented
	// in a subclass
	DependentLibraryInfo libraryInfos[image->libraryCount()]; 
	image->doGetDependentLibraries(libraryInfos);
	for(unsigned int i=0; i < image->libraryCount(); ++i) {
		ImageLoader* lib = image->libImage(i);
		if ( (lib != NULL) && ! libraryInfos[i].upward )
			lib->fStaticReferenceCount--;
	}
	delete image;
}


ImageLoader::~ImageLoader()
{
	if ( fRealPath != NULL ) 
		delete [] fRealPath;
	if ( fPathOwnedByImage && (fPath != NULL) ) 
		delete [] fPath;
	if ( fDynamicReferences != NULL ) {
		for (std::vector<const ImageLoader*>::iterator it = fDynamicReferences->begin(); it != fDynamicReferences->end(); ++it ) {
			const_cast<ImageLoader*>(*it)->fDynamicReferenceCount--;
		}
		delete fDynamicReferences;
	}
}

void ImageLoader::setFileInfo(dev_t device, ino_t inode, time_t modDate)
{
	fDevice = device;
	fInode = inode;
	fLastModified = modDate;
}

void ImageLoader::setMapped(const LinkContext& context)
{
	fState = dyld_image_state_mapped;
	context.notifySingle(dyld_image_state_mapped, this);  // note: can throw exception
}

void ImageLoader::addDynamicReference(const ImageLoader* target)
{
	bool alreadyInVector = false;
	if ( fDynamicReferences == NULL ) {
		fDynamicReferences = new std::vector<const ImageLoader*>();
	}
	else {
		for (std::vector<const ImageLoader*>::iterator it = fDynamicReferences->begin(); it != fDynamicReferences->end(); ++it ) {
			if ( *it == target ) {
				alreadyInVector = true;
				break;
			}
		}
	}
	if ( ! alreadyInVector ) {	
		fDynamicReferences->push_back(target);
		const_cast<ImageLoader*>(target)->fDynamicReferenceCount++;
	}
	//dyld::log("dyld: addDynamicReference() from %s to %s, fDynamicReferences->size()=%lu\n", this->getPath(), target->getPath(), fDynamicReferences->size());
}

int ImageLoader::compare(const ImageLoader* right) const
{
	if ( this->fDepth == right->fDepth ) {
		if ( this->fLoadOrder == right->fLoadOrder )
			return 0;
		else if ( this->fLoadOrder < right->fLoadOrder )
			return -1;
		else
			return 1;
	}
	else {
		if ( this->fDepth < right->fDepth )
			return -1;
		else
			return 1;
	}
}

void ImageLoader::setPath(const char* path)
{
	if ( fPathOwnedByImage && (fPath != NULL) ) 
		delete [] fPath;
	fPath = new char[strlen(path)+1];
	strcpy((char*)fPath, path);
	fPathOwnedByImage = true;  // delete fPath when this image is destructed
	fPathHash = hash(fPath);
	fRealPath = NULL;
}

void ImageLoader::setPathUnowned(const char* path)
{
	if ( fPathOwnedByImage && (fPath != NULL) ) {
		delete [] fPath;
	}
	fPath = path;
	fPathOwnedByImage = false;  
	fPathHash = hash(fPath);
}

void ImageLoader::setPaths(const char* path, const char* realPath)
{
	this->setPath(path);
	fRealPath = new char[strlen(realPath)+1];
	strcpy((char*)fRealPath, realPath);
}

const char* ImageLoader::getRealPath() const 
{ 
	if ( fRealPath != NULL ) 
		return fRealPath;
	else
		return fPath; 
}


uint32_t ImageLoader::hash(const char* path)
{
	// this does not need to be a great hash
	// it is just used to reduce the number of strcmp() calls
	// of existing images when loading a new image
	uint32_t h = 0;
	for (const char* s=path; *s != '\0'; ++s)
		h = h*5 + *s;
	return h;
}

bool ImageLoader::matchInstallPath() const
{
	return fMatchByInstallName;
}

void ImageLoader::setMatchInstallPath(bool match)
{
	fMatchByInstallName = match;
}

bool ImageLoader::statMatch(const struct stat& stat_buf) const
{
	return ( (this->fDevice == stat_buf.st_dev) && (this->fInode == stat_buf.st_ino) );	
}

const char* ImageLoader::getShortName() const
{
	// try to return leaf name
	if ( fPath != NULL ) {
		const char* s = strrchr(fPath, '/');
		if ( s != NULL ) 
			return &s[1];
	}
	return fPath; 
}

void ImageLoader::setLeaveMapped()
{
	fLeaveMapped = true;
}

void ImageLoader::setHideExports(bool hide)
{
	fHideSymbols = hide;
}

bool ImageLoader::hasHiddenExports() const
{
	return fHideSymbols;
}

bool ImageLoader::isLinked() const
{
	return (fState >= dyld_image_state_bound);
}

time_t ImageLoader::lastModified() const
{
	return fLastModified;
}

bool ImageLoader::containsAddress(const void* addr) const
{
	for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
		const uint8_t* start = (const uint8_t*)segActualLoadAddress(i);
		const uint8_t* end = (const uint8_t*)segActualEndAddress(i);
		if ( (start <= addr) && (addr < end) && !segUnaccessible(i) )
			return true;
	}
	return false;
}

bool ImageLoader::overlapsWithAddressRange(const void* start, const void* end) const
{
	for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
		const uint8_t* segStart = (const uint8_t*)segActualLoadAddress(i);
		const uint8_t* segEnd = (const uint8_t*)segActualEndAddress(i);
		if ( strcmp(segName(i), "__UNIXSTACK") == 0 ) {
			// __UNIXSTACK never slides.  This is the only place that cares
			// and checking for that segment name in segActualLoadAddress()
			// is too expensive.
			segStart -= getSlide();
			segEnd -= getSlide();
		}
		if ( (start <= segStart) && (segStart < end) )
			return true;
		if ( (start <= segEnd) && (segEnd < end) )
			return true;
		if ( (segStart < start) && (end < segEnd) )
			return true;
	}
	return false;
}

void ImageLoader::getMappedRegions(MappedRegion*& regions) const
{
	for(unsigned int i=0, e=segmentCount(); i < e; ++i) {
		MappedRegion region;
		region.address = segActualLoadAddress(i);
		region.size = segSize(i);
		*regions++ = region;
	}
}


static bool notInImgageList(const ImageLoader* image, const ImageLoader** dsiStart, const ImageLoader** dsiCur)
{
	for (const ImageLoader** p = dsiStart; p < dsiCur; ++p)
		if ( *p == image )
			return false;
	return true;
}


// private method that handles circular dependencies by only search any image once
const ImageLoader::Symbol* ImageLoader::findExportedSymbolInDependentImagesExcept(const char* name, 
			const ImageLoader** dsiStart, const ImageLoader**& dsiCur, const ImageLoader** dsiEnd, const ImageLoader** foundIn) const
{
	const ImageLoader::Symbol* sym;
	// search self
	if ( notInImgageList(this, dsiStart, dsiCur) ) {
		sym = this->findExportedSymbol(name, false, foundIn);
		if ( sym != NULL )
			return sym;
		*dsiCur++ = this;
	}

	// search directly dependent libraries
	for(unsigned int i=0; i < libraryCount(); ++i) {
		ImageLoader* dependentImage = libImage(i);
		if ( (dependentImage != NULL) && notInImgageList(dependentImage, dsiStart, dsiCur) ) {
			const ImageLoader::Symbol* sym = dependentImage->findExportedSymbol(name, false, foundIn);
			if ( sym != NULL )
				return sym;
		}
	}
	
	// search indirectly dependent libraries
	for(unsigned int i=0; i < libraryCount(); ++i) {
		ImageLoader* dependentImage = libImage(i);
		if ( (dependentImage != NULL) && notInImgageList(dependentImage, dsiStart, dsiCur) ) {
			*dsiCur++ = dependentImage; 
			const ImageLoader::Symbol* sym = dependentImage->findExportedSymbolInDependentImagesExcept(name, dsiStart, dsiCur, dsiEnd, foundIn);
			if ( sym != NULL )
				return sym;
		}
	}

	return NULL;
}


const ImageLoader::Symbol* ImageLoader::findExportedSymbolInDependentImages(const char* name, const LinkContext& context, const ImageLoader** foundIn) const
{
	unsigned int imageCount = context.imageCount();
	const ImageLoader* dontSearchImages[imageCount];
	dontSearchImages[0] = this; // don't search this image
	const ImageLoader** cur = &dontSearchImages[1];
	return this->findExportedSymbolInDependentImagesExcept(name, &dontSearchImages[0], cur, &dontSearchImages[imageCount], foundIn);
}

const ImageLoader::Symbol* ImageLoader::findExportedSymbolInImageOrDependentImages(const char* name, const LinkContext& context, const ImageLoader** foundIn) const
{
	unsigned int imageCount = context.imageCount();
	const ImageLoader* dontSearchImages[imageCount];
	const ImageLoader** cur = &dontSearchImages[0];
	return this->findExportedSymbolInDependentImagesExcept(name, &dontSearchImages[0], cur, &dontSearchImages[imageCount], foundIn);
}

// this is called by initializeMainExecutable() to interpose on the initial set of images
void ImageLoader::applyInterposing(const LinkContext& context)
{
	if ( fgInterposingTuples.size() != 0 )
		this->recursiveApplyInterposing(context);
}

void ImageLoader::link(const LinkContext& context, bool forceLazysBound, bool preflightOnly, const RPathChain& loaderRPaths)
{
	//dyld::log("ImageLoader::link(%s) refCount=%d, neverUnload=%d\n", this->getPath(), fStaticReferenceCount, fNeverUnload);
	
	// clear error strings
	(*context.setErrorStrings)(dyld_error_kind_none, NULL, NULL, NULL);

	uint64_t t0 = mach_absolute_time();
	this->recursiveLoadLibraries(context, preflightOnly, loaderRPaths);
	context.notifyBatch(dyld_image_state_dependents_mapped);
	
	// we only do the loading step for preflights
	if ( preflightOnly )
		return;
		
	uint64_t t1 = mach_absolute_time();
	context.clearAllDepths();
	this->recursiveUpdateDepth(context.imageCount());

	uint64_t t2 = mach_absolute_time();
 	this->recursiveRebase(context);
	context.notifyBatch(dyld_image_state_rebased);
	
	uint64_t t3 = mach_absolute_time();
 	this->recursiveBind(context, forceLazysBound);

	uint64_t t4 = mach_absolute_time();
	this->weakBind(context);
	uint64_t t5 = mach_absolute_time();	

	context.notifyBatch(dyld_image_state_bound);
	uint64_t t6 = mach_absolute_time();	

	std::vector<DOFInfo> dofs;
	this->recursiveGetDOFSections(context, dofs);
	context.registerDOFs(dofs);
	uint64_t t7 = mach_absolute_time();	

	// interpose any dynamically loaded images
	if ( !context.linkingMainExecutable && (fgInterposingTuples.size() != 0) ) {
		this->recursiveApplyInterposing(context);
	}
	
	// clear error strings
	(*context.setErrorStrings)(dyld_error_kind_none, NULL, NULL, NULL);

	fgTotalLoadLibrariesTime += t1 - t0;
	fgTotalRebaseTime += t3 - t2;
	fgTotalBindTime += t4 - t3;
	fgTotalWeakBindTime += t5 - t4;
	fgTotalDOF += t7 - t6;
	
	// done with initial dylib loads
	fgNextPIEDylibAddress = 0;
}


void ImageLoader::printReferenceCounts()
{
	dyld::log("      dlopen=%d, static=%d, dynamic=%d for %s\n", 
				fDlopenReferenceCount, fStaticReferenceCount, fDynamicReferenceCount, getPath() );
}


bool ImageLoader::decrementDlopenReferenceCount() 
{
	if ( fDlopenReferenceCount == 0 )
		return true;
	--fDlopenReferenceCount;
	return false;
}

void ImageLoader::runInitializers(const LinkContext& context, InitializerTimingList& timingInfo)
{
	uint64_t t1 = mach_absolute_time();
	mach_port_t this_thread = mach_thread_self();
	this->recursiveInitialization(context, this_thread, timingInfo);
	context.notifyBatch(dyld_image_state_initialized);
	mach_port_deallocate(mach_task_self(), this_thread);
	uint64_t t2 = mach_absolute_time();
	fgTotalInitTime += (t2 - t1);
}


void ImageLoader::bindAllLazyPointers(const LinkContext& context, bool recursive)
{
	if ( ! fAllLazyPointersBound ) {
		fAllLazyPointersBound = true;

		if ( recursive ) {
			// bind lower level libraries first
			for(unsigned int i=0; i < libraryCount(); ++i) {
				ImageLoader* dependentImage = libImage(i);
				if ( dependentImage != NULL )
					dependentImage->bindAllLazyPointers(context, recursive);
			}
		}
		// bind lazies in this image
		this->doBindJustLazies(context);
	}
}


bool ImageLoader::allDependentLibrariesAsWhenPreBound() const
{
	return fAllLibraryChecksumsAndLoadAddressesMatch;
}


unsigned int ImageLoader::recursiveUpdateDepth(unsigned int maxDepth)
{
	// the purpose of this phase is to make the images sortable such that 
	// in a sort list of images, every image that an image depends on
	// occurs in the list before it.
	if ( fDepth == 0 ) {
		// break cycles
		fDepth = maxDepth;
		
		// get depth of dependents
		unsigned int minDependentDepth = maxDepth;
		for(unsigned int i=0; i < libraryCount(); ++i) {
			ImageLoader* dependentImage = libImage(i);
			if ( (dependentImage != NULL) && !libIsUpward(i) ) {
				unsigned int d = dependentImage->recursiveUpdateDepth(maxDepth);
				if ( d < minDependentDepth )
					minDependentDepth = d;
			}
		}
	
		// make me less deep then all my dependents
		fDepth = minDependentDepth - 1;
	}
	
	return fDepth;
}


void ImageLoader::recursiveLoadLibraries(const LinkContext& context, bool preflightOnly, const RPathChain& loaderRPaths)
{
	if ( fState < dyld_image_state_dependents_mapped ) {
		// break cycles
		fState = dyld_image_state_dependents_mapped;
		
		// get list of libraries this image needs
		//dyld::log("ImageLoader::recursiveLoadLibraries() %ld = %d*%ld\n", fLibrariesCount*sizeof(DependentLibrary), fLibrariesCount, sizeof(DependentLibrary));
		DependentLibraryInfo libraryInfos[fLibraryCount]; 
		this->doGetDependentLibraries(libraryInfos);
		
		// get list of rpaths that this image adds
		std::vector<const char*> rpathsFromThisImage;
		this->getRPaths(context, rpathsFromThisImage);
		const RPathChain thisRPaths(&loaderRPaths, &rpathsFromThisImage);
		
		// try to load each
		bool canUsePrelinkingInfo = true; 
		for(unsigned int i=0; i < fLibraryCount; ++i){
			ImageLoader* dependentLib;
			bool depLibReExported = false;
			bool depLibReRequired = false;
			bool depLibCheckSumsMatch = false;
			DependentLibraryInfo& requiredLibInfo = libraryInfos[i];
#if DYLD_SHARED_CACHE_SUPPORT
			if ( preflightOnly && context.inSharedCache(requiredLibInfo.name) ) {
				// <rdar://problem/5910137> dlopen_preflight() on image in shared cache leaves it loaded but not objc initialized
				// in preflight mode, don't even load dylib that are in the shared cache because they will never be unloaded
				setLibImage(i, NULL, false, false);
				continue;
			}
#endif
			try {
				dependentLib = context.loadLibrary(requiredLibInfo.name, true, this->getPath(), &thisRPaths);
				if ( dependentLib == this ) {
					// found circular reference, perhaps DYLD_LIBARY_PATH is causing this rdar://problem/3684168 
					dependentLib = context.loadLibrary(requiredLibInfo.name, false, NULL, NULL);
					if ( dependentLib != this )
						dyld::warn("DYLD_ setting caused circular dependency in %s\n", this->getPath());
				}
				if ( fNeverUnload )
					dependentLib->setNeverUnload();
				if ( requiredLibInfo.upward ) {
					dependentLib->fIsReferencedUpward = true;
				}
				else { 
					dependentLib->fStaticReferenceCount += 1;
					dependentLib->fIsReferencedDownward = true;
				}
				LibraryInfo actualInfo = dependentLib->doGetLibraryInfo();
				depLibReRequired = requiredLibInfo.required;
				depLibCheckSumsMatch = ( actualInfo.checksum == requiredLibInfo.info.checksum );
				depLibReExported = requiredLibInfo.reExported;
				if ( ! depLibReExported ) {
					// for pre-10.5 binaries that did not use LC_REEXPORT_DYLIB
					depLibReExported = dependentLib->isSubframeworkOf(context, this) || this->hasSubLibrary(context, dependentLib);
				}
				// check found library version is compatible
				// <rdar://problem/89200806> 0xFFFFFFFF is wildcard that matches any version
				if ( (requiredLibInfo.info.minVersion != 0xFFFFFFFF) && (actualInfo.minVersion < requiredLibInfo.info.minVersion) ) {
					// record values for possible use by CrashReporter or Finder
					dyld::throwf("Incompatible library version: %s requires version %d.%d.%d or later, but %s provides version %d.%d.%d",
							this->getShortName(), requiredLibInfo.info.minVersion >> 16, (requiredLibInfo.info.minVersion >> 8) & 0xff, requiredLibInfo.info.minVersion & 0xff,
							dependentLib->getShortName(), actualInfo.minVersion >> 16, (actualInfo.minVersion >> 8) & 0xff, actualInfo.minVersion & 0xff);
				}
				// prebinding for this image disabled if any dependent library changed
				if ( !depLibCheckSumsMatch ) 
					canUsePrelinkingInfo = false;
				// prebinding for this image disabled unless both this and dependent are in the shared cache
				if ( !dependentLib->inSharedCache() || !this->inSharedCache() )
					canUsePrelinkingInfo = false;
					
				//if ( context.verbosePrebinding ) {
				//	if ( !requiredLib.checksumMatches )
				//		fprintf(stderr, "dyld: checksum mismatch, (%u v %u) for %s referencing %s\n", 
				//			requiredLibInfo.info.checksum, actualInfo.checksum, this->getPath(), 	dependentLib->getPath());		
				//	if ( dependentLib->getSlide() != 0 )
				//		fprintf(stderr, "dyld: dependent library slid for %s referencing %s\n", this->getPath(), dependentLib->getPath());		
				//}
			}
			catch (const char* msg) {
				//if ( context.verbosePrebinding )
				//	fprintf(stderr, "dyld: exception during processing for %s referencing %s\n", this->getPath(), dependentLib->getPath());		
				if ( requiredLibInfo.required ) {
					fState = dyld_image_state_mapped;
					// record values for possible use by CrashReporter or Finder
					if ( strstr(msg, "Incompatible") != NULL )
						(*context.setErrorStrings)(dyld_error_kind_dylib_version, this->getPath(), requiredLibInfo.name, NULL);
					else if ( strstr(msg, "architecture") != NULL )
						(*context.setErrorStrings)(dyld_error_kind_dylib_wrong_arch, this->getPath(), requiredLibInfo.name, NULL);
					else
						(*context.setErrorStrings)(dyld_error_kind_dylib_missing, this->getPath(), requiredLibInfo.name, NULL);
					dyld::throwf("Library not loaded: %s\n  Referenced from: %s\n  Reason: %s", requiredLibInfo.name, this->getRealPath(), msg);
				}
				// ok if weak library not found
				dependentLib = NULL;
				canUsePrelinkingInfo = false;  // this disables all prebinding, we may want to just slam import vectors for this lib to zero
			}
			setLibImage(i, dependentLib, depLibReExported, requiredLibInfo.upward);
		}
		fAllLibraryChecksumsAndLoadAddressesMatch = canUsePrelinkingInfo;

		// tell each to load its dependents
		for(unsigned int i=0; i < libraryCount(); ++i) {
			ImageLoader* dependentImage = libImage(i);
			if ( dependentImage != NULL ) {	
				dependentImage->recursiveLoadLibraries(context, preflightOnly, thisRPaths);
			}
		}
		
		// do deep prebind check
		if ( fAllLibraryChecksumsAndLoadAddressesMatch ) {
			for(unsigned int i=0; i < libraryCount(); ++i){
				ImageLoader* dependentImage = libImage(i);
				if ( dependentImage != NULL ) {	
					if ( !dependentImage->allDependentLibrariesAsWhenPreBound() )
						fAllLibraryChecksumsAndLoadAddressesMatch = false;
				}
			}
		}
		
		// free rpaths (getRPaths() malloc'ed each string)
		for(std::vector<const char*>::iterator it=rpathsFromThisImage.begin(); it != rpathsFromThisImage.end(); ++it) {
			const char* str = *it;
			free((void*)str);
		}
		
	}
}

void ImageLoader::recursiveRebase(const LinkContext& context)
{ 
	if ( fState < dyld_image_state_rebased ) {
		// break cycles
		fState = dyld_image_state_rebased;
		
		try {
			// rebase lower level libraries first
			for(unsigned int i=0; i < libraryCount(); ++i) {
				ImageLoader* dependentImage = libImage(i);
				if ( dependentImage != NULL )
					dependentImage->recursiveRebase(context);
			}
				
			// rebase this image
			doRebase(context);
			
			// notify
			context.notifySingle(dyld_image_state_rebased, this);
		}
		catch (const char* msg) {
			// this image is not rebased
			fState = dyld_image_state_dependents_mapped;
            CRSetCrashLogMessage2(NULL);
			throw;
		}
	}
}

void ImageLoader::recursiveApplyInterposing(const LinkContext& context)
{ 
	if ( ! fInterposed ) {
		// break cycles
		fInterposed = true;
		
		try {
			// interpose lower level libraries first
			for(unsigned int i=0; i < libraryCount(); ++i) {
				ImageLoader* dependentImage = libImage(i);
				if ( dependentImage != NULL )
					dependentImage->recursiveApplyInterposing(context);
			}
				
			// interpose this image
			doInterpose(context);
		}
		catch (const char* msg) {
			// this image is not interposed
			fInterposed = false;
			throw;
		}
	}
}



void ImageLoader::recursiveBind(const LinkContext& context, bool forceLazysBound)
{
	// Normally just non-lazy pointers are bound immediately.
	// The exceptions are:
	//   1) DYLD_BIND_AT_LAUNCH will cause lazy pointers to be bound immediately
	//   2) some API's (e.g. RTLD_NOW) can cause lazy pointers to be bound immediately
	if ( fState < dyld_image_state_bound ) {
		// break cycles
		fState = dyld_image_state_bound;
	
		try {
			// bind lower level libraries first
			for(unsigned int i=0; i < libraryCount(); ++i) {
				ImageLoader* dependentImage = libImage(i);
				if ( dependentImage != NULL )
					dependentImage->recursiveBind(context, forceLazysBound);
			}
			// bind this image
			this->doBind(context, forceLazysBound);	
			// mark if lazys are also bound
			if ( forceLazysBound || this->usablePrebinding(context) )
				fAllLazyPointersBound = true;
				
			context.notifySingle(dyld_image_state_bound, this);
		}
		catch (const char* msg) {
			// restore state
			fState = dyld_image_state_rebased;
            CRSetCrashLogMessage2(NULL);
			throw;
		}
	}
}

void ImageLoader::weakBind(const LinkContext& context)
{
	if ( context.verboseWeakBind )
		dyld::log("dyld: weak bind start:\n");
	// get set of ImageLoaders that participate in coalecsing
	ImageLoader* imagesNeedingCoalescing[fgImagesRequiringCoalescing];
	int count = context.getCoalescedImages(imagesNeedingCoalescing);
	
	// count how many have not already had weakbinding done
	int countNotYetWeakBound = 0;
	int countOfImagesWithWeakDefinitions = 0;
	int countOfImagesWithWeakDefinitionsNotInSharedCache = 0;
	for(int i=0; i < count; ++i) {
		if ( ! imagesNeedingCoalescing[i]->fWeakSymbolsBound )
			++countNotYetWeakBound;
		if ( imagesNeedingCoalescing[i]->hasCoalescedExports() ) {
			++countOfImagesWithWeakDefinitions;
			if ( ! imagesNeedingCoalescing[i]->inSharedCache() ) 
				++countOfImagesWithWeakDefinitionsNotInSharedCache;
		}
	}

	// don't need to do any coalescing if only one image has overrides, or all have already been done
	if ( (countOfImagesWithWeakDefinitionsNotInSharedCache > 0) && (countNotYetWeakBound > 0) ) {
		// make symbol iterators for each
		ImageLoader::CoalIterator iterators[count];
		ImageLoader::CoalIterator* sortedIts[count];
		for(int i=0; i < count; ++i) {
			imagesNeedingCoalescing[i]->initializeCoalIterator(iterators[i], i);
			sortedIts[i] = &iterators[i];
			if ( context.verboseWeakBind )
				dyld::log("dyld: weak bind load order %d/%d for %s\n", i, count, imagesNeedingCoalescing[i]->getPath());
		}
		
		// walk all symbols keeping iterators in sync by 
		// only ever incrementing the iterator with the lowest symbol 
		int doneCount = 0;
		while ( doneCount != count ) {
			//for(int i=0; i < count; ++i)
			//	dyld::log("sym[%d]=%s ", sortedIts[i]->loadOrder, sortedIts[i]->symbolName);
			//dyld::log("\n");
			// increment iterator with lowest symbol
			if ( sortedIts[0]->image->incrementCoalIterator(*sortedIts[0]) )
				++doneCount; 
			// re-sort iterators
			for(int i=1; i < count; ++i) {
				int result = strcmp(sortedIts[i-1]->symbolName, sortedIts[i]->symbolName);
				if ( result == 0 )
					sortedIts[i-1]->symbolMatches = true;
				if ( result > 0 ) {
					// new one is bigger then next, so swap
					ImageLoader::CoalIterator* temp = sortedIts[i-1];
					sortedIts[i-1] = sortedIts[i];
					sortedIts[i] = temp;
				}
				if ( result < 0 )
					break;
			}
			// process all matching symbols just before incrementing the lowest one that matches
			if ( sortedIts[0]->symbolMatches && !sortedIts[0]->done ) {
				const char* nameToCoalesce = sortedIts[0]->symbolName;
				// pick first symbol in load order (and non-weak overrides weak)
				uintptr_t targetAddr = 0;
				ImageLoader* targetImage = NULL;
				for(int i=0; i < count; ++i) {
					if ( strcmp(iterators[i].symbolName, nameToCoalesce) == 0 ) {
						if ( context.verboseWeakBind )
							dyld::log("dyld: weak bind, found %s weak=%d in %s \n", nameToCoalesce, iterators[i].weakSymbol, iterators[i].image->getPath());
						if ( iterators[i].weakSymbol ) {
							if ( targetAddr == 0 ) {
								targetAddr = iterators[i].image->getAddressCoalIterator(iterators[i], context);
								if ( targetAddr != 0 )
									targetImage = iterators[i].image;
							}
						}
						else {
							targetAddr = iterators[i].image->getAddressCoalIterator(iterators[i], context);
							if ( targetAddr != 0 ) {
								targetImage = iterators[i].image;
								// strong implementation found, stop searching
								break;
							}
						}
					}
				}
				if ( context.verboseWeakBind )
					dyld::log("dyld: weak binding all uses of %s to copy from %s\n", nameToCoalesce, targetImage->getShortName());
				
				// tell each to bind to this symbol (unless already bound)
				if ( targetAddr != 0 ) {
					for(int i=0; i < count; ++i) {
						if ( strcmp(iterators[i].symbolName, nameToCoalesce) == 0 ) {
							if ( context.verboseWeakBind )
								dyld::log("dyld: weak bind, setting all uses of %s in %s to 0x%lX from %s\n", nameToCoalesce, iterators[i].image->getShortName(), targetAddr, targetImage->getShortName());
							if ( ! iterators[i].image->fWeakSymbolsBound )
								iterators[i].image->updateUsesCoalIterator(iterators[i], targetAddr, targetImage, context);
							iterators[i].symbolMatches = false; 
						}
					}
				}
				
			}
		}
		
		// mark all as having all weak symbols bound
		for(int i=0; i < count; ++i) {
			imagesNeedingCoalescing[i]->fWeakSymbolsBound = true;
		}
	}
	if ( context.verboseWeakBind )
		dyld::log("dyld: weak bind end\n");
}



void ImageLoader::recursiveGetDOFSections(const LinkContext& context, std::vector<DOFInfo>& dofs)
{
	if ( ! fRegisteredDOF ) {
		// break cycles
		fRegisteredDOF = true;
		
		// gather lower level libraries first
		for(unsigned int i=0; i < libraryCount(); ++i) {
			ImageLoader* dependentImage = libImage(i);
			if ( dependentImage != NULL )
				dependentImage->recursiveGetDOFSections(context, dofs);
		}
		this->doGetDOFSections(context, dofs);
	}
}


void ImageLoader::recursiveSpinLock(recursive_lock& rlock)
{
	// try to set image's ivar fInitializerRecursiveLock to point to this lock_info
	// keep trying until success (spin)
	while ( ! OSAtomicCompareAndSwapPtrBarrier(NULL, &rlock, (void**)&fInitializerRecursiveLock) ) {
		// if fInitializerRecursiveLock already points to a different lock_info, if it is for
		// the same thread we are on, the increment the lock count, otherwise continue to spin
		if ( (fInitializerRecursiveLock != NULL) && (fInitializerRecursiveLock->thread == rlock.thread) )
			break;
	}
	++(fInitializerRecursiveLock->count); 
}

void ImageLoader::recursiveSpinUnLock()
{
	if ( --(fInitializerRecursiveLock->count) == 0 )
		fInitializerRecursiveLock = NULL;
}


void ImageLoader::recursiveInitialization(const LinkContext& context, mach_port_t this_thread, InitializerTimingList& timingInfo)
{
	recursive_lock lock_info(this_thread);
	recursiveSpinLock(lock_info);

	if ( fState < dyld_image_state_dependents_initialized-1 ) {
		uint8_t oldState = fState;
		// break cycles
		fState = dyld_image_state_dependents_initialized-1;
		try {
			bool hasUpwards = false;
			// initialize lower level libraries first
			for(unsigned int i=0; i < libraryCount(); ++i) {
				ImageLoader* dependentImage = libImage(i);
				if ( dependentImage != NULL ) {
					// don't try to initialize stuff "above" me
					bool isUpward = libIsUpward(i);
					if ( (dependentImage->fDepth >= fDepth) && !isUpward ) {
						dependentImage->recursiveInitialization(context, this_thread, timingInfo);
					}
					hasUpwards |= isUpward;
                }
			}
			
			// record termination order
			if ( this->needsTermination() )
				context.terminationRecorder(this);
			
			// let objc know we are about to initialize this image
			uint64_t t1 = mach_absolute_time();
			fState = dyld_image_state_dependents_initialized;
			oldState = fState;
			context.notifySingle(dyld_image_state_dependents_initialized, this);
			
			// initialize this image
			bool hasInitializers = this->doInitialization(context);
			
			// <rdar://problem/10491874> initialize any upward depedencies
			if ( hasUpwards ) {
				for(unsigned int i=0; i < libraryCount(); ++i) {
					ImageLoader* dependentImage = libImage(i);
					// <rdar://problem/10643239> ObjC CG hang
					// only init upward lib here if lib is not downwardly referenced somewhere 
					if ( (dependentImage != NULL) && libIsUpward(i) && !dependentImage->isReferencedDownward() ) {
						dependentImage->recursiveInitialization(context, this_thread, timingInfo);
					}
				}
			}
            
			// let anyone know we finished initializing this image
			fState = dyld_image_state_initialized;
			oldState = fState;
			context.notifySingle(dyld_image_state_initialized, this);
			
			if ( hasInitializers ) {
				uint64_t t2 = mach_absolute_time();
				timingInfo.images[timingInfo.count].image = this;
				timingInfo.images[timingInfo.count].initTime = (t2-t1);
				timingInfo.count++;
			}
		}
		catch (const char* msg) {
			// this image is not initialized
			fState = oldState;
			recursiveSpinUnLock();
			throw;
		}
	}
	
	recursiveSpinUnLock();
}


static void printTime(const char* msg, uint64_t partTime, uint64_t totalTime)
{
	static uint64_t sUnitsPerSecond = 0;
	if ( sUnitsPerSecond == 0 ) {
		struct mach_timebase_info timeBaseInfo;
		if ( mach_timebase_info(&timeBaseInfo) == KERN_SUCCESS ) {
			sUnitsPerSecond = 1000000000ULL * timeBaseInfo.denom / timeBaseInfo.numer;
		}
	}
	if ( partTime < sUnitsPerSecond ) {
		uint32_t milliSecondsTimesHundred = (partTime*100000)/sUnitsPerSecond;
		uint32_t milliSeconds = milliSecondsTimesHundred/100;
		uint32_t percentTimesTen = (partTime*1000)/totalTime;
		uint32_t percent = percentTimesTen/10;
		dyld::log("%s: %u.%02u milliseconds (%u.%u%%)\n", msg, milliSeconds, milliSecondsTimesHundred-milliSeconds*100, percent, percentTimesTen-percent*10);
	}
	else {
		uint32_t secondsTimeTen = (partTime*10)/sUnitsPerSecond;
		uint32_t seconds = secondsTimeTen/10;
		uint32_t percentTimesTen = (partTime*1000)/totalTime;
		uint32_t percent = percentTimesTen/10;
		dyld::log("%s: %u.%u seconds (%u.%u%%)\n", msg, seconds, secondsTimeTen-seconds*10, percent, percentTimesTen-percent*10);
	}
}

static char* commatize(uint64_t in, char* out)
{
	uint64_t div10 = in / 10;
	uint8_t delta = in - div10*10;
	char* s = &out[32];
	int digitCount = 1;
	*s = '\0';
	*(--s) = '0' + delta;
	in = div10;
	while ( in != 0 ) {
		if ( (digitCount % 3) == 0 )
			*(--s) = ',';
		div10 = in / 10;
		delta = in - div10*10;
		*(--s) = '0' + delta;
		in = div10;
		++digitCount;
	}
	return s;
}


void ImageLoader::printStatistics(unsigned int imageCount, const InitializerTimingList& timingInfo)
{
	uint64_t totalTime = fgTotalLoadLibrariesTime + fgTotalRebaseTime + fgTotalBindTime + fgTotalWeakBindTime + fgTotalDOF + fgTotalInitTime;
	char commaNum1[40];
	char commaNum2[40];

	printTime("total time", totalTime, totalTime);
#if __IPHONE_OS_VERSION_MIN_REQUIRED	
	if ( fgImagesUsedFromSharedCache != 0 )
		dyld::log("total images loaded:  %d (%u from dyld shared cache)\n", imageCount, fgImagesUsedFromSharedCache);
	else
		dyld::log("total images loaded:  %d\n", imageCount);
#else
	dyld::log("total images loaded:  %d (%u from dyld shared cache)\n", imageCount, fgImagesUsedFromSharedCache);
#endif
	dyld::log("total segments mapped: %u, into %llu pages with %llu pages pre-fetched\n", fgTotalSegmentsMapped, fgTotalBytesMapped/4096, fgTotalBytesPreFetched/4096);
	printTime("total images loading time", fgTotalLoadLibrariesTime, totalTime);
	printTime("total dtrace DOF registration time", fgTotalDOF, totalTime);
	dyld::log("total rebase fixups:  %s\n", commatize(fgTotalRebaseFixups, commaNum1));
	printTime("total rebase fixups time", fgTotalRebaseTime, totalTime);
	dyld::log("total binding fixups: %s\n", commatize(fgTotalBindFixups, commaNum1));
	if ( fgTotalBindSymbolsResolved != 0 ) {
		uint32_t avgTimesTen = (fgTotalBindImageSearches * 10) / fgTotalBindSymbolsResolved;
		uint32_t avgInt = fgTotalBindImageSearches / fgTotalBindSymbolsResolved;
		uint32_t avgTenths = avgTimesTen - (avgInt*10);
		dyld::log("total binding symbol lookups: %s, average images searched per symbol: %u.%u\n", 
				commatize(fgTotalBindSymbolsResolved, commaNum1), avgInt, avgTenths);
	}
	printTime("total binding fixups time", fgTotalBindTime, totalTime);
	printTime("total weak binding fixups time", fgTotalWeakBindTime, totalTime);
	dyld::log("total bindings lazily fixed up: %s of %s\n", commatize(fgTotalLazyBindFixups, commaNum1), commatize(fgTotalPossibleLazyBindFixups, commaNum2));
	printTime("total initializer time", fgTotalInitTime, totalTime);
	for (uintptr_t i=0; i < timingInfo.count; ++i) {
		dyld::log("%21s ", timingInfo.images[i].image->getShortName());
		printTime("", timingInfo.images[i].initTime, totalTime);
	}
	
}


//
// copy path and add suffix to result
//
//  /path/foo.dylib		_debug   =>   /path/foo_debug.dylib	
//  foo.dylib			_debug   =>   foo_debug.dylib
//  foo     			_debug   =>   foo_debug
//  /path/bar			_debug   =>   /path/bar_debug  
//  /path/bar.A.dylib   _debug   =>   /path/bar.A_debug.dylib
//
void ImageLoader::addSuffix(const char* path, const char* suffix, char* result)
{
	strcpy(result, path);
	
	char* start = strrchr(result, '/');
	if ( start != NULL )
		start++;
	else
		start = result;
		
	char* dot = strrchr(start, '.');
	if ( dot != NULL ) {
		strcpy(dot, suffix);
		strcat(&dot[strlen(suffix)], &path[dot-result]);
	}
	else {
		strcat(result, suffix);
	}
}





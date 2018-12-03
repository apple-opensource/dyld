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

#ifndef ClosureFileSystemPhysical_h
#define ClosureFileSystemPhysical_h

#include "ClosureFileSystem.h"

namespace dyld3 {
namespace closure {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
class __attribute__((visibility("hidden"))) FileSystemPhysical : public FileSystem {
public:
    FileSystemPhysical(const char* fileSystemPrefix = nullptr) : FileSystem(), _fileSystemPrefix(fileSystemPrefix) { }

    bool getRealPath(const char possiblePath[MAXPATHLEN], char realPath[MAXPATHLEN]) const override;

    bool loadFile(const char* path, LoadedFileInfo& info, char realerPath[MAXPATHLEN], void (^error)(const char* format, ...)) const override;

    void unloadFile(const LoadedFileInfo& info) const override;

    void unloadPartialFile(LoadedFileInfo& info, uint64_t keepStartOffset, uint64_t keepLength) const override;

    bool fileExists(const char* path, uint64_t* inode=nullptr, uint64_t* mtime=nullptr, bool* issetuid=nullptr) const override;

private:
    const char* _fileSystemPrefix;
};
#pragma clang diagnostic pop

} //  namespace closure
} //  namespace dyld3

#endif /* ClosureFileSystemPhysical_h */

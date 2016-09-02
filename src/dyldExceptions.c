/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
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
#include <stdio.h>
#include <mach-o/loader.h>

//
// BEGIN copy of code from libgcc.a source file unwind-dw2-fde-darwin.c
//
#define KEYMGR_API_MAJOR_GCC3           3       
/* ... with these keys.  */
#define KEYMGR_GCC3_LIVE_IMAGE_LIST	301     /* loaded images  */
#define KEYMGR_GCC3_DW2_OBJ_LIST	302     /* Dwarf2 object list  */   
#define KEYMGR_EH_GLOBALS_KEY           13 

/* Node of KEYMGR_GCC3_LIVE_IMAGE_LIST.  Info about each resident image.  */
struct live_images {
  unsigned long this_size;                      /* sizeof (live_images)  */
  struct mach_header *mh;                       /* the image info  */
  unsigned long vm_slide;
  void (*destructor)(struct live_images *);     /* destructor for this  */
  struct live_images *next;
  unsigned int examined_p;
  void *fde;
  void *object_info;
  unsigned long info[2];                        /* Future use.  */
};
//
// END copy of code from libgcc.a source file unwind-dw2-fde-darwin.c
//


//
// dyld is built as stand alone executable with a static copy of libc, libstdc++, and libgcc.
//
// In order for C++ exceptions to work within dyld, the C++ exception handling code
// must be able to find the exception handling frame data inside dyld.  The standard
// exception handling code uses crt and keymgr to keep track of all images and calls
// getsectdatafromheader("__eh_frame") to find the EH data for each image. We implement
// our own copy of those functions below to enable exceptions within dyld.
//
// Note: This exception handling is completely separate from any user code exception .
//		 handling which has its own keymgr (in libSystem).
// 


static struct live_images   sDyldImage;  // zero filled
static void*				sObjectList = NULL;
#if defined(__GNUC__) && (((__GNUC__ == 3) && (__GNUC_MINOR__ >= 5)) || (__GNUC__ >= 4))
static void*				sEHGlobals = NULL;
#endif


// called by dyldStartup.s very early
void dyld_exceptions_init(struct mach_header *mh, uintptr_t slide)
{
	sDyldImage.this_size = sizeof(struct live_images);
	sDyldImage.mh = mh;
	sDyldImage.vm_slide = slide;
}



//  Hack for gcc 3.5's use of keymgr includes accessing __keymgr_global
#if defined(__GNUC__) && (((__GNUC__ == 3) && (__GNUC_MINOR__ >= 5)) || (__GNUC__ >= 4))
typedef struct Sinfo_Node {
  uint32_t size; 		/* Size of this node.  */
  uint16_t major_version; /* API major version.  */
  uint16_t minor_version;	/* API minor version.  */
} Tinfo_Node;
static const Tinfo_Node keymgr_info = { sizeof (Tinfo_Node), KEYMGR_API_MAJOR_GCC3, 0 };
const Tinfo_Node* __keymgr_global[3] = { NULL, NULL, &keymgr_info };
#endif

static __attribute__((noreturn)) 
void dyld_abort() 
{
	fprintf(stderr, "internal dyld error\n");
	abort();
}

void* _keymgr_get_and_lock_processwide_ptr(unsigned int key)
{
	// The C++ exception handling code uses two keys. No other keys should be seen
	if ( key == KEYMGR_GCC3_LIVE_IMAGE_LIST ) {
		return &sDyldImage;
	}
	else if ( key == KEYMGR_GCC3_DW2_OBJ_LIST ) {
		return sObjectList;
	}
	dyld_abort();
}

void _keymgr_set_and_unlock_processwide_ptr(unsigned int key, void* value)
{
	// The C++ exception handling code uses just this key. No other keys should be seen
	if ( key == KEYMGR_GCC3_DW2_OBJ_LIST ) {
		sObjectList = value;
		return;
	}
	dyld_abort();
}

void _keymgr_unlock_processwide_ptr(unsigned int key)
{
	// The C++ exception handling code uses just this key. No other keys should be seen
	if ( key == KEYMGR_GCC3_LIVE_IMAGE_LIST ) {
		return;
	}
	dyld_abort();
}
	
void* _keymgr_get_per_thread_data(unsigned int key)
{
#if defined(__GNUC__) && (((__GNUC__ == 3) && (__GNUC_MINOR__ >= 5)) || (__GNUC__ >= 4))
	// gcc 3.5 and later use this key
	if ( key == KEYMGR_EH_GLOBALS_KEY )
		return sEHGlobals;
#endif

	// used by std::termination which dyld does not use
	dyld_abort();
}

void _keymgr_set_per_thread_data(unsigned int key, void *keydata)
{
#if defined(__GNUC__) && (((__GNUC__ == 3) && (__GNUC_MINOR__ >= 5)) || (__GNUC__ >= 4))
	// gcc 3.5 and later use this key
	if ( key == KEYMGR_EH_GLOBALS_KEY ) {
		sEHGlobals = keydata;
		return;
	}
#endif
	// used by std::termination which dyld does not use
	dyld_abort();
}

#if __LP64__
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define macho_header			mach_header_64
	#define macho_segment_command	segment_command_64
	#define macho_section			section_64
	#define getsectdatafromheader	getsectdatafromheader_64
#else
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define macho_header			mach_header
	#define macho_segment_command	segment_command
	#define macho_section			section
#endif

// needed by C++ exception handling code to find __eh_frame section
const void* getsectdatafromheader(struct mach_header* mh, const char* segname, const char* sectname, unsigned long* size)
{
	const struct load_command* cmd;
	unsigned long i;
        
	cmd = (struct load_command*) ((char *)mh + sizeof(struct macho_header));
	for(i = 0; i < mh->ncmds; i++) {
	    if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
			if ( strcmp(seg->segname, segname) == 0 ) {
				const struct macho_section* sect = (struct macho_section*)( (char*)seg + sizeof(struct macho_segment_command) );
				unsigned long j;
				for (j = 0; j < seg->nsects; j++) {
					if ( strcmp(sect[j].sectname, sectname) == 0 ) {
						*size = sect[j].size;
						return (void*)(sect[j].addr);
					}
				}
		    }
		}
	    cmd = (struct load_command*)( (char*)cmd + cmd->cmdsize );
	}
	return NULL;
}


// Hack for transition of rdar://problem/3933738
// Can be removed later.
// Allow C++ runtime to call getsectdatafromheader or getsectdatafromheader_64
#if __LP64__
	#undef getsectdatafromheader
	const void* getsectdatafromheader(struct mach_header* mh, const char* segname, const char* sectname, unsigned long* size)
	{
		return getsectdatafromheader_64(mh, segname, sectname, size);
	}
#endif



/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
#include <stdio.h>  // fprintf(), NULL
#include <stdlib.h> // exit(), EXIT_SUCCESS
#include <string.h> // strcmp(), strncmp()

#include "test.h" // PASS(), FAIL(), XPASS(), XFAIL()

//
// binaries set to run as some other user id never use DYLD_INSERT_LIBRARIES
// That environment variable is cleared by dyld (its right-hand-side is set to empty)
//

int main(int argc, const char *argv[])
{
	const char* rhs = getenv("DYLD_INSERT_LIBRARIES");
	if ( rhs == NULL )
        FAIL("insert-libraries-with-suid DYLD_INSERT_LIBRARIES not set");
	else if ( rhs[0] != '\0' )
        FAIL("insert-libraries-with-suid DYLD_INSERT_LIBRARIES not cleared");
	else
		PASS("insert-libraries-with-suid");
	return EXIT_SUCCESS;
}

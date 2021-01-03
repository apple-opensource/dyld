// BUILD(macos):  $CC foo.c -dynamiclib -o $BUILD_DIR/lc/libfoo.dylib -install_name /blah/libfoo.dylib
// BUILD(macos):  $CC main.c $BUILD_DIR/lc/libfoo.dylib -o $BUILD_DIR/restrict-search-lc-find.exe    -Wl,-dyld_env,DYLD_LIBRARY_PATH=@loader_path/lc -DMODE=lc-find -DSHOULD_BE_FOUND=1
// BUILD(macos):  $CC main.c $BUILD_DIR/lc/libfoo.dylib -o $BUILD_DIR/restrict-search-lc-no-find.exe -Wl,-dyld_env,DYLD_LIBRARY_PATH=@loader_path/lc -DMODE=lc-no-find -sectcreate __RESTRICT __restrict /dev/null
// BUILD(macos):  $CC foo.c -dynamiclib -o $BUILD_DIR/rpath/libfoo.dylib -install_name @rpath/libfoo.dylib
// BUILD(macos):  $CC main.c $BUILD_DIR/rpath/libfoo.dylib -o $BUILD_DIR/restrict-search-rpath-find.exe    -Wl,-rpath,@loader_path/rpath/ -DMODE=rpath-find -DSHOULD_BE_FOUND=1
// BUILD(macos):  $CC main.c $BUILD_DIR/rpath/libfoo.dylib -o $BUILD_DIR/restrict-search-rpath-no-find.exe -Wl,-rpath,@loader_path/rpath/ -DMODE=rpath-no-find -sectcreate __RESTRICT __restrict /dev/null

// BUILD(ios,tvos,watchos,bridgeos):

// RUN:  ./restrict-search-lc-find.exe
// RUN:  ./restrict-search-lc-no-find.exe
// RUN:  ./restrict-search-rpath-find.exe
// RUN:  ./restrict-search-rpath-no-find.exe



#include <stdio.h>
#include <dlfcn.h>

#include "test_support.h"

// Two ways to find libfoo.dylb: @rpath or DYLD_LIBRARY_PATH (set via LC_DYLD_ENVIRONMENT)
// These should work for non-restrictured programs.
// These should fail for restricted programs.
// By weak linking we can test if libfoo.dylib was found or not.


extern int foo() __attribute__((weak_import));


#define STRINGIFY2( x) #x
#define STRINGIFY(x) STRINGIFY2(x)


int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
#if SHOULD_BE_FOUND
    if ( &foo == NULL )
        FAIL("Incorrectly found %s", STRINGIFY(MODE));
    else
        PASS("Incorrectly did not find %s", STRINGIFY(MODE));
#else
    // dylib won't be found at runtime, so &foo should be NULL
    if ( &foo == NULL )
        PASS("Found %s", STRINGIFY(MODE));
    else
        FAIL("Could not find %s", STRINGIFY(MODE));
#endif

	return 0;
}


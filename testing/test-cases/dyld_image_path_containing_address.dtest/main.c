
// BUILD:  $CC main.c            -o $BUILD_DIR/dyld_image_path_containing_address-test.exe

// RUN:  ./dyld_image_path_containing_address-test.exe


#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>

#include "test_support.h"

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    int count = _dyld_image_count();
    for (int i=0; i < count; ++i) {
        const struct mach_header* mh    = _dyld_get_image_header(i);
        const char*               name1 = _dyld_get_image_name(i);
        const char*               name2 = dyld_image_path_containing_address(mh);
        if ( strcmp(name1, name2) != 0 ) {
            FAIL("%s != %s", name1, name2);
        }
    }

    PASS("Success");
}


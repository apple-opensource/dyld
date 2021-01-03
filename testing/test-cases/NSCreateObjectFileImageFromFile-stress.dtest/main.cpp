// BUILD(macos):  $CXX main.cpp  -o $BUILD_DIR/NSCreateObjectFileImageFromFile-stress.exe -Wno-deprecated-declarations
// BUILD(macos):  $CC  foo.c     -o $BUILD_DIR/foo.bundle -bundle

// BUILD(ios,tvos,watchos,bridgeos):

// RUN:  ./NSCreateObjectFileImageFromFile-stress.exe $RUN_DIR/foo.bundle


#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <vector>

#include "test_support.h"

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    const char* path = argv[1];

    std::vector<NSObjectFileImage> ofis;
    for (unsigned i = 0; i != 32; ++i) {
        NSObjectFileImage ofi;
        if ( NSCreateObjectFileImageFromFile(path, &ofi) != NSObjectFileImageSuccess ) {
            FAIL("NSCreateObjectFileImageFromFile failed");
        }
        ofis.push_back(ofi);
    }

    for(unsigned i = 0; i != 32; ++i) {
    NSObjectFileImage ofi = ofis[i];
    NSModule mod = NSLinkModule(ofi, path, NSLINKMODULE_OPTION_NONE);
        if ( mod == NULL ) {
            FAIL("NSLinkModule failed");
        }

        NSSymbol sym = NSLookupSymbolInModule(mod, "_fooInBundle");
        if ( sym == NULL ) {
            FAIL("NSLookupSymbolInModule failed");
        }

        void* func = NSAddressOfSymbol(sym);
        if ( func == NULL ) {
            FAIL("NSAddressOfSymbol failed");
        }

        Dl_info info;
        if ( dladdr(func, &info) == 0 ) {
            FAIL("dladdr(&p, xx) fail");
        }
        LOG("_fooInBundle found in %s", info.dli_fname);

        if ( !NSUnLinkModule(mod, NSUNLINKMODULE_OPTION_NONE) ) {
            FAIL("NSUnLinkModule failed");
        }

        if ( dladdr(func, &info) != 0 ) {
            FAIL("dladdr(&p, xx) found but should not have");
        }
    }

    for (unsigned i = 0; i != 32; ++i) {
        NSObjectFileImage ofi = ofis[i];
        if ( !NSDestroyObjectFileImage(ofi) ) {
            FAIL("NSDestroyObjectFileImage failed");
        }
    }

    PASS("Success");
}


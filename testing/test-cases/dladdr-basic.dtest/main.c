
// BUILD:  $CC main.c            -o $BUILD_DIR/dladdr-basic.exe

// RUN:  ./dladdr-basic.exe

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <dlfcn.h> 
#include <mach-o/dyld_priv.h>


int bar()
{
    return 2;
}

static int foo()
{
    return 3;
}

__attribute__((visibility("hidden"))) int hide()
{
    return 4;
}

// checks global symbol
static void verifybar()
{
    Dl_info info;
    if ( dladdr(&bar, &info) == 0 ) {
        printf("[FAIL] dladdr(&bar, xx) failed");
        exit(0);
    }
    if ( strcmp(info.dli_sname, "bar") != 0 ) {
        printf("[FAIL] dladdr()->dli_sname is \"%s\" instead of \"bar\"", info.dli_sname);
        exit(0);
    }
    if ( info.dli_saddr != &bar) {
        printf("[FAIL] dladdr()->dli_saddr is not &bar");
        exit(0);
    }
    if ( info.dli_fbase != dyld_image_header_containing_address(&bar) ) {
        printf("[FAIL] dladdr()->dli_fbase is not image that contains &bar");
        exit(0);
    }
}

// checks local symbol
static void verifyfoo()
{
    Dl_info info;
    if ( dladdr(&foo, &info) == 0 ) {
        printf("[FAIL] dladdr(&foo, xx) failed");
        exit(0);
    }
    if ( strcmp(info.dli_sname, "foo") != 0 ) {
        printf("[FAIL] dladdr()->dli_sname is \"%s\" instead of \"foo\"", info.dli_sname);
        exit(0);
    }
    if ( info.dli_saddr != &foo) {
        printf("[FAIL] dladdr()->dli_saddr is not &foo");
        exit(0);
    }
    if ( info.dli_fbase != dyld_image_header_containing_address(&foo) ) {
        printf("[FAIL] dladdr()->dli_fbase is not image that contains &foo");
        exit(0);
    }
}

// checks hidden symbol
static void verifyhide()
{
    Dl_info info;
    if ( dladdr(&hide, &info) == 0 ) {
        printf("[FAIL] dladdr(&hide, xx) failed");
        exit(0);
    }
    if ( strcmp(info.dli_sname, "hide") != 0 ) {
        printf("[FAIL] dladdr()->dli_sname is \"%s\" instead of \"hide\"", info.dli_sname);
        exit(0);
    }
    if ( info.dli_saddr != &hide) {
        printf("[FAIL] dladdr()->dli_saddr is not &hide");
        exit(0);
    }
    if ( info.dli_fbase != dyld_image_header_containing_address(&hide) ) {
        printf("[FAIL] dladdr()->dli_fbase is not image that contains &hide");
        exit(0);
    }
}

// checks dylib symbol
static void verifymalloc()
{
    Dl_info info;
    if ( dladdr(&malloc, &info) == 0 ) {
        printf("[FAIL] dladdr(&malloc, xx) failed");
        exit(0);
    }
    if ( strcmp(info.dli_sname, "malloc") != 0 ) {
        printf("[FAIL] dladdr()->dli_sname is \"%s\" instead of \"malloc\"", info.dli_sname);
        exit(0);
    }
    if ( info.dli_saddr != &malloc) {
        printf("[FAIL] dladdr()->dli_saddr is not &malloc");
        exit(0);
    }
    if ( info.dli_fbase != dyld_image_header_containing_address(&malloc) ) {
        printf("[FAIL] dladdr()->dli_fbase is not image that contains &malloc");
        exit(0);
    }
}


int main()
{
    printf("[BEGIN] dladdr-basic\n");
    verifybar();
    verifyhide();
    verifyfoo();
    verifymalloc();


    printf("[PASS] dladdr-basic\n");
    return 0;
}




#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <dlfcn.h> 
#include <mach-o/dyld_priv.h>
#if __has_feature(ptrauth_calls)
    #include <ptrauth.h>
#endif

#include "test_support.h"

extern void* __dso_handle;


static const void* stripPointer(const void* ptr)
{
#if __has_feature(ptrauth_calls)
    return __builtin_ptrauth_strip(ptr, ptrauth_key_asia);
#else
    return ptr;
#endif
}


int dylib_bar()
{
    return 2;
}

static int dylib_foo()
{
    return 3;
}

__attribute__((visibility("hidden"))) int dylib_hide()
{
    return 4;
}

// checks global symbol
static void verifybar()
{
    Dl_info info;
    if ( dladdr(&dylib_bar, &info) == 0 ) {
        FAIL("dladdr(&dylib_bar, xx) failed");
    }
    if ( strcmp(info.dli_sname, "dylib_bar") != 0 ) {
        FAIL("dladdr()->dli_sname is \"%s\" instead of \"dylib_bar\"", info.dli_sname);
    }
    if ( info.dli_saddr != stripPointer(&dylib_bar) ) {
        FAIL("dladdr()->dli_saddr is not &dylib_bar");
    }
    if ( info.dli_fbase != &__dso_handle ) {
        FAIL("dladdr()->dli_fbase is not image that contains &dylib_bar");
    }
}

// checks local symbol
static void verifyfoo()
{
    Dl_info info;
    if ( dladdr(&dylib_foo, &info) == 0 ) {
        FAIL("dladdr(&dylib_foo, xx) failed");
    }
    if ( strcmp(info.dli_sname, "dylib_foo") != 0 ) {
        FAIL("dladdr()->dli_sname is \"%s\" instead of \"dylib_foo\"", info.dli_sname);
    }
    if ( info.dli_saddr != stripPointer(&dylib_foo) ) {
        FAIL("dladdr()->dli_saddr is not &dylib_foo");
    }
    if ( info.dli_fbase != &__dso_handle ) {
        FAIL("dladdr()->dli_fbase is not image that contains &dylib_foo");
    }
}

// checks hidden symbol
static void verifyhide()
{ 
    Dl_info info;
    if ( dladdr(&dylib_hide, &info) == 0 ) {
        FAIL("dladdr(&dylib_hide, xx) failed");
    }
    if ( strcmp(info.dli_sname, "dylib_hide") != 0 ) {
        FAIL("dladdr()->dli_sname is \"%s\" instead of \"dylib_hide\"", info.dli_sname);
    }
    if ( info.dli_saddr != stripPointer(&dylib_hide) ) {
        FAIL("dladdr()->dli_saddr is not &dylib_hide");
    }
    if ( info.dli_fbase != &__dso_handle ) {
        FAIL("dladdr()->dli_fbase is not image that contains &dylib_hide");
    }
}

// checks DSO handle
static void verifyDSOHandle()
{
    Dl_info info;
    if ( dladdr(&__dso_handle, &info) == 0 ) {
        FAIL("dladdr(&__dso_handle, xx) failed");
    }
    if ( strcmp(info.dli_sname, "__dso_handle") != 0 ) {
        FAIL("dladdr()->dli_sname is \"%s\" instead of \"__dso_handle\"", info.dli_sname);
    }
    if ( info.dli_saddr != stripPointer(&__dso_handle) ) {
        FAIL("dladdr()->dli_saddr is not &__dso_handle");
    }
    if ( info.dli_fbase != &__dso_handle ) {
        FAIL("dladdr()->dli_fbase is not image that contains &__dso_handle");
    }
}


void verifyDylib()
{
    verifybar();
    verifyfoo();
    verifyhide();
    verifyDSOHandle();
}


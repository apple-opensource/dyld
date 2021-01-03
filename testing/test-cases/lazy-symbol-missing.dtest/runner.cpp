
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/machine.h>
#include <err.h>
#include <System/sys/reason.h>
#include <System/sys/proc_info.h>
#include <System/kern/kern_cdata.h>
#include <libproc.h>
#include <mach-o/dyld_priv.h>

#if __arm64e__
    // arm64e uses chained binds which does not support lazy binding
    #define SUPPORTS_LAZY_BINDING 0
#else
    #define SUPPORTS_LAZY_BINDING 1
#endif

#include "test_support.h"

static const char* getUserDescription(struct proc_exitreasoninfo& info) {
    kcdata_iter_t iter =  kcdata_iter((void*)info.eri_kcd_buf, info.eri_reason_buf_size);
    if ( !kcdata_iter_valid(iter) )
        return NULL;
    if ( kcdata_iter_type(iter) != KCDATA_BUFFER_BEGIN_OS_REASON )
        return NULL;
    iter = kcdata_iter_find_type(iter, EXIT_REASON_USER_DESC);
    if ( !kcdata_iter_valid(iter) ) {
        return NULL;
    }
    return kcdata_iter_string(iter, 0);
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
#if SUPPORTS_LAZY_BINDING
    _process process;
#ifdef WEAK
    process.set_executable_path(RUN_DIR "/lazy-symbol-missing-called-weak-lib.exe");
#else
    process.set_executable_path(RUN_DIR "/lazy-symbol-missing-called.exe");
#endif
    const char* env[] = { "TEST_OUTPUT=None", NULL};
    process.set_env(env);
    process.set_exit_handler(^(pid_t pid) {
        LOG("Child exited pid=%d", pid);

        struct proc_exitreasoninfo info;
        bzero(&info, sizeof(info));
        uint8_t packReasonData[OS_REASON_BUFFER_MAX_SIZE];
        bzero(packReasonData, OS_REASON_BUFFER_MAX_SIZE);
        info.eri_reason_buf_size = OS_REASON_BUFFER_MAX_SIZE;
        info.eri_kcd_buf = (user_addr_t)packReasonData;
        LOG("info=%p", &info);
        int procResult = proc_pidinfo(pid, PROC_PIDEXITREASONINFO, 1, &info, PROC_PIDEXITREASONINFO_SIZE);
        if ( procResult != sizeof(struct proc_exitreasoninfo) ) {
            FAIL("bad return size from proc_pidinfo(), %d expected %lu", procResult, PROC_PIDEXITREASONINFO_SIZE);
        }
        if ( info.eri_namespace != OS_REASON_DYLD ) {
            FAIL("eri_namespace (%d) != OS_REASON_DYLD", info.eri_namespace);
        }
        const char* userDesc = getUserDescription(info);
        if ( userDesc != NULL ) {
            LOG("user desc=%s", (const char*)userDesc);
        }
#ifdef WEAK
        // Make sure we print a dylib name, not a dependency library # when referencing a symbol from a missing library
        if ( userDesc != NULL ) {
            if ( strstr(userDesc, "libbar-missing.dylib") == NULL ) {
                FAIL("Expected name of missing dylib");
            }
        }
#endif
        PASS("Success");
    });
    (void)process.launch();
#endif
    PASS("Success");
}


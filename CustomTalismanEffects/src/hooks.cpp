#include "hooks.hpp"

#include "log.hpp"
#include "overlay_coexist.hpp"

#include <MinHook.h>

namespace cte::hooks {

namespace {

// Long enough to outlast a legitimate batch (MH_ApplyQueued suspends and
// resumes every thread in the process, which on a loaded game can take tens of
// milliseconds), short enough that a wedged peer can't hang our bootstrap.
constexpr DWORD kInstallWaitMs = 5000;

} // namespace

InstallLock::InstallLock() {
    // A named mutex is recursive per owning thread, so nesting an InstallLock
    // inside another on the same thread is safe (each ctor acquires, each dtor
    // releases once).
    HANDLE h = CreateMutexW(nullptr, FALSE,
                            coexist::kHookInstallMutexName);
    if (!h) {
        flog("[hooks] [WARN] hook-install mutex unavailable (err %lu); "
             "installing unserialized -- a second mod installing right now "
             "could clobber these hooks", GetLastError());
        return;
    }
    handle_ = h;
    const DWORD w = WaitForSingleObject(h, kInstallWaitMs);
    if (w == WAIT_OBJECT_0) {
        held_ = true;
    } else if (w == WAIT_ABANDONED) {
        // Previous owner died mid-batch. We own it now; the target's bytes may
        // be in whatever state that mod left them, but proceeding is still
        // strictly better than not serializing at all.
        held_ = true;
        flog("[hooks] [WARN] hook-install mutex was abandoned by another mod "
             "(it died mid-install); continuing");
    } else {
        flog("[hooks] [WARN] hook-install mutex wait returned %lu; installing "
             "unserialized -- concurrent installs by another mod may clobber "
             "these hooks", w);
    }
}

InstallLock::~InstallLock() {
    if (handle_) {
        if (held_) ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

bool init() {
    const MH_STATUS s = MH_Initialize();
    return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
}

bool create(void* target, void* detour, void** original) {
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK) {
        flog("[hooks] [ERROR] MH_CreateHook(%p -> %p) failed: %s (%d)",
             target, detour, MH_StatusToString(created),
             static_cast<int>(created));
        return false;
    }
    const MH_STATUS queued = MH_QueueEnableHook(target);
    if (queued != MH_OK) {
        flog("[hooks] [ERROR] MH_QueueEnableHook(%p) failed: %s (%d)",
             target, MH_StatusToString(queued), static_cast<int>(queued));
        return false;
    }
    return true;
}

bool apply() {
    const MH_STATUS status = MH_ApplyQueued();
    if (status == MH_OK) return true;
    flog("[hooks] [ERROR] MH_ApplyQueued failed: %s (%d)",
         MH_StatusToString(status), static_cast<int>(status));
    return false;
}

void deinit() {
    MH_Uninitialize();
}

} // namespace cte::hooks

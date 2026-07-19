#include "hooks.hpp"

#include "log.hpp"

#include <MinHook.h>

namespace cte::hooks {

namespace {

// Backend-wide, session-local. Every mod embedding this backend contends for
// this exact name -- see the InstallLock comment in hooks.hpp before changing
// it (the short answer is: don't).
constexpr const wchar_t* kInstallMutexName = L"Local\\QPBackend.HookInstall.v1";

// Long enough to outlast a legitimate batch (MH_ApplyQueued suspends and
// resumes every thread in the process, which on a loaded game can take tens of
// milliseconds), short enough that a wedged peer can't hang our bootstrap.
constexpr DWORD kInstallWaitMs = 5000;

} // namespace

InstallLock::InstallLock() {
    // A named mutex is recursive per owning thread, so nesting an InstallLock
    // inside another on the same thread is safe (each ctor acquires, each dtor
    // releases once).
    HANDLE h = CreateMutexW(nullptr, FALSE, kInstallMutexName);
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
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    return MH_QueueEnableHook(target) == MH_OK;
}

bool apply() {
    return MH_ApplyQueued() == MH_OK;
}

void deinit() {
    MH_Uninitialize();
}

} // namespace cte::hooks

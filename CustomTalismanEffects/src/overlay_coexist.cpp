#include "overlay_coexist.hpp"

#include "log.hpp"

namespace cte::coexist {

namespace {

HANDLE g_menu_owner = nullptr;
bool   g_owned = false;

} // namespace

bool acquire_menu_ownership() {
    if (g_owned) return true; // recursive open edge; nothing to do
    if (!g_menu_owner) {
        g_menu_owner = CreateMutexW(nullptr, FALSE, kMenuOwnerMutexName);
        if (!g_menu_owner) {
            // Without the handle we cannot arbitrate. Fail OPEN: a single-mod
            // install (the overwhelmingly common case) must never lose its
            // menu because a mutex handle could not be created.
            flog("[overlay] [WARN] menu-owner mutex unavailable (err %lu); "
                 "opening without arbitration", GetLastError());
            g_owned = true;
            return true;
        }
    }
    const DWORD w = WaitForSingleObject(g_menu_owner, 0);
    if (w == WAIT_OBJECT_0) {
        g_owned = true;
        return true;
    }
    if (w == WAIT_ABANDONED) {
        // The previous owner's thread died with the menu open. Its raw-input
        // restore never ran, so the game may already be input-starved; taking
        // ownership and running OUR restore on close is the recovery.
        g_owned = true;
        flog("[overlay] [WARN] menu ownership was abandoned by another overlay; "
             "taking it over");
        return true;
    }
    return false; // WAIT_TIMEOUT: another overlay in this process is open
}

void release_menu_ownership() {
    if (!g_owned) return;
    g_owned = false;
    if (g_menu_owner) ReleaseMutex(g_menu_owner);
}

bool holds_menu_ownership() {
    return g_owned;
}

} // namespace cte::coexist

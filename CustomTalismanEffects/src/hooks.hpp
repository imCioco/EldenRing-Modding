#pragma once

// Tiny MinHook wrapper. Same queue-then-apply model MapForGoblins' `modutils`
// uses: create() queues a hook, apply() commits all queued hooks at once.

namespace cte::hooks {

// ── cross-instance install serialization ──────────────────────────────────
// MinHook is statically compiled INTO this DLL. When two mods that both embed
// this backend are loaded into the same game process (CTE + QuestPath), there
// are two MinHook instances that share no lock -- and MinHook splits hooking
// into two steps:
//
//   MH_CreateHook     builds the trampoline from the target's CURRENT bytes
//   MH_ApplyQueued    writes the 5-byte jmp over the target's prologue
//
// Sequential installs chain correctly (the later instance's trampoline
// contains the earlier instance's jmp, so both detours run). Interleaved ones
// do not:
//
//   A: MH_CreateHook(target)   -- snapshots the ORIGINAL prologue
//   B: MH_CreateHook(target)   -- A hasn't patched yet, snapshots it too
//   A: MH_ApplyQueued          -- writes jmp -> A_detour
//   B: MH_ApplyQueued          -- writes jmp -> B_detour built from the
//                                 ORIGINAL bytes, erasing A's jmp entirely
//
// Every call returns MH_OK and nothing crashes; A's detour is simply
// unreachable forever. Observed 2026-07-19 with CTE + QuestPath: both logged
// "hooks installed" in the same millisecond and only one ever saw a swapchain
// creation.
//
// InstallLock closes the window with a process-local NAMED mutex, so it
// serializes across DLL instances and not merely across our own threads. Hold
// it across a whole create->apply batch: with installs serialized the second
// instance's MH_CreateHook reads the first instance's already-patched bytes
// and the chain composes correctly. MH_ApplyQueued also suspends every thread
// in the process, so the lock additionally keeps two instances from freezing
// each other mid-patch.
//
// This mod's DXGI creation interception no longer depends on the lock -- it
// uses factory vtable slots, a different layer entirely (overlay_present.cpp,
// install_factory_slot). The lock still matters for every other batch: the
// dinput8, XInput and cursor-unpin detours are ordinary code patches, and a
// clobber there leaks pad input to the game or re-pins the cursor while a menu
// is open. It also protects the code-patching FALLBACK path in
// install_all_hooks.
//
// The mutex name is deliberately backend-wide (NOT per-mod) -- every mod
// embedding this backend must contend for the same one or the lock is
// pointless. Do not rename it; see overlay_coexist.hpp.
//
// Acquisition is bounded: a mod that dies holding the mutex must not wedge
// ours. An abandoned mutex is acquired and logged (Win32 transfers ownership);
// a timeout proceeds unlocked and logged, which is exactly the previous
// unserialized behavior -- never worse.
class InstallLock {
  public:
    InstallLock();
    ~InstallLock();
    InstallLock(const InstallLock&) = delete;
    InstallLock& operator=(const InstallLock&) = delete;

    // True when the mutex was actually acquired. Callers may proceed either
    // way; this exists for diagnostics.
    bool held() const { return held_; }

  private:
    void* handle_ = nullptr; // HANDLE (kept opaque to avoid <windows.h> here)
    bool  held_ = false;
};

// MH_Initialize. Safe to call once; returns false on failure.
bool init();

// MH_CreateHook + MH_QueueEnableHook. `original` receives the trampoline.
// Call under an InstallLock, in the same scope as the matching apply().
bool create(void* target, void* detour, void** original);

// MH_ApplyQueued -- enable every queued hook. Call under the SAME InstallLock
// scope that guarded the create() calls it commits.
bool apply();

// MH_Uninitialize (best-effort; called on shutdown paths, usually never).
void deinit();

} // namespace cte::hooks

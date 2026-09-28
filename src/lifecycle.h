// Single-instance ownership and ordered unload.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "swaplogic.h"

namespace tf {

// A process-wide mutex prevents duplicate loads, including renamed DLLs.
// Pinned instances retain it until the process exits.
inline std::string soleInstanceName(unsigned long pid) { return "Local\\truefont-sole-instance-" + std::to_string(pid); }
// Report from Initialize: a refused instance never receives a frame callback.
inline constexpr const char* kAlreadyLoadedText = "TrueFont is already loaded (perhaps under another file name); /unload it first, or restart the game if no copy is listed.";
class SoleInstance {
public:
    SoleInstance() = default;
    SoleInstance(const SoleInstance&) = delete;
    SoleInstance& operator=(const SoleInstance&) = delete;
    // Refuse existing ownership, including this object after a pinned DLL is reloaded.
    bool acquire(const std::string& name) {
        if (h_) return false;
        const HANDLE h = CreateMutexA(nullptr, TRUE, name.c_str());
        const DWORD e = GetLastError();
        if (!h || e == ERROR_ALREADY_EXISTS) {
            if (h) CloseHandle(h);
            return false;
        }
        h_ = h;
        return true;
    }
    // Release last, and only if the DLL can unload.
    bool release(bool pinned) {
        if (!h_ || pinned) return false;
        ReleaseMutex(h_);   // fails harmlessly off Initialize's thread: the name goes with the last handle
        CloseHandle(h_);
        h_ = nullptr;
        return true;
    }
    bool held() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

struct ReleaseInstallers {
    OffReport rep;                                  // both installers' reports, merged
    bool workerStopped = true, spriteStopped = true;   // their workers stopped (a running one pins the DLL)
};
// Platform operations used by runRelease.
class ReleaseHost {
public:
    virtual void markReleasing() = 0;             // the frame callback does nothing more
    virtual void abortFontScans() = 0;            // a fonts scan waiting for a build gives up (the writer stops sooner)
    virtual bool stopDiskWriter() = 0;            // a queued save is written (2 s at most); false: it runs on
    virtual unsigned long renderThread() const = 0;   // 0: no frame was seen
    virtual unsigned long thisThread() const = 0;
    virtual bool waitFrameLeft() = 0;             // off the render thread: the frame callback leaves (1 s at most)
    virtual bool inFrame() const = 0;             // TrueFont's frame callback is running
    virtual void releaseDev() = 0;                // the DEV build's self-check (nothing in a release build)
    virtual void releaseShape(ReleaseMode mode, bool frameLeft, bool mayWriteCode) = 0;   // Aspect and Size (shape.h)
    virtual bool hasInstallers() const = 0;
    virtual ReleaseInstallers releaseInstallers(ReleaseMode mode) = 0;
    virtual void deleteInstallers(bool chat, bool sprites) = 0;   // each only when its worker stopped
    virtual void unloadFolderFonts() = 0;         // the fonts folder's fonts (logs what it did)
    virtual void say(const std::string& line, bool chat) = 0;   // Unload warning: chat when safe, otherwise log.
    virtual void pin() = 0;                       // the DLL stays mapped until the game closes
    virtual bool pinned() const = 0;
    virtual bool stopLog() = 0;                   // "unloaded" logged and the writer stopped; false: it runs on
    virtual void endLog() = 0;                    // nothing more is logged (after the log's own pin, whose line is logged)
    virtual void releaseSoleInstance() = 0;       // SoleInstance::release(false)
    virtual unsigned callerAddress() const = 0;   // Release's return address, for the log
    virtual void info(const std::string& s) = 0;
    virtual void warn(const std::string& s) = 0;
    virtual void err(const std::string& s) = 0;

protected:
    ~ReleaseHost() = default;
};
struct ReleaseRun {
    ReleaseMode mode = ReleaseMode::Nothing;
    bool onRender = false, frameLeft = true, insideFrame = false, diskStopped = true, mustPin = false, logStopped = true, soleReleased = false;
    std::string line;   // Unload warning; empty when none.
};
inline std::string releaseThreadText(unsigned long here, unsigned long render, bool insideFrame, ReleaseMode mode, unsigned caller) {
    char b[256];
    _snprintf_s(b, sizeof b, _TRUNCATE, "Release on thread %lu (render %lu); inside the frame callback %d; %s; return address %08X", here, render, insideFrame ? 1 : 0,
                releaseModeText(mode), caller);
    return b;
}
inline constexpr const char* kFrameStuckLine =
    "unloaded while a frame was running: nothing could be put back, so the game's font stays replaced until the game closes. Restart the game before loading TrueFont again.";

// Stop producers before restoring game state and releasing resources.
// Retain the DLL and instance mutex if any callback, worker or installed resource may survive.
inline ReleaseRun runRelease(ReleaseHost& h, bool refused) {
    ReleaseRun r;
    if (refused) return r;
    h.markReleasing();
    const unsigned long here = h.thisThread();
    h.abortFontScans();
    r.diskStopped = h.stopDiskWriter();
    // Re-read after each wait: the first frame can establish the render thread during either wait.
    const unsigned long renderAtStop = h.renderThread();
    r.onRender = renderAtStop && here == renderAtStop;
    // Wait even if no render thread was recorded; a first callback may already be entering.
    if (!r.onRender) r.frameLeft = h.waitFrameLeft();
    const unsigned long render = h.renderThread();
    h.releaseDev();   // after the wait: no frame callback is running it now
    r.insideFrame = r.onRender && h.inFrame();
    r.mode = decideReleaseMode(false, render, here, r.insideFrame);
    h.info(releaseThreadText(here, render, r.insideFrame, r.mode, h.callerAddress()));
    h.releaseShape(r.mode, r.frameLeft, r.onRender || !render);   // code is written from the game thread only
    r.mustPin = !r.diskStopped;   // DataOnly pins only when something was installed or a thread runs on
    if (!r.diskStopped) h.err("Release: the settings writer did not stop within 2 s; the DLL is pinned");
    if (!r.frameLeft) {
        h.err("Release: the render thread stayed inside TrueFont's frame callback for 1 s; NOTHING was touched; the DLL is pinned");
        r.mustPin = true;
        r.line = kFrameStuckLine;
    } else if (h.hasInstallers()) {
        const ReleaseInstallers ri = h.releaseInstallers(r.mode);
        if (!ri.workerStopped) { r.mustPin = true; h.err("Release: the atlas worker did not stop; the DLL is pinned and the installer is kept"); }
        if (!ri.spriteStopped) { r.mustPin = true; h.err("Release: the sprite-font worker did not stop; the DLL is pinned and its installer is kept"); }
        if (!ri.rep.clean()) r.mustPin = true;
        if (r.mode == ReleaseMode::DataOnly && ri.rep.wasOn) r.mustPin = true;
        r.line = releaseLine(ri.rep, r.mode, r.onRender, r.mustPin);
        if (ri.rep.gameGone) h.info("Release: the game is closing: nothing was written; what TrueFont still held stays and goes with the game (no leftover to report)");
        // A reentrant Release must retain installers whose methods are still on this stack.
        if (r.insideFrame) {
            r.mustPin = true;
            h.info("Release: inside the frame callback: the installers are kept (their methods may be on this stack) and the DLL is pinned");
        } else h.deleteInstallers(ri.workerStopped, ri.spriteStopped);
    }
    // Folder fonts can unload after their users stop. Retained fonts alone do not require pinning the DLL.
    if (!r.diskStopped) h.warn("Release: the fonts folder's fonts stay loaded (the disk writer did not stop); they go when the game closes");
    else h.unloadFolderFonts();
    if (!r.line.empty()) h.say(r.line, r.onRender || !render);
    if (r.mustPin) h.pin();
    r.logStopped = h.stopLog();
    if (!r.logStopped) h.pin();   // the writer thread outlives unload: keep its code mapped
    h.endLog();                   // after that pin, so its line is logged
    if (!h.pinned()) {
        h.releaseSoleInstance();
        r.soleReleased = true;
    }
    return r;
}

}  // namespace tf

// Exclude private-font replacement while builds hold selected HFONTs; removal would trigger GDI substitution.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace tf {

// A build enters it for as long as it draws; the swap locks it.
class FontGate {
public:
    // A build, before its first font: waits while a swap runs. False when `cancel` turned true meanwhile.
    bool enter(const std::atomic<bool>* cancel) {
        std::unique_lock<std::mutex> lk(m_);
        while (held_) {
            if (cancel && cancel->load()) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(20));
        }
        ++users_;
        return true;
    }
    void leave() {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (users_ > 0) --users_;
        }
        cv_.notify_all();
    }
    // Block new builds and wait up to ms for active builds. Reopen the gate on timeout or abort.
    bool lock(unsigned long ms) {
        std::unique_lock<std::mutex> lk(m_);
        if (aborted_) return false;
        held_ = true;
        const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return users_ == 0 || aborted_; }) && !aborted_;
        if (!ok) {
            held_ = false;
            cv_.notify_all();
        }
        return ok;
    }
    // Release's unload: held only when no build is running at this moment (whatever abort() said).
    bool tryLock() {
        std::lock_guard<std::mutex> lk(m_);
        if (users_ != 0 || held_) return false;
        held_ = true;
        return true;
    }
    void unlock() {
        {
            std::lock_guard<std::mutex> lk(m_);
            held_ = false;
            ++generation_;   // a swap may have changed what a family name draws with
        }
        cv_.notify_all();
    }
    // Bumped by every swap; a build's kept shapes are re-used only under the generation that drew them.
    unsigned generation() {
        std::lock_guard<std::mutex> lk(m_);
        return generation_;
    }
    // Release: a swap waiting for a build gives up, and later swaps change nothing.
    void abort() {
        {
            std::lock_guard<std::mutex> lk(m_);
            aborted_ = true;
        }
        cv_.notify_all();
    }
    int users() {
        std::lock_guard<std::mutex> lk(m_);
        return users_;
    }
    class Use {
    public:
        Use(FontGate& g, const std::atomic<bool>* cancel) : g_(g), in_(g.enter(cancel)) {}
        ~Use() { if (in_) g_.leave(); }
        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;
        bool in() const { return in_; }

    private:
        FontGate& g_;
        bool in_;
    };

private:
    std::mutex m_;
    std::condition_variable cv_;
    int users_ = 0;
    unsigned generation_ = 0;
    bool held_ = false, aborted_ = false;
};
inline FontGate& fontGate() {
    static FontGate g;
    return g;
}

}  // namespace tf

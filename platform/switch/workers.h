// Switch worker threads (libnx), each pinned to its own CPU core.
//
// Applications get cores 0-2; the main thread runs on core 0. A thread made
// with the default core would share core 0 with the main thread and gain
// nothing, so every worker here is created on an explicit core.
//
//   RasterLanes: cores 1 and 2 draw scanline lanes of the 3D layer while the
//                main thread draws lane 0 (Raster::set_parallel). Synchronous.
//   SoundThread: core 2, one sound-board frame at a time, overlapping the
//                next main-board frame (GameLoop::run_frame_sound_packet).
#pragma once
#include <switch.h>
#include <algorithm>
#include <exception>
#include <functional>
#include <string>

namespace nx {

inline int current_priority() {
    s32 priority = 0x2c;
    if (R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE))) priority = 0x2c;
    return priority;
}

class RasterLanes {
public:
    static constexpr int kWorkers = 2; // cores 1 and 2
    RasterLanes() = default;
    RasterLanes(const RasterLanes &) = delete;
    RasterLanes &operator=(const RasterLanes &) = delete;
    ~RasterLanes() { close(); }

    // Returns how many lanes are available (1 = no workers, serial).
    int open() {
        close();
        mutexInit(&mutex_);
        condvarInit(&start_);
        condvarInit(&done_);
        quit_ = false;
        generation_ = 0;
        const int priority = current_priority();
        for (int k = 0; k < kWorkers; ++k) {
            slots_[k] = {this, k + 1, 0};
            if (R_FAILED(threadCreate(&threads_[k], entry, &slots_[k], nullptr, 0x40000, priority, k + 1))) break;
            if (R_FAILED(threadStart(&threads_[k]))) { threadClose(&threads_[k]); break; }
            ++started_;
        }
        return started_ + 1;
    }
    void close() {
        if (!started_) return;
        mutexLock(&mutex_);
        quit_ = true;
        condvarWakeAll(&start_);
        mutexUnlock(&mutex_);
        for (int k = 0; k < started_; ++k) { threadWaitForExit(&threads_[k]); threadClose(&threads_[k]); }
        started_ = 0;
    }
    int lanes() const { return started_ + 1; }

    // job(0) runs here; job(1..count-1) on the workers. count <= lanes().
    void run(int count, const std::function<void(int)> &job) {
        const int workers = std::min(count - 1, started_);
        if (workers <= 0) { for (int k = 0; k < count; ++k) job(k); return; }
        mutexLock(&mutex_);
        job_ = &job;
        active_ = workers;
        pending_ = workers;
        ++generation_;
        condvarWakeAll(&start_);
        mutexUnlock(&mutex_);
        job(0);
        mutexLock(&mutex_);
        while (pending_) condvarWait(&done_, &mutex_);
        job_ = nullptr;
        mutexUnlock(&mutex_);
    }

private:
    struct Slot { RasterLanes *owner; int lane; u64 seen; };
    static void entry(void *arg) {
        Slot &slot = *static_cast<Slot *>(arg);
        RasterLanes &self = *slot.owner;
        for (;;) {
            mutexLock(&self.mutex_);
            while (!self.quit_ && self.generation_ == slot.seen) condvarWait(&self.start_, &self.mutex_);
            if (self.quit_) { mutexUnlock(&self.mutex_); return; }
            slot.seen = self.generation_;
            const bool mine = slot.lane <= self.active_;
            const std::function<void(int)> *job = self.job_;
            mutexUnlock(&self.mutex_);
            if (!mine) continue;
            (*job)(slot.lane); // the rasterizer does not throw
            mutexLock(&self.mutex_);
            if (--self.pending_ == 0) condvarWakeAll(&self.done_);
            mutexUnlock(&self.mutex_);
        }
    }
    Thread threads_[kWorkers];
    Slot slots_[kWorkers];
    int started_ = 0;
    Mutex mutex_;
    CondVar start_, done_;
    bool quit_ = false;
    u64 generation_ = 0;
    int active_ = 0, pending_ = 0;
    const std::function<void(int)> *job_ = nullptr;
};

// One task in flight at a time: submit, then wait before the next submit.
class SoundThread {
public:
    SoundThread() = default;
    SoundThread(const SoundThread &) = delete;
    SoundThread &operator=(const SoundThread &) = delete;
    ~SoundThread() { close(); }

    bool open() {
        close();
        mutexInit(&mutex_);
        condvarInit(&wake_);
        quit_ = busy_ = false;
        // One step above the main thread: a short task that should not wait
        // behind a raster lane sharing its core.
        const int priority = std::max(current_priority() - 1, 0x18);
        if (R_FAILED(threadCreate(&thread_, entry, this, nullptr, 0x80000, priority, 2))) return false;
        if (R_FAILED(threadStart(&thread_))) { threadClose(&thread_); return false; }
        running_ = true;
        return true;
    }
    void close() {
        if (!running_) return;
        wait();
        mutexLock(&mutex_);
        quit_ = true;
        condvarWakeAll(&wake_);
        mutexUnlock(&mutex_);
        threadWaitForExit(&thread_);
        threadClose(&thread_);
        running_ = false;
    }
    bool threaded() const { return running_; }

    void submit(std::function<uint64_t()> task) {
        if (!running_) { result_ = 0; error_.clear(); run(task); return; }
        mutexLock(&mutex_);
        task_ = std::move(task);
        busy_ = true;
        condvarWakeAll(&wake_);
        mutexUnlock(&mutex_);
    }
    // Joins the task in flight (if any). Returns its ticks; `error` is empty
    // unless it threw.
    uint64_t wait(std::string *error = nullptr) {
        if (running_) {
            mutexLock(&mutex_);
            while (busy_) condvarWait(&wake_, &mutex_);
            mutexUnlock(&mutex_);
        }
        if (error) *error = error_;
        error_.clear();
        const uint64_t ticks = result_;
        result_ = 0;
        return ticks;
    }

private:
    void run(std::function<uint64_t()> &task) {
        try { result_ = task(); }
        catch (const std::exception &e) { error_ = e.what(); }
        catch (...) { error_ = "sound worker: unknown error"; }
        task = nullptr;
    }
    static void entry(void *arg) {
        SoundThread &self = *static_cast<SoundThread *>(arg);
        mutexLock(&self.mutex_);
        for (;;) {
            while (!self.quit_ && !self.busy_) condvarWait(&self.wake_, &self.mutex_);
            if (self.quit_) break;
            mutexUnlock(&self.mutex_);
            self.run(self.task_);
            mutexLock(&self.mutex_);
            self.busy_ = false;
            condvarWakeAll(&self.wake_);
        }
        mutexUnlock(&self.mutex_);
    }
    Thread thread_;
    bool running_ = false;
    Mutex mutex_;
    CondVar wake_;
    bool quit_ = false, busy_ = false;
    std::function<uint64_t()> task_;
    uint64_t result_ = 0;
    std::string error_;
};

} // namespace nx

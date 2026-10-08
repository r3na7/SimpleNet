#pragma once

#include <functional>

namespace snet {
class EventLoop;
namespace detail {

// A prepared, single-threaded work registration. It must outlive its action;
// moving/destroying the registration from inside the action is prohibited.
class LoopWork {
public:
    LoopWork(EventLoop& loop, std::function<void()> action);
    ~LoopWork() noexcept;
    LoopWork(const LoopWork&) = delete;
    LoopWork& operator=(const LoopWork&) = delete;
    LoopWork(LoopWork&&) = delete;
    LoopWork& operator=(LoopWork&&) = delete;
    void schedule() noexcept;
    void cancel() noexcept;
    bool pending() const noexcept { return pending_; }
    bool executing() const noexcept { return executing_; }
private:
    friend class snet::EventLoop;
    EventLoop& loop_;
    std::function<void()> action_;
    LoopWork* prev_ = nullptr;
    LoopWork* next_ = nullptr;
    LoopWork* all_prev_ = nullptr;
    LoopWork* all_next_ = nullptr;
    bool pending_ = false;
    bool executing_ = false;
};
} // namespace detail
} // namespace snet

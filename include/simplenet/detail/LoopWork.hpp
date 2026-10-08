#pragma once

#include <functional>

namespace snet {
class EventLoop;
/// @brief Internal single-threaded component-building utilities.
namespace detail {
struct WorkQueue;

/**
 * @brief A prepared single-threaded action with cancellation and stable address.
 * @pre Its EventLoop outlives this registration.
 * @warning Do not destroy this registration from its executing action.
 */
class LoopWork {
public:
    /**
     * @brief Prepares an action without scheduling it.
     * @param loop The associated single-threaded loop.
     * @param action A fixed callable; executed only by loop(), never by schedule().
     * @throws std::invalid_argument The action is empty.
     * @note Preparing the callable may allocate; queued operations do not.
     */
    LoopWork(EventLoop& loop, std::function<void()> action);
    /// @brief Cancels pending work and unlinks the registration.
    ~LoopWork() noexcept;
    /// @brief Stable-address registrations cannot be copied.
    LoopWork(const LoopWork&) = delete;
    /// @brief Stable-address registrations cannot be assigned.
    LoopWork& operator=(const LoopWork&) = delete;
    /// @brief Stable-address registrations cannot be moved.
    LoopWork(LoopWork&&) = delete;
    /// @brief Stable-address registrations cannot be move-assigned.
    LoopWork& operator=(LoopWork&&) = delete;
    /// @brief Enqueues once; rescheduling an executing action waits for another phase.
    void schedule() noexcept;
    /// @brief Unlinks a pending invocation; does not interrupt an executing action.
    void cancel() noexcept;
    /// @brief Checks both queue generations.
    /// @return True if an invocation is waiting.
    bool pending() const noexcept { return pending_; }
    /// @brief Checks whether this registration's action is executing.
    /// @return True inside the action; false after normal or exceptional return.
    bool executing() const noexcept { return executing_; }
private:
    friend class snet::EventLoop;
    EventLoop& loop_;
    std::function<void()> action_;
    LoopWork* prev_ = nullptr;
    LoopWork* next_ = nullptr;
    LoopWork* all_prev_ = nullptr;
    LoopWork* all_next_ = nullptr;
    WorkQueue* queue_ = nullptr;
    bool pending_ = false;
    bool executing_ = false;
};
/// @cond INTERNAL
struct WorkQueue {
    LoopWork* head = nullptr;
    LoopWork* tail = nullptr;
};
/// @endcond
} // namespace detail
} // namespace snet

#pragma once

/**
 * @file EventLoop.hpp
 * @brief A ready-to-use Reactor loop.
 */

#include "Poller.hpp"
#include "detail/LoopWork.hpp"
#include <cstddef>
#include <cstdint>

namespace snet
{

/**
 * @brief A ready-to-use single-threaded event waiting and dispatch loop.
 *
 * Owns its Poller, but not channels or their fds. All operations and callbacks run
 * in one thread without synchronization. The Channel and Poller lifetime contract
 * applies: stable addresses, successful removal before destroying channels or
 * closing fds, and no destruction of a channel during its dispatch.
 *
 * Copying and moving are prohibited by Poller's restrictions. The implicit
 * constructor may throw exceptions from Poller::Poller(). The internal Poller
 * initially uses default settings; its batch capacity and timeout can be changed
 * through this API in the same thread, including from callbacks or between runs.
 * @see docs/reactor.md
 */
class EventLoop
{
public:
    /**
     * @brief Adds a channel or applies its requested mask.
     * @param ch The channel; nullptr has no effect.
     * @pre The preconditions of Poller::update_channel() hold.
     * @throws std::logic_error The fd belongs to another channel.
     * @throws std::system_error epoll_ctl() failed.
     * @throws std::bad_alloc Allocation failed while adding the registration.
     * @see Poller::update_channel()
     */
    void update_channel(snet::Channel *ch);

    /**
     * @brief Removes registration without closing the fd or destroying the channel.
     * @param ch The channel; nullptr or an unregistered fd has no effect.
     * @pre The preconditions of Poller::remove_channel() hold.
     * @throws std::logic_error The fd belongs to another channel.
     * @throws std::system_error epoll_ctl() failed.
     * @note May be called from a callback: cancels remaining callbacks of the current
     *       channel or clears a pending channel's entry in the batch.
     * @see Poller::remove_channel()
     */
    void remove_channel(snet::Channel *ch);

    /**
     * @brief Changes the maximum number of events per wait.
     * @param max_events Positive int batch capacity from 1 through INT_MAX, inclusive.
     * @pre Called in the thread that uses this EventLoop.
     * @throws std::invalid_argument The capacity is zero or negative.
     * @throws std::bad_alloc Allocation failed while resizing the event buffer.
     * @throws std::length_error The capacity exceeds the vector's limits.
     * @note Applies to subsequent waits without changing the current or saved batch.
     *       Does not limit channel registrations. Failure preserves the previous setting.
     * @see Poller::set_max_events()
     */
    void set_max_events(int max_events);

    /**
     * @brief Returns the maximum number of events per wait.
     * @return The stored batch capacity (1024 by default).
     * @pre Called in the thread that uses this EventLoop.
     */
    int get_max_events() const noexcept;

    /**
     * @brief Changes the timeout for subsequent event waits.
     * @param timeout Milliseconds: -1 waits indefinitely, 0 checks immediately,
     *                and positive values specify a bounded wait.
     * @pre Called in the thread that uses this EventLoop.
     * @note The value is stored without validation; other negative values are
     *       outside the contract. Does not interrupt an ongoing wait. Expiration
     *       does not invoke a callback or return control from loop(); waiting repeats.
     * @warning A zero timeout may cause loop() to busy-poll and consume CPU.
     * @see Poller::set_timeout()
     */
    void set_timeout(int timeout) noexcept;

    /**
     * @brief Returns the event wait timeout.
     * @return The stored timeout in milliseconds (initially -1).
     * @pre Called in the thread that uses this EventLoop.
     */
    int get_timeout() const noexcept;

    /**
     * @brief Waits for events and calls Channel::handle_event() until stopped.
     * @pre One execution thread; Channel and Poller contracts are respected.
     * @throws std::logic_error This loop() is already running (reentrant call).
     * @throws std::system_error Waiting failed in the internal Poller.
     * @throws std::bad_alloc Event batch preparation failed.
     * @note Callback exceptions propagate. On any exception, iteration stops and
     *       execution flags are reset.
     * @note After a callback exception, the next loop() resumes the saved batch
     *       at the next channel before waiting for new events. The channel that
     *       threw and its remaining callbacks are not retried. Removed channels
     *       are skipped; pending channels must remain alive until dispatch or removal.
     * @note Exceptions while preparing a batch in Poller::poll() do not provide
     *       this continuation guarantee. Resuming dispatch does not restore
     *       application state changed by the failed callback.
     * @note Skips nullptr entries; finishes the current batch after quit().
     *       Registrations survive exit and loop() may be called again.
     */
    void loop();

    void set_work_budget(std::size_t count);
    std::size_t get_work_budget() const noexcept { return work_budget_; }
    std::uint64_t iteration_id() const noexcept { return iteration_id_; }

    /**
     * @brief Requests termination after the current event batch.
     * @pre Called in the same thread that runs loop().
     * @note Does not interrupt the callback, cancel remaining callbacks or channels
     *       in the batch, unregister channels, or close resources.
     *       Calling this before loop() does not prevent startup.
     * @warning Does not wake epoll_wait(); calls from another thread are unsupported.
     */
    void quit();

private:
    friend class detail::LoopWork;
    void schedule_work(detail::LoopWork& work) noexcept;
    void cancel_work(detail::LoopWork& work) noexcept;
    void run_work();
    detail::WorkQueue queues_[2];
    detail::WorkQueue* ready_ = &queues_[0];
    detail::WorkQueue* phase_ = &queues_[1];
    std::size_t work_budget_ = 64;
    std::uint64_t iteration_id_ = 0;
    detail::LoopWork* all_work_ = nullptr;
    Poller poller_;

    const std::vector<Channel *> *pending_batch_ = nullptr;
    size_t next_channel_ = 0;

    bool running_ = false;
    bool looping_ = false;
};

} // namespace snet

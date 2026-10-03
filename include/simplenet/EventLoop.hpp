#pragma once

/**
 * @file EventLoop.hpp
 * @brief A ready-to-use Reactor loop.
 */

#include "Poller.hpp"

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
 * uses default settings; they cannot be changed through the EventLoop API.
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
     * @brief Waits for events and calls Channel::handle_event() until stopped.
     * @pre One execution thread; Channel and Poller contracts are respected.
     * @throws std::logic_error This loop() is already running (reentrant call).
     * @throws std::system_error Waiting failed in the internal Poller.
     * @throws std::bad_alloc Event batch preparation failed.
     * @note Callback exceptions propagate. On any exception, iteration stops and
     *       execution flags are reset.
     * @note Skips nullptr entries; finishes the current batch after quit().
     *       Registrations survive exit and loop() may be called again; redelivery
     *       of unprocessed events is not guaranteed.
     */
    void loop();

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
    Poller poller_;

    bool running_ = false;
    bool looping_ = false;
};

} // namespace snet

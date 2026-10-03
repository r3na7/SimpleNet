#pragma once

/**
 * @file Poller.hpp
 * @brief Channel registration and event waiting.
 */

#include "Channel.hpp"

#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

namespace snet
{

/**
 * @brief Single-threaded channel registration and event waiting with Linux epoll.
 *
 * Owns only the epoll fd; stores non-owning Channel* pointers. Does not invoke
 * callbacks, destroy channels, or close monitored fds. For a custom loop, use
 * poll() together with Channel::handle_event().
 *
 * @pre All operations on the Poller and its channels run in one thread.
 * @pre Each Channel is used with only one Poller or EventLoop.
 * @pre Registered channels keep a stable address; successfully unregister them
 *      before destroying them or closing their fds.
 * @warning Nested poll() while iterating over the current batch, including from
 *          a callback, is prohibited by the contract and is not checked.
 * @see docs/reactor.md
 */
class Poller
{
public:
    /**
     * @brief Creates an epoll fd with EPOLL_CLOEXEC and no registered channels.
     * @note Initial settings: at most 1024 events per wait, timeout -1.
     * @throws std::system_error epoll_create1() failed.
     * @throws std::bad_alloc Allocation of the event buffer failed.
     */
    Poller();

    /// @brief Copying and moving the epoll fd owner are prohibited.
    Poller(Poller &&poller) = delete;
    /// @brief Copying and moving the epoll fd owner are prohibited.
    Poller &operator=(Poller &&poller) = delete;

    /// @brief Copying and moving the epoll fd owner are prohibited.
    Poller(const Poller &) = delete;
    /// @brief Copying and moving the epoll fd owner are prohibited.
    Poller &operator=(const Poller &) = delete;

    /**
     * @brief Adds a channel (EPOLL_CTL_ADD) or updates its mask (EPOLL_CTL_MOD).
     * @param ch The channel; nullptr has no effect.
     * @pre A non-null ch is alive, has an open fd, and meets the class contract.
     * @throws std::logic_error This fd already belongs to another Channel object.
     * @throws std::system_error epoll_ctl() failed.
     * @throws std::bad_alloc Allocation failed while adding the registration.
     * @note Passes the requested mask without validating its bits. A zero mask is
     *       allowed and does not unregister the channel or cancel the current event.
     * @note On failed ADD, the entry is removed from the internal table; on failed MOD,
     *       the previous registration remains. The channel's requested mask is not
     *       automatically rolled back.
     */
    void update_channel(snet::Channel *ch);

    /**
     * @brief Removes registration through EPOLL_CTL_DEL.
     * @param ch The channel; nullptr or an fd absent from the table has no effect.
     * @pre A non-null ch is alive; a registered fd remains open.
     * @throws std::logic_error This fd belongs to another Channel object.
     * @throws std::system_error epoll_ctl() failed; registration, dispatch cancellation,
     *         and the current batch remain unchanged.
     * @note Success cancels remaining channel callbacks and replaces its pointers in
     *       the current internal vector with nullptr. The channel and fd are not
     *       destroyed; masks are not cleared. Registering again does not restore
     *       a cleared entry in the old batch.
     */
    void remove_channel(snet::Channel *ch);

    /**
     * @brief Performs one epoll_wait() and stores received masks in channels.
     * @return A reference to the internal vector of ready channels, valid until the
     *         Poller is destroyed. The next poll() replaces the contents; do not use
     *         previous iterators or references to elements.
     * @pre The previous batch is no longer being iterated over; no nested call occurs.
     * @throws std::system_error epoll_wait() failed with an error other than EINTR.
     * @throws std::bad_alloc Allocation failed while preparing the batch.
     * @note Returns an empty batch on timeout or EINTR; waiting is not retried for EINTR.
     *       Does not invoke callbacks; channel order does not guarantee the order of
     *       application actions.
     * @warning Iterate over the internal vector by reference and check for nullptr:
     *          remove_channel() may clear entries. A copy does not receive these
     *          changes and may contain invalid pointers.
     * @note The received batch is not guaranteed if allocation fails: waiting has
     *       already completed before allocation.
     */
    const std::vector<Channel *> &poll();

    /**
     * @brief Changes the maximum events per wait, not the number of registrations.
     * @param max_events Batch capacity from 1 through INT_MAX, inclusive.
     * @throws std::invalid_argument The value is outside the allowed range.
     * @throws std::bad_alloc Allocation failed while resizing the buffer.
     * @throws std::length_error The size exceeds the vector's limits.
     * @note The setting is stored only after successfully resizing the buffer.
     */
    void set_max_events(size_t max_events);
    /**
     * @brief Stores the wait timeout without validating the argument.
     * @param timeout Milliseconds: -1 waits indefinitely, 0 checks immediately,
     *                and positive values specify a bounded wait.
     * @note Actual waiting may exceed the timeout due to rounding and thread scheduling.
     *       Other negative values are outside the contract.
     */
    void set_timeout(int timeout) noexcept;

    /**
     * @brief Returns the maximum number of events per wait.
     * @return The stored batch capacity (initially 1024).
     */
    size_t get_max_events() const noexcept;
    /**
     * @brief Returns the wait timeout.
     * @return The stored value in milliseconds (initially -1).
     */
    int get_timeout() const noexcept;

    /**
     * @brief Closes only the epoll fd; the result of close() is ignored.
     * @note Monitored fds and channels remain the caller's responsibility.
     *       Callbacks are not invoked; the reference returned by poll() becomes invalid.
     */
    ~Poller();

private:
    void add_channel(snet::Channel *channel);

    void modify_channel(snet::Channel *channel);

    int epoll_fd_ = -1;
    size_t max_events_ = 1024;
    int timeout_ = -1;

    std::vector<epoll_event> events_;
    std::vector<snet::Channel *> active_channels_;
    std::unordered_map<int, snet::Channel *> channels_;
};

} // namespace snet
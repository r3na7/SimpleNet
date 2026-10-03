#pragma once

/**
 * @file Channel.hpp
 * @brief File descriptor monitoring and epoll event dispatch.
 */

#include <cstdint>
#include <functional>

namespace snet
{

/**
 * @brief A non-owning description of monitoring for one file descriptor.
 *
 * Stores the fd, requested and received epoll masks, and synchronous callbacks.
 * Does not create or close the fd, perform I/O, or unregister itself on destruction.
 * Use each channel with one Poller or EventLoop in one thread; there is no internal
 * synchronization.
 *
 * @pre The address of a registered channel remains stable.
 * @pre Before destroying a registered channel or closing its fd, successfully call
 *      remove_channel() on its Poller/EventLoop.
 * @warning Do not destroy a channel inside its own callback; the channel and objects
 *          used by callbacks must remain alive until handle_event() completes.
 * @see docs/reactor.md
 */
class Channel
{
public:
    /**
     * @brief Creates a channel with zero masks and empty callbacks.
     * @param fd An open file descriptor suitable for epoll.
     * @pre fd >= 0; the caller configures the required I/O mode.
     * @note Only assert(fd >= 0) is checked; the check is absent with NDEBUG.
     */
    Channel(int fd);

    /// @brief Copying is forbidden: registered channels keep a stable address.
    Channel(const Channel &) = delete;
    /// @brief Moving the object is forbidden; moving a unique_ptr to it is allowed.
    Channel(Channel &&) = delete;

    /**
     * @brief Returns the stored fd number.
     * @return The descriptor number without checking whether it is open.
     */
    int get_fd() const noexcept;
    /**
     * @brief Returns the requested interest mask.
     * @return epoll bits; kernel state is not queried.
     * @note Until update_channel() succeeds, the mask may differ from the registration.
     */
    uint32_t get_events() const noexcept;
    /**
     * @brief Returns the last received event mask.
     * @return The mask set by Poller::poll(), or the initial zero.
     * @note Current fd readiness is not checked. The mask is not cleared after dispatch,
     *       removal, or absence from a new batch.
     */
    uint32_t get_revents() const noexcept;

    /**
     * @brief Replaces the requested mask without contacting the kernel.
     * @param events A new combination of epoll bits; the library does not validate it.
     * @note Apply the mask through Poller::update_channel() or EventLoop::update_channel().
     */
    void set_events(uint32_t events) noexcept;
    /**
     * @brief Adds bits to the requested mask without contacting the kernel.
     * @param event One or more epoll bits.
     * @see set_events()
     */
    void add_event(uint32_t event) noexcept;
    /**
     * @brief Removes bits from the requested mask without contacting the kernel.
     * @param event One or more epoll bits to remove.
     * @see set_events()
     */
    void remove_event(uint32_t event) noexcept;
    /**
     * @brief Clears only the requested mask.
     * @note Does not unregister the channel, clear the received mask, or cancel current
     *       dispatch. Apply the change through update_channel(). Even with a zero mask,
     *       the kernel may report EPOLLERR and EPOLLHUP.
     */
    void clear_events() noexcept;

    /**
     * @brief Replaces the EPOLLIN callback.
     * @param callback A handler taking no arguments; an empty std::function disables it.
     * @throws std::logic_error This channel's read callback is currently executing.
     * @note Runs synchronously in the handle_event() thread, performs its own I/O,
     *       and may throw exceptions. The setter does not change the interest mask.
     * @note Preparing the std::function argument (for example, copying a callable)
     *       may throw; the setter moves the argument into the member.
     */
    void set_read_callback(std::function<void()> callback);
    /**
     * @brief Replaces the EPOLLOUT callback.
     * @param callback A handler taking no arguments; an empty std::function disables it.
     * @throws std::logic_error This channel's write callback is currently executing.
     * @note Runs synchronously in the handle_event() thread, performs its own I/O,
     *       and may throw exceptions. The setter does not change the interest mask.
     * @note Preparing the std::function argument (for example, copying a callable)
     *       may throw; the setter moves the argument into the member.
     */
    void set_write_callback(std::function<void()> callback);
    /**
     * @brief Replaces the EPOLLERR callback.
     * @param callback A handler taking no arguments; an empty std::function disables it.
     * @throws std::logic_error This channel's error callback is currently executing.
     * @note Runs synchronously in the handle_event() thread, performs its own I/O,
     *       and may throw exceptions. The setter does not change the interest mask.
     * @note Preparing the std::function argument (for example, copying a callable)
     *       may throw; the setter moves the argument into the member.
     */
    void set_error_callback(std::function<void()> callback);

    /**
     * @brief Invokes callbacks for an event prepared by Poller::poll().
     *
     * Resets the cancellation flag and uses a snapshot of the received mask.
     * Non-empty callbacks run in error, read, write order for EPOLLERR, EPOLLIN,
     * and EPOLLOUT. Successful remove_channel() cancels the remaining calls;
     * changing the requested mask or calling update_channel() does not.
     *
     * @pre The channel is in the current Poller batch and has not been unregistered.
     * @pre The event has not been dispatched yet; the next poll() has not run.
     * @pre The channel and objects used by callbacks remain alive until the call completes.
     * @throws std::logic_error handle_event() is already executing for this channel.
     * @note Callback exceptions propagate to the caller and stop dispatch.
     * @note Dispatch and callback execution markers are restored on every exit,
     *       including exceptions. Callbacks may replace other handlers, but not themselves.
     * @note Does not wait for events or clear the received mask. EPOLLHUP, EPOLLRDHUP,
     *       and EPOLLPRI alone do not invoke callbacks.
     * @warning Only reentry is checked. A repeated call may dispatch an old event;
     *          calling this method for an unregistered channel is invalid.
     * @warning Do not destroy this channel in its callback or call the next poll()
     *          while iterating over the current batch.
     */
    void handle_event();

private:
    friend class Poller;
    friend class EventLoop;

    void stop_event_dispatch() noexcept;
    void set_revents(uint32_t events) noexcept;

    void handle_read();
    void handle_write();
    void handle_error();
    void invoke_callback(std::function<void()> &callback);

    int fd_ = -1;

    uint32_t events_ = 0;
    uint32_t revents_ = 0;

    std::function<void()> read_callback_;
    std::function<void()> write_callback_;
    std::function<void()> error_callback_;

    bool dispatch_cancelled_ = false;
    bool dispatching_ = false;
    std::function<void()> *active_callback_ = nullptr;
};

} // namespace snet

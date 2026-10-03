#include "Channel.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>

#include <sys/epoll.h>

namespace
{

// Used only for bool and pointer state; restores the previous value on all exits.
template <typename T>
class ScopedValue
{
public:
    ScopedValue(T &value, T replacement) noexcept : value_(value), previous_(value)
    {
        value_ = replacement;
    }

    ~ScopedValue() noexcept { value_ = previous_; }

    ScopedValue(const ScopedValue &) = delete;
    ScopedValue &operator=(const ScopedValue &) = delete;

private:
    T &value_;
    T previous_;
};

} // namespace

snet::Channel::Channel(int fd) : fd_(fd) { assert(fd >= 0); }

int snet::Channel::get_fd() const noexcept { return fd_; }

uint32_t snet::Channel::get_events() const noexcept { return events_; }

uint32_t snet::Channel::get_revents() const noexcept { return revents_; }

void snet::Channel::set_events(uint32_t events) noexcept { events_ = events; }

void snet::Channel::add_event(uint32_t event) noexcept { events_ |= event; }

void snet::Channel::remove_event(uint32_t event) noexcept { events_ &= ~event; }

void snet::Channel::clear_events() noexcept { events_ = 0; }

void snet::Channel::set_read_callback(std::function<void()> callback)
{
    if (active_callback_ == &read_callback_)
        throw std::logic_error("Cannot replace the executing read callback");
    read_callback_ = std::move(callback);
}

void snet::Channel::set_write_callback(std::function<void()> callback)
{
    if (active_callback_ == &write_callback_)
        throw std::logic_error("Cannot replace the executing write callback");
    write_callback_ = std::move(callback);
}

void snet::Channel::set_error_callback(std::function<void()> callback)
{
    if (active_callback_ == &error_callback_)
        throw std::logic_error("Cannot replace the executing error callback");
    error_callback_ = std::move(callback);
}

void snet::Channel::handle_event()
{
    if (dispatching_)
        throw std::logic_error("Channel::handle_event() is already running");

    ScopedValue dispatch_guard(dispatching_, true);
    dispatch_cancelled_ = false;
    const uint32_t events = revents_;

    if (events & EPOLLERR) {
        handle_error();
        if (dispatch_cancelled_)
            return;
    }

    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
        handle_read();
        if (dispatch_cancelled_)
            return;
    }

    if (events & EPOLLOUT) {
        handle_write();
        if (dispatch_cancelled_)
            return;
    }
}

void snet::Channel::stop_event_dispatch() noexcept { dispatch_cancelled_ = true; }

void snet::Channel::set_revents(uint32_t revents) noexcept { revents_ = revents; }

void snet::Channel::handle_read() { invoke_callback(read_callback_); }

void snet::Channel::handle_write() { invoke_callback(write_callback_); }

void snet::Channel::handle_error() { invoke_callback(error_callback_); }

void snet::Channel::invoke_callback(std::function<void()> &callback)
{
    if (callback) {
        ScopedValue callback_guard(active_callback_, &callback);
        callback();
    }
}

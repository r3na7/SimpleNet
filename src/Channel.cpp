#include "Channel.hpp"

#include <cassert>

#include <sys/epoll.h>

snet::Channel::Channel(int fd) : fd_(fd) { assert(fd >= 0); }

int snet::Channel::get_fd() const noexcept { return fd_; }

uint32_t snet::Channel::get_events() const noexcept { return events_; }

uint32_t snet::Channel::get_revents() const noexcept { return revents_; }

void snet::Channel::set_events(uint32_t events) noexcept { events_ = events; }

void snet::Channel::add_event(uint32_t event) noexcept { events_ |= event; }

void snet::Channel::remove_event(uint32_t event) noexcept { events_ &= ~event; }

void snet::Channel::set_revents(uint32_t revents) noexcept { revents_ = revents; }

void snet::Channel::set_read_callback(std::function<void()> callback) { read_callback_ = std::move(callback); }

void snet::Channel::set_write_callback(std::function<void()> callback) { write_callback_ = std::move(callback); }

void snet::Channel::set_error_callback(std::function<void()> callback) { error_callback_ = std::move(callback); }

void snet::Channel::handle_read()
{
    if (read_callback_)
        read_callback_();
}

void snet::Channel::handle_write()
{
    if (write_callback_)
        write_callback_();
}

void snet::Channel::handle_error()
{
    if (error_callback_)
        error_callback_();
}

void snet::Channel::handle_event()
{
    dispatch_cancelled_ = false;
    const uint32_t events = revents_;

    if (events & EPOLLERR) {
        handle_error();
        if (dispatch_cancelled_)
            return;
    }

    if (events & EPOLLIN) {
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

void snet::Channel::stop_handling_current_event() noexcept { dispatch_cancelled_ = true; }

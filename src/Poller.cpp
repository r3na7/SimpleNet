#include "Poller.hpp"

#include <limits>
#include <system_error>
#include <unistd.h>

snet::Poller::Poller() : events_(max_events_)
{
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1)
        throw std::system_error(errno, std::system_category(), "epoll_create1");
}

void snet::Poller::update_channel(snet::Channel *channel)
{

    if (channel == nullptr)
        return;

    auto it = channels_.find(channel->get_fd());

    if (it == channels_.end()) {
        add_channel(channel);
    } else if (it->second == channel) {
        modify_channel(channel);
    } else {
        throw std::logic_error("fd belongs to another Channel");
    }
}

void snet::Poller::remove_channel(snet::Channel *channel)
{

    if (channel == nullptr)
        return;

    auto it = channels_.find(channel->get_fd());

    if (it == channels_.end()) {
        return;
    } else if (it->second == channel) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, channel->get_fd(), nullptr) == -1)
            throw std::system_error(errno, std::system_category(), "epoll_ctl: removechannel");
    } else {
        throw std::logic_error("fd belongs to another Channel");
    }

    channel->stop_handling_current_event();
    channels_.erase(it);

    for (auto &active_channel : active_channels_) {
        if (active_channel == channel)
            active_channel = nullptr;
    }
}

const std::vector<snet::Channel *> &snet::Poller::poll()
{
    int num_events = epoll_wait(epoll_fd_, events_.data(), events_.size(), timeout_);

    if (num_events == -1) {

        if (errno == EINTR) {
            active_channels_.clear();
            return active_channels_;
        }

        throw std::system_error(errno, std::system_category(), "epoll_wait");
    }

    active_channels_.clear();
    active_channels_.reserve(num_events);

    for (int i = 0; i < num_events; ++i) {

        snet::Channel *channel = static_cast<snet::Channel *>(events_[i].data.ptr);

        if (channel) {
            channel->set_revents(events_[i].events);
            active_channels_.push_back(channel);
        }
    }

    return active_channels_;
}

void snet::Poller::set_max_events(size_t max_events)
{

    if (max_events == 0 || max_events > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Invelid max_events");
    }

    events_.resize(max_events);

    max_events_ = max_events;
}

void snet::Poller::set_timeout(int timeout) noexcept { timeout_ = timeout; }

size_t snet::Poller::get_max_events() const noexcept { return max_events_; }

int snet::Poller::get_timeout() const noexcept { return timeout_; }

snet::Poller::~Poller()
{
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
    }
}

void snet::Poller::add_channel(snet::Channel *channel)
{

    epoll_event event{};
    event.events = channel->get_events();
    event.data.ptr = channel;

    auto [it, interested] = channels_.emplace(channel->get_fd(), channel);

    if (!interested) {
        throw std::logic_error("fd already belongs to a Channel");
    }

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, channel->get_fd(), &event) == -1) {

        int error = errno;

        channels_.erase(channel->get_fd());

        throw std::system_error(error, std::system_category(), "epoll_ctr");
    }
}

void snet::Poller::modify_channel(snet::Channel *channel)
{
    epoll_event event{};
    event.events = channel->get_events();
    event.data.ptr = channel;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, channel->get_fd(), &event) == -1)
        throw std::system_error(errno, std::system_category(), "epoll_ctr");
}

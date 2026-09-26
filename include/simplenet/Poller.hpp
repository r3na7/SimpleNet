#pragma once

#include "Channel.hpp"

#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

namespace snet
{

class Poller
{
public:
    Poller();

    Poller(Poller &&poller) = delete;
    Poller &operator=(Poller &&poller) = delete;

    Poller(const Poller &) = delete;
    Poller &operator=(const Poller &) = delete;

    void update_channel(snet::Channel *ch);

    void remove_channel(snet::Channel *ch);

    const std::vector<Channel *> &poll();

    void set_max_events(size_t max_events);
    void set_timeout(int timeout) noexcept;

    size_t get_max_events() const noexcept;
    int get_timeout() const noexcept;

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
#pragma once

#include "Poller.hpp"

namespace snet
{

class EventLoop
{
public:
    void update_channel(snet::Channel *ch);

    void remove_channel(snet::Channel *ch);

    void loop();

    void quit();

private:
    Poller poller_;

    bool running_ = false;
    bool looping_ = false;
};

} // namespace snet

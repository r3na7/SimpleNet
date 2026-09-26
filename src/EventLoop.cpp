#include "EventLoop.hpp"

#include <stdexcept>

void snet::EventLoop::update_channel(snet::Channel *ch) { poller_.update_channel(ch); }

void snet::EventLoop::remove_channel(snet::Channel *ch) { poller_.remove_channel(ch); }

void snet::EventLoop::loop()
{
    if (looping_) {
        throw std::logic_error("EventLoop::loop() is already running");
    }

    running_ = true;
    looping_ = true;

    try {
        while (running_) {

            auto &channels = poller_.poll();

            for (auto channel : channels) {
                if (channel != nullptr)
                    channel->handle_event();
            }
        }

    } catch (...) {
        running_ = false;
        looping_ = false;
        throw;
    }

    running_ = false;
    looping_ = false;
}

void snet::EventLoop::quit() { running_ = false; }

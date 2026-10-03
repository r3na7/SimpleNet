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

            if (pending_batch_ == nullptr) {
                pending_batch_ = &poller_.poll();
                next_channel_ = 0;
            }

            while (next_channel_ < pending_batch_->size()) {
                // Advance before dispatch so a throwing channel is not retried.
                auto *channel = (*pending_batch_)[next_channel_++];
                if (channel != nullptr)
                    channel->handle_event();
            }

            pending_batch_ = nullptr;
            next_channel_ = 0;
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

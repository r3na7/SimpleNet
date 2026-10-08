#include "EventLoop.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>

void snet::EventLoop::update_channel(snet::Channel *ch) { poller_.update_channel(ch); }

void snet::EventLoop::remove_channel(snet::Channel *ch) { poller_.remove_channel(ch); }

void snet::EventLoop::set_max_events(int max_events) { poller_.set_max_events(max_events); }

int snet::EventLoop::get_max_events() const noexcept { return poller_.get_max_events(); }

void snet::EventLoop::set_timeout(int timeout) noexcept { poller_.set_timeout(timeout); }

int snet::EventLoop::get_timeout() const noexcept { return poller_.get_timeout(); }

void snet::EventLoop::loop()
{
    if (looping_) {
        throw std::logic_error("EventLoop::loop() is already running");
    }

    running_ = true;
    looping_ = true;

    try {
        while (running_) {
            ++iteration_id_;

            if (pending_batch_ == nullptr) {
                pending_batch_ = &poller_.poll_with_timeout(
                    (ready_->head || phase_->head || cleanup_requested_) ? 0 : poller_.get_timeout());
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
            run_work();
            run_cleanup(detail::CleanupReason::normal);
            collect_retired();
        }

    } catch (...) {
        for (auto *entry = retired_; entry; entry = entry->next_)
            entry->cancel_pending();
        collect_retired();
        running_ = false;
        looping_ = false;
        throw;
    }

    running_ = false;
    looping_ = false;
}

void snet::EventLoop::quit() { running_ = false; }

void snet::EventLoop::set_work_budget(std::size_t count)
{
    if (count == 0)
        throw std::invalid_argument("Work budget must be positive");
    work_budget_ = count;
}

void snet::EventLoop::schedule_work(detail::LoopWork &work) noexcept
{
    if (work.pending_)
        return;
    work.pending_ = true;
    work.queue_ = ready_;
    work.prev_ = ready_->tail;
    work.next_ = nullptr;
    if (ready_->tail)
        ready_->tail->next_ = &work;
    else
        ready_->head = &work;
    ready_->tail = &work;
}

void snet::EventLoop::cancel_work(detail::LoopWork &work) noexcept
{
    if (!work.pending_)
        return;
    auto &queue = *work.queue_;
    if (work.prev_)
        work.prev_->next_ = work.next_;
    else
        queue.head = work.next_;
    if (work.next_)
        work.next_->prev_ = work.prev_;
    else
        queue.tail = work.prev_;
    work.prev_ = work.next_ = nullptr;
    work.queue_ = nullptr;
    work.pending_ = false;
}

void snet::EventLoop::run_work()
{
    if (!phase_->head)
        std::swap(ready_, phase_);
    const auto budget = work_budget_;
    for (std::size_t count = 0; count < budget && phase_->head; ++count) {
        auto *work = phase_->head;
        cancel_work(*work);
        work->executing_ = true;
        try {
            work->action_();
        } catch (...) {
            work->executing_ = false;
            throw;
        }
        work->executing_ = false;
    }
}

void snet::EventLoop::retire_erased(std::unique_ptr<detail::RetirementEntry> entry) noexcept
{
    entry->next_ = retired_;
    retired_ = entry.release();
}

void snet::EventLoop::collect_retired() noexcept
{
    auto **cursor = &retired_;
    while (*cursor) {
        auto *entry = *cursor;
        if (entry->can_destroy()) {
            *cursor = entry->next_;
            delete entry;
        } else
            cursor = &entry->next_;
    }
}

snet::EventLoop::~EventLoop() noexcept
{
    assert(!looping_);
    for (auto *work = all_work_; work; work = work->all_next_)
        cancel_work(*work);
    for (auto *entry = retired_; entry; entry = entry->next_)
        entry->cancel_pending();
    while (retired_) {
        auto *entry = retired_;
        retired_ = entry->next_;
        delete entry;
    }
    assert(all_work_ == nullptr); // External registrations must not outlive the loop.
}

void snet::EventLoop::request_cleanup() noexcept { cleanup_requested_ = true; }

void snet::EventLoop::run_cleanup(detail::CleanupReason reason) noexcept
{
    assert(!cleaning_);
    cleanup_requested_ = false;
    cleaning_ = true;
    for (auto *registration = cleanup_head_; registration; registration = registration->next_)
        registration->action_(registration->context_, reason);
    cleaning_ = false;
}

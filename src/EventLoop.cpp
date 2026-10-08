#include "EventLoop.hpp"

#include <stdexcept>
#include <cassert>
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

            if (pending_batch_ == nullptr) {
                pending_batch_ = &poller_.poll_with_timeout(work_head_ ? 0 : poller_.get_timeout());
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

snet::detail::LoopWork::LoopWork(EventLoop& loop, std::function<void()> action)
    : loop_(loop), action_(std::move(action))
{
    if (!action_) throw std::invalid_argument("Empty work action");
    all_next_ = loop_.all_work_;
    if (all_next_) all_next_->all_prev_ = this;
    loop_.all_work_ = this;
}

snet::detail::LoopWork::~LoopWork() noexcept
{
    assert(!executing_);
    cancel();
    if (all_prev_) all_prev_->all_next_ = all_next_;
    else loop_.all_work_ = all_next_;
    if (all_next_) all_next_->all_prev_ = all_prev_;
}

void snet::detail::LoopWork::schedule() noexcept { loop_.schedule_work(*this); }
void snet::detail::LoopWork::cancel() noexcept { loop_.cancel_work(*this); }

void snet::EventLoop::schedule_work(detail::LoopWork& work) noexcept
{
    if (work.pending_) return;
    work.pending_ = true;
    work.prev_ = work_tail_;
    work.next_ = nullptr;
    if (work_tail_) work_tail_->next_ = &work;
    else work_head_ = &work;
    work_tail_ = &work;
}

void snet::EventLoop::cancel_work(detail::LoopWork& work) noexcept
{
    if (!work.pending_) return;
    if (work.prev_) work.prev_->next_ = work.next_;
    else work_head_ = work.next_;
    if (work.next_) work.next_->prev_ = work.prev_;
    else work_tail_ = work.prev_;
    work.prev_ = work.next_ = nullptr;
    work.pending_ = false;
}

void snet::EventLoop::run_work()
{
    while (work_head_) {
        auto* work = work_head_;
        cancel_work(*work);
        work->executing_ = true;
        try { work->action_(); }
        catch (...) { work->executing_ = false; throw; }
        work->executing_ = false;
    }
}

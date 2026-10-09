#include "detail/LoopWork.hpp"

#include "EventLoop.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>

snet::detail::LoopWork::LoopWork(EventLoop &loop, std::function<void()> action)
    : loop_(loop), action_(std::move(action))
{
    if (!action_)
        throw std::invalid_argument("Empty work action");

    all_next_ = loop_.all_work_;

    if (all_next_)
        all_next_->all_prev_ = this;

    loop_.all_work_ = this;
}

snet::detail::LoopWork::~LoopWork() noexcept
{
    assert(!executing_);
    cancel();

    if (all_prev_)
        all_prev_->all_next_ = all_next_;
    else
        loop_.all_work_ = all_next_;

    if (all_next_)
        all_next_->all_prev_ = all_prev_;
}

void snet::detail::LoopWork::schedule() noexcept { loop_.schedule_work(*this); }

void snet::detail::LoopWork::cancel() noexcept { loop_.cancel_work(*this); }

bool snet::detail::LoopWork::pending() const noexcept { return pending_; }

bool snet::detail::LoopWork::executing() const noexcept { return executing_; }

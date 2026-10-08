#include "detail/LoopCleanup.hpp"

#include "EventLoop.hpp"

#include <cassert>
#include <stdexcept>

snet::detail::LoopCleanup::LoopCleanup(EventLoop &loop, void *context, Action action)
    : loop_(loop), context_(context), action_(action)
{
    if (!action_)
        throw std::invalid_argument("Empty cleanup action");
    if (loop_.cleaning_)
        throw std::logic_error("Cannot register cleanup during cleanup dispatch");
    next_ = loop_.cleanup_head_;
    if (next_)
        next_->prev_ = this;
    loop_.cleanup_head_ = this;
}

snet::detail::LoopCleanup::~LoopCleanup() noexcept
{
    assert(!loop_.cleaning_);
    if (prev_)
        prev_->next_ = next_;
    else
        loop_.cleanup_head_ = next_;
    if (next_)
        next_->prev_ = prev_;
}

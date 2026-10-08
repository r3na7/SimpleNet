#include <simplenet/EventLoop.hpp>
#include <simplenet/detail/LoopWork.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <vector>

TEST(LoopWorkTest, RunsWithoutSocket)
{
    snet::EventLoop loop;
    int calls = 0;
    snet::detail::LoopWork work(loop, [&] { ++calls; loop.quit(); });
    work.schedule();
    loop.loop();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(work.pending());
    EXPECT_FALSE(work.executing());
    EXPECT_EQ(loop.get_timeout(), -1);
}

TEST(LoopWorkTest, CoalescesAndCancels)
{
    snet::EventLoop loop;
    int first_calls = 0, cancelled_calls = 0;
    snet::detail::LoopWork first(loop, [&] { ++first_calls; });
    snet::detail::LoopWork cancelled(loop, [&] { ++cancelled_calls; });
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    first.schedule(); first.schedule();
    cancelled.schedule(); cancelled.cancel(); cancelled.cancel();
    stop.schedule();
    loop.loop();
    EXPECT_EQ(first_calls, 1);
    EXPECT_EQ(cancelled_calls, 0);
}

TEST(LoopWorkTest, CancelsAnotherRecord)
{
    snet::EventLoop loop;
    int first_calls = 0, cancelled_calls = 0;
    std::unique_ptr<snet::detail::LoopWork> second;
    snet::detail::LoopWork first(loop, [&] {
        ++first_calls; second->cancel(); second.reset(); loop.quit();
    });
    second = std::make_unique<snet::detail::LoopWork>(loop, [&] { ++cancelled_calls; });
    first.schedule(); second->schedule();
    loop.loop();
    EXPECT_EQ(first_calls, 1);
    EXPECT_EQ(cancelled_calls, 0);
}

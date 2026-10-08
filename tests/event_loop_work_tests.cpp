#include <simplenet/EventLoop.hpp>
#include <simplenet/detail/LoopWork.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>

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

TEST(LoopWorkTest, DefaultBudgetAndValidation)
{
    snet::EventLoop loop;
    EXPECT_EQ(loop.get_work_budget(), 64u);
    EXPECT_THROW(loop.set_work_budget(0), std::invalid_argument);
    EXPECT_EQ(loop.get_work_budget(), 64u);
    EXPECT_THROW((snet::detail::LoopWork(loop, {})), std::invalid_argument);
}

TEST(LoopWorkTest, QuitKeepsRemainingWork)
{
    snet::EventLoop loop;
    int calls = 0;
    std::vector<std::unique_ptr<snet::detail::LoopWork>> work;
    for (int i = 0; i < 65; ++i) {
        work.push_back(std::make_unique<snet::detail::LoopWork>(loop, [&, i] {
            ++calls;
            if (i == 0 || i == 64) loop.quit();
        }));
        work.back()->schedule();
    }
    loop.loop();
    ASSERT_EQ(calls, 64);
    EXPECT_TRUE(work.back()->pending());
    loop.loop();
    EXPECT_EQ(calls, 65);
}

TEST(LoopWorkTest, RescheduleWaitsForNextIteration)
{
    snet::EventLoop loop;
    std::vector<std::uint64_t> iterations;
    std::unique_ptr<snet::detail::LoopWork> work;
    work = std::make_unique<snet::detail::LoopWork>(loop, [&] {
        iterations.push_back(loop.iteration_id());
        if (iterations.size() < 2) work->schedule();
        loop.quit();
    });
    work->schedule();
    loop.loop();
    ASSERT_EQ(iterations.size(), 1u);
    loop.loop();
    ASSERT_EQ(iterations.size(), 2u);
    EXPECT_NE(iterations[0], iterations[1]);
}

TEST(LoopWorkTest, SocketAndWorkBothProgress)
{
    snet::EventLoop loop;
    const int fd = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    snet::Channel channel(fd);
    int socket_calls = 0, work_calls = 0;
    std::unique_ptr<snet::detail::LoopWork> work;
    channel.set_events(EPOLLIN);
    channel.set_read_callback([&] {
        uint64_t value; EXPECT_EQ(read(fd, &value, sizeof(value)), sizeof(value));
        ++socket_calls;
        if (work_calls > 0) loop.quit();
    });
    work = std::make_unique<snet::detail::LoopWork>(loop, [&] {
        ++work_calls;
        if (work_calls == 1) {
            uint64_t value = 1; EXPECT_EQ(write(fd, &value, sizeof(value)), sizeof(value));
            work->schedule();
        } else loop.quit();
    });
    loop.update_channel(&channel);
    work->schedule();
    loop.loop();
    EXPECT_GE(socket_calls, 2);
    EXPECT_EQ(work_calls, 2);
    loop.remove_channel(&channel);
    close(fd);
}

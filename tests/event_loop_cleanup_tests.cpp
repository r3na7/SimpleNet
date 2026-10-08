#include <simplenet/EventLoop.hpp>
#include <simplenet/detail/LoopCleanup.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace
{
using snet::detail::CleanupReason;
using snet::detail::LoopCleanup;

struct CleanupContext {
    std::function<void(CleanupReason)> action;
    static void invoke(void *context, CleanupReason reason) noexcept
    {
        static_cast<CleanupContext *>(context)->action(reason);
    }
};

class Fd
{
public:
    explicit Fd(int fd) : fd_(fd)
    {
        if (fd_ == -1)
            throw std::system_error(errno, std::system_category(), "eventfd");
    }
    ~Fd() { close(fd_); }
    int get() const { return fd_; }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

private:
    int fd_;
};
} // namespace

TEST(LoopCleanupTest, RunsWithoutSocket)
{
    snet::EventLoop loop;
    int calls = 0;
    CleanupContext context{[&](CleanupReason reason) {
        EXPECT_EQ(reason, CleanupReason::normal);
        ++calls;
        loop.quit();
    }};
    LoopCleanup cleanup(loop, &context, CleanupContext::invoke);
    loop.request_cleanup();
    loop.request_cleanup();
    EXPECT_EQ(calls, 0);
    loop.loop();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(loop.get_timeout(), -1);
}

TEST(LoopCleanupTest, AfterSocketAndWorkCallbacks)
{
    snet::EventLoop loop;
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Channel channel(fd.get());
    std::vector<int> order;
    order.reserve(3);
    snet::detail::LoopWork work(loop, [&] {
        order.push_back(2);
        loop.quit();
    });
    CleanupContext context{[&](CleanupReason reason) {
        EXPECT_EQ(reason, CleanupReason::normal);
        order.push_back(3);
    }};
    LoopCleanup cleanup(loop, &context, CleanupContext::invoke);
    channel.set_events(EPOLLIN);
    channel.set_read_callback([&] {
        uint64_t value;
        EXPECT_EQ(read(fd.get(), &value, sizeof(value)), sizeof(value));
        order.push_back(1);
        work.schedule();
    });
    loop.update_channel(&channel);
    loop.loop();
    loop.remove_channel(&channel);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(LoopCleanupTest, DestroyedRegistrationIsSkipped)
{
    snet::EventLoop loop;
    int removed_calls = 0, remaining_calls = 0;
    CleanupContext removed{[&](CleanupReason) { ++removed_calls; }};
    CleanupContext remaining{[&](CleanupReason) { ++remaining_calls; }};
    LoopCleanup survivor(loop, &remaining, CleanupContext::invoke);
    auto registration = std::make_unique<LoopCleanup>(loop, &removed, CleanupContext::invoke);
    registration.reset();
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(removed_calls, 0);
    EXPECT_EQ(remaining_calls, 1);
}

TEST(LoopCleanupTest, RequestDuringCleanupSurvives)
{
    snet::EventLoop loop;
    int calls = 0;
    CleanupContext context{[&](CleanupReason) {
        ++calls;
        if (calls == 1)
            loop.request_cleanup();
        loop.quit();
    }};
    LoopCleanup cleanup(loop, &context, CleanupContext::invoke);
    loop.request_cleanup();
    loop.loop();
    EXPECT_EQ(calls, 1);
    loop.loop();
    EXPECT_EQ(calls, 2);
}

TEST(LoopCleanupTest, NullActionRejected)
{
    snet::EventLoop loop;
    EXPECT_THROW((LoopCleanup(loop, nullptr, nullptr)), std::invalid_argument);
    int calls = 0;
    CleanupContext context{[&](CleanupReason) { ++calls; }};
    LoopCleanup cleanup(loop, &context, CleanupContext::invoke);
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 1);
}

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

namespace {
struct Probe {
    snet::EventLoop& loop;
    bool& destroyed;
    int fd;
    snet::Channel channel;
    snet::detail::LoopWork notification;
    bool inside = false;
    Probe(snet::EventLoop& l, bool& d, std::function<void()> action)
        : loop(l), destroyed(d), fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC)),
          channel(fd), notification(l, std::move(action)) {}
    ~Probe() noexcept { destroyed = true; if (fd != -1) close(fd); }
};
bool can_destroy_probe(const Probe& p) noexcept {
    return !p.inside && !p.notification.pending() && !p.notification.executing();
}
void cancel_probe(Probe& p) noexcept { p.notification.cancel(); }
}

TEST(RetirementTest, CallbackOwnerSurvives)
{
    snet::EventLoop loop;
    bool destroyed = false;
    auto owner = std::make_unique<Probe>(loop, destroyed, [] {});
    auto slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    auto* raw = owner.get();
    raw->channel.set_events(EPOLLIN);
    raw->channel.set_read_callback([&] {
        raw->inside = true;
        loop.remove_channel(&raw->channel);
        loop.retire(std::move(slot), std::move(owner));
        EXPECT_FALSE(destroyed);
        raw->inside = false;
        loop.quit();
    });
    loop.update_channel(&raw->channel);
    loop.loop();
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(owner.get(), nullptr);
}

TEST(RetirementTest, WaitsForPendingNotification)
{
    snet::EventLoop loop;
    loop.set_work_budget(1);
    bool destroyed = false;
    int notifications = 0;
    auto owner = std::make_unique<Probe>(loop, destroyed, [&] {
        EXPECT_FALSE(destroyed); ++notifications; loop.quit();
    });
    auto slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    snet::detail::LoopWork kickoff(loop, [&] {
        owner->notification.schedule();
        loop.retire(std::move(slot), std::move(owner));
        loop.quit();
    });
    kickoff.schedule(); loop.loop();
    EXPECT_FALSE(destroyed);
    loop.loop();
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(notifications, 1);
}

TEST(RetirementTest, UnregistersBeforeFdReuse)
{
    snet::EventLoop loop;
    bool first_destroyed = false, second_destroyed = false;
    auto first = std::make_unique<Probe>(loop, first_destroyed, [] {});
    auto second = std::make_unique<Probe>(loop, second_destroyed, [] {});
    auto first_slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    auto second_slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    int stale_calls = 0, new_calls = 0, reused_fd = -1;
    bool handled = false;
    std::unique_ptr<snet::Channel> replacement;
    auto handle = [&](Probe* current) {
        if (handled) { ++stale_calls; return; }
        handled = true;
        Probe* victim = current == first.get() ? second.get() : first.get();
        loop.remove_channel(&current->channel);
        loop.remove_channel(&victim->channel);
        reused_fd = victim->fd;
        close(victim->fd); victim->fd = -1;
        const int source = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
        ASSERT_GE(source, 0);
        if (source != reused_fd) { ASSERT_EQ(dup2(source, reused_fd), reused_fd); close(source); }
        replacement = std::make_unique<snet::Channel>(reused_fd);
        replacement->set_events(EPOLLIN);
        replacement->set_read_callback([&] { ++new_calls; loop.quit(); });
        loop.update_channel(replacement.get());
        loop.retire(std::move(first_slot), std::move(first));
        loop.retire(std::move(second_slot), std::move(second));
        loop.quit();
    };
    auto* a = first.get(); auto* b = second.get();
    a->channel.set_events(EPOLLIN); b->channel.set_events(EPOLLIN);
    a->channel.set_read_callback([&] { handle(a); });
    b->channel.set_read_callback([&] { handle(b); });
    loop.update_channel(&a->channel); loop.update_channel(&b->channel);
    loop.loop();
    EXPECT_TRUE(first_destroyed); EXPECT_TRUE(second_destroyed);
    EXPECT_EQ(stale_calls, 0); EXPECT_EQ(new_calls, 0);
    loop.loop();
    EXPECT_EQ(new_calls, 1);
    loop.remove_channel(replacement.get()); close(reused_fd);
}

TEST(LoopWorkTest, ExceptionPreservesLiveWork)
{
    snet::EventLoop loop;
    int throwing_calls = 0, other_live_calls = 0;
    snet::detail::LoopWork throwing(loop, [&] { ++throwing_calls; throw std::runtime_error("original"); });
    snet::detail::LoopWork other(loop, [&] { ++other_live_calls; loop.quit(); });
    throwing.schedule(); other.schedule();
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_FALSE(throwing.executing());
    EXPECT_TRUE(other.pending());
    loop.loop();
    EXPECT_EQ(throwing_calls, 1); EXPECT_EQ(other_live_calls, 1);
}

TEST(RetirementTest, ExceptionCancelsNotification)
{
    snet::EventLoop loop;
    bool destroyed = false;
    int notifications = 0;
    auto owner = std::make_unique<Probe>(loop, destroyed, [&] { ++notifications; });
    auto slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    snet::detail::LoopWork close_then_throw(loop, [&] {
        owner->notification.schedule();
        loop.retire(std::move(slot), std::move(owner));
        throw std::runtime_error("original");
    });
    close_then_throw.schedule();
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(notifications, 0);
}

TEST(RetirementTest, ExceptionWithSavedBatch)
{
    snet::EventLoop loop;
    bool first_destroyed = false, second_destroyed = false;
    int notifications = 0, calls = 0;
    auto first = std::make_unique<Probe>(loop, first_destroyed, [&] { ++notifications; });
    auto second = std::make_unique<Probe>(loop, second_destroyed, [&] { ++notifications; });
    auto first_slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    auto second_slot = loop.prepare_retirement<Probe>(can_destroy_probe, cancel_probe);
    auto handle = [&](Probe* current) {
        ++calls;
        loop.remove_channel(&current->channel);
        if (calls == 1) {
            current->notification.schedule();
            if (current == first.get()) loop.retire(std::move(first_slot), std::move(first));
            else loop.retire(std::move(second_slot), std::move(second));
            throw std::runtime_error("original");
        }
        loop.quit();
    };
    auto* a = first.get(); auto* b = second.get();
    a->channel.set_events(EPOLLIN); b->channel.set_events(EPOLLIN);
    a->channel.set_read_callback([&] { handle(a); });
    b->channel.set_read_callback([&] { handle(b); });
    loop.update_channel(&a->channel); loop.update_channel(&b->channel);
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_NE(first_destroyed, second_destroyed);
    EXPECT_EQ(notifications, 0);
    loop.loop();
    EXPECT_EQ(calls, 2);
    if (first) loop.remove_channel(&first->channel);
    if (second) loop.remove_channel(&second->channel);
}

TEST(RetirementTest, LoopDestructionSkipsActions)
{
    bool destroyed = false;
    int actions = 0, readiness_calls = 0;
    struct Owned {
        bool& destroyed;
        int& readiness_calls;
        snet::detail::LoopWork work;
        Owned(snet::EventLoop& loop, bool& d, int& r, int& a)
            : destroyed(d), readiness_calls(r), work(loop, [&a] { ++a; }) {}
        ~Owned() noexcept { destroyed = true; }
    };
    {
        snet::EventLoop loop;
        auto owner = std::make_unique<Owned>(loop, destroyed, readiness_calls, actions);
        auto slot = loop.prepare_retirement<Owned>(
            [](const Owned& o) noexcept { ++o.readiness_calls; return false; },
            [](Owned& o) noexcept { o.work.cancel(); });
        owner->work.schedule();
        loop.retire(std::move(slot), std::move(owner));
    }
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(actions, 0);
    EXPECT_EQ(readiness_calls, 0);
}

TEST(RetirementTest, InvalidPreparationPreservesOwner)
{
    snet::EventLoop loop;
    bool destroyed = false;
    auto owner = std::make_unique<Probe>(loop, destroyed, [] {});
    EXPECT_THROW(loop.prepare_retirement<Probe>(nullptr, cancel_probe), std::invalid_argument);
    EXPECT_THROW(loop.prepare_retirement<Probe>(can_destroy_probe, nullptr), std::invalid_argument);
    EXPECT_NE(owner.get(), nullptr);
    EXPECT_FALSE(destroyed);
}

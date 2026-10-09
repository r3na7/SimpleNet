#include <simplenet/EventLoop.hpp>
#include <simplenet/detail/LoopCleanup.hpp>

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>
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

    ~Fd() { close_now(); }

    void close_now() noexcept
    {
        if (fd_ >= 0)
            close(std::exchange(fd_, -1));
    }

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

namespace
{
struct Probe {
    bool &destroyed;
    Fd fd;
    snet::Channel channel;
    snet::detail::LoopWork notification;
    bool registered = false;
    bool closed = false;
    bool inside_callback = false;

    Probe(snet::EventLoop &loop, bool &d, std::function<void()> action)
        : destroyed(d), fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC)), channel(fd.get()),
          notification(loop, std::move(action))
    {
    }

    ~Probe() noexcept { destroyed = true; }
};

struct CallbackGuard {
    bool &inside;

    explicit CallbackGuard(bool &flag) : inside(flag) { inside = true; }

    ~CallbackGuard() { inside = false; }
};

struct OwnerFixture {
    snet::EventLoop &loop;
    std::array<std::unique_ptr<Probe>, 2> objects;
    int cleanup_calls = 0;
    CleanupReason last_reason = CleanupReason::normal;
    LoopCleanup registration;

    explicit OwnerFixture(snet::EventLoop &l) : loop(l), registration(l, this, collect) {}

    ~OwnerFixture() noexcept
    {
        for (auto &object : objects) {
            if (object && object->registered) {
                try {
                    loop.remove_channel(&object->channel);
                } catch (...) {
                    std::terminate();
                }
            }

            object.reset();
        }
    }

    void activate(Probe &p)
    {
        p.channel.set_events(EPOLLIN);
        loop.update_channel(&p.channel);
        p.registered = true;
    }

    void close(Probe &p)
    {
        if (p.registered) {
            loop.remove_channel(&p.channel);
            p.registered = false;
        }

        p.fd.close_now();
        p.closed = true;
        loop.request_cleanup();
    }

    static void collect(void *context, CleanupReason reason) noexcept
    {
        auto &owner = *static_cast<OwnerFixture *>(context);

        ++owner.cleanup_calls;
        owner.last_reason = reason;

        for (auto &object : owner.objects) {
            if (!object || !object->closed)
                continue;

            if (reason == CleanupReason::exception)
                object->notification.cancel();

            if (!object->inside_callback && !object->notification.pending() && !object->notification.executing())
                object.reset();
        }
    }
};
} // namespace

TEST(OwnerCleanupTest, CallbackOwnerSurvives)
{
    snet::EventLoop loop;
    bool destroyed = false;
    OwnerFixture owner(loop);

    owner.objects[0] = std::make_unique<Probe>(loop, destroyed, [] {});
    auto *raw = owner.objects[0].get();

    raw->channel.set_read_callback([&] {
        CallbackGuard guard(raw->inside_callback);

        owner.close(*raw);

        EXPECT_EQ(owner.objects[0].get(), raw);
        EXPECT_FALSE(destroyed);
        loop.quit();
    });
    owner.activate(*raw);
    loop.loop();

    EXPECT_TRUE(destroyed);
    EXPECT_EQ(owner.objects[0].get(), nullptr);
}

TEST(OwnerCleanupTest, WaitsForPendingNotification)
{
    snet::EventLoop loop;

    loop.set_work_budget(1);
    bool destroyed = false;
    int notifications = 0;
    OwnerFixture owner(loop);

    owner.objects[0] = std::make_unique<Probe>(loop, destroyed, [&] {
        EXPECT_FALSE(destroyed);
        ++notifications;
        loop.quit();
    });
    snet::detail::LoopWork close(loop, [&] {
        owner.objects[0]->notification.schedule();
        owner.close(*owner.objects[0]);
        loop.quit();
    });
    close.schedule();
    loop.loop();

    EXPECT_FALSE(destroyed);
    ASSERT_NE(owner.objects[0].get(), nullptr);
    loop.loop();

    EXPECT_TRUE(destroyed);
    EXPECT_EQ(notifications, 1);
}

TEST(OwnerCleanupTest, ExceptionCancelsClosedWorkPreservesLiveWork)
{
    snet::EventLoop loop;
    bool closed_destroyed = false, live_destroyed = false;
    int closed_actions = 0, live_actions = 0;
    OwnerFixture owner(loop);

    owner.objects[0] = std::make_unique<Probe>(loop, closed_destroyed, [&] { ++closed_actions; });
    owner.objects[1] = std::make_unique<Probe>(loop, live_destroyed, [&] {
        ++live_actions;
        loop.quit();
    });
    snet::detail::LoopWork close_then_throw(loop, [&] {
        owner.objects[0]->notification.schedule();
        owner.objects[1]->notification.schedule();
        owner.close(*owner.objects[0]);
        throw std::runtime_error("original");
    });
    close_then_throw.schedule();

    try {
        loop.loop();
        FAIL() << "Expected original exception";
    } catch (const std::runtime_error &error) {
        EXPECT_STREQ(error.what(), "original");
    }

    EXPECT_TRUE(closed_destroyed);
    EXPECT_FALSE(live_destroyed);
    EXPECT_EQ(owner.last_reason, CleanupReason::exception);
    EXPECT_EQ(closed_actions, 0);
    ASSERT_NE(owner.objects[1].get(), nullptr);
    EXPECT_TRUE(owner.objects[1]->notification.pending());
    loop.loop();

    EXPECT_EQ(closed_actions, 0);
    EXPECT_EQ(live_actions, 1);
}

TEST(OwnerCleanupTest, ExceptionWithSavedBatch)
{
    snet::EventLoop loop;
    std::array<bool, 2> destroyed{false, false};
    int calls = 0, closed_actions = 0;
    OwnerFixture owner(loop);

    for (int i = 0; i < 2; ++i)
        owner.objects[i] = std::make_unique<Probe>(loop, destroyed[i], [&] { ++closed_actions; });

    auto handle = [&](Probe *current) {
        ++calls;
        CallbackGuard guard(current->inside_callback);

        for (auto &object : owner.objects) {
            object->notification.schedule();
            owner.close(*object);
        }

        throw std::runtime_error("original");
    };

    for (auto &object : owner.objects) {
        auto *raw = object.get();

        raw->channel.set_read_callback([&, raw] { handle(raw); });
        owner.activate(*raw);
    }

    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_TRUE(destroyed[0]);
    EXPECT_TRUE(destroyed[1]);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(closed_actions, 0);
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });

    stop.schedule();
    loop.loop();

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(closed_actions, 0);
}

TEST(OwnerCleanupTest, NoHandlerStillDeletesClosedObject)
{
    snet::EventLoop loop;
    bool destroyed = false;
    OwnerFixture owner(loop);

    owner.objects[0] = std::make_unique<Probe>(loop, destroyed, [] {});
    LoopCleanup stop(loop, &loop,
                     [](void *context, CleanupReason) noexcept { static_cast<snet::EventLoop *>(context)->quit(); });

    owner.close(*owner.objects[0]);

    EXPECT_FALSE(destroyed);
    loop.loop();

    EXPECT_TRUE(destroyed);
    EXPECT_EQ(owner.objects[0].get(), nullptr);
}

TEST(LoopCleanupTest, RegistrationDuringCleanupRejected)
{
    snet::EventLoop loop;
    int calls = 0, rejected = 0;
    CleanupContext context{[&](CleanupReason) {
        ++calls;

        try {
            LoopCleanup invalid(loop, nullptr, [](void *, CleanupReason) noexcept {});
        } catch (const std::logic_error &) {
            ++rejected;
        }

        if (calls == 1)
            loop.request_cleanup();

        loop.quit();
    }};
    LoopCleanup cleanup(loop, &context, CleanupContext::invoke);

    loop.request_cleanup();
    loop.loop();
    loop.loop();

    EXPECT_EQ(calls, 2);
    EXPECT_EQ(rejected, 2);
}

TEST(LoopCleanupTest, DoesNotRunOnLoopDestruction)
{
    int calls = 0;
    auto loop = std::make_unique<snet::EventLoop>();
    CleanupContext context{[&](CleanupReason) { ++calls; }};

    {
        LoopCleanup cleanup(*loop, &context, CleanupContext::invoke);

        loop->request_cleanup();
    }

    loop.reset();

    EXPECT_EQ(calls, 0);
}

TEST(OwnerCleanupTest, OtherOwnerRemainsAliveAfterException)
{
    snet::EventLoop loop;
    bool closed_destroyed = false, live_destroyed = false;
    int live_calls = 0;
    OwnerFixture first(loop), second(loop);

    first.objects[0] = std::make_unique<Probe>(loop, closed_destroyed, [] {});
    second.objects[0] = std::make_unique<Probe>(loop, live_destroyed, [&] {
        ++live_calls;
        loop.quit();
    });
    snet::detail::LoopWork close_then_throw(loop, [&] {
        first.close(*first.objects[0]);
        second.objects[0]->notification.schedule();
        throw std::runtime_error("original");
    });
    close_then_throw.schedule();

    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_TRUE(closed_destroyed);
    EXPECT_FALSE(live_destroyed);
    EXPECT_EQ(first.last_reason, CleanupReason::exception);
    EXPECT_EQ(second.last_reason, CleanupReason::exception);
    ASSERT_NE(second.objects[0].get(), nullptr);
    EXPECT_TRUE(second.objects[0]->notification.pending());
    loop.loop();

    EXPECT_EQ(live_calls, 1);
}

TEST(OwnerCleanupTest, UnregistersBeforeFdReuse)
{
    snet::EventLoop loop;
    std::array<bool, 2> destroyed{false, false};
    OwnerFixture owner(loop);

    for (int i = 0; i < 2; ++i)
        owner.objects[i] = std::make_unique<Probe>(loop, destroyed[i], [] {});

    bool handled = false;
    int stale_calls = 0, new_calls = 0;
    std::unique_ptr<Fd> replacement_fd;
    std::unique_ptr<snet::Channel> replacement;
    auto handle = [&](Probe *current) {
        if (handled) {
            ++stale_calls;
            return;
        }

        handled = true;
        Probe *victim = current == owner.objects[0].get() ? owner.objects[1].get() : owner.objects[0].get();
        const int reused_fd = victim->fd.get();

        owner.close(*current);
        owner.close(*victim);
        const int source = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);

        ASSERT_GE(source, 0);

        if (source != reused_fd) {
            ASSERT_EQ(dup2(source, reused_fd), reused_fd);
            close(source);
        }

        replacement_fd = std::make_unique<Fd>(reused_fd);
        replacement = std::make_unique<snet::Channel>(reused_fd);
        replacement->set_events(EPOLLIN);
        replacement->set_read_callback([&] {
            ++new_calls;
            loop.quit();
        });
        loop.update_channel(replacement.get());
        loop.quit();
    };

    for (auto &object : owner.objects) {
        auto *raw = object.get();

        raw->channel.set_read_callback([&, raw] { handle(raw); });
        owner.activate(*raw);
    }

    loop.loop();

    EXPECT_TRUE(destroyed[0]);
    EXPECT_TRUE(destroyed[1]);
    EXPECT_EQ(stale_calls, 0);
    EXPECT_EQ(new_calls, 0);
    loop.loop();

    EXPECT_EQ(new_calls, 1);
    loop.remove_channel(replacement.get());
}

TEST(LoopCleanupTest, NullContextSupported)
{
    snet::EventLoop loop;
    static int calls = 0;

    calls = 0;
    LoopCleanup cleanup(loop, nullptr, [](void *context, CleanupReason reason) noexcept {
        EXPECT_EQ(context, nullptr);
        EXPECT_EQ(reason, CleanupReason::normal);
        ++calls;
    });
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });

    stop.schedule();
    loop.loop();

    EXPECT_EQ(calls, 1);
}

TEST(LoopCleanupTest, UnlinksHeadMiddleAndTail)
{
    snet::EventLoop loop;
    std::array<int, 4> calls{};
    std::array<CleanupContext, 4> contexts;
    std::array<std::unique_ptr<LoopCleanup>, 4> registrations;

    for (int i = 0; i < 4; ++i) {
        contexts[i].action = [&, i](CleanupReason) { ++calls[i]; };
        registrations[i] = std::make_unique<LoopCleanup>(loop, &contexts[i], CleanupContext::invoke);
    }

    registrations[2].reset(); // middle
    registrations[0].reset(); // tail
    registrations[3].reset(); // head
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });

    stop.schedule();
    loop.loop();

    EXPECT_EQ(calls, (std::array<int, 4>{0, 1, 0, 0}));
}

TEST(LoopCleanupTest, ReentryDuringCleanupRejected)
{
    snet::EventLoop loop;
    int rejected = 0;
    CleanupContext context{[&](CleanupReason) {
        try {
            loop.loop();
        } catch (const std::logic_error &) {
            ++rejected;
        }

        loop.quit();
    }};
    LoopCleanup cleanup(loop, &context, CleanupContext::invoke);

    loop.request_cleanup();
    loop.loop();

    EXPECT_EQ(rejected, 1);
}

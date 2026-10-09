#include <simplenet/Simplenet.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{

class Fd
{
public:
    explicit Fd(int value) : value_(value)
    {
        if (value == -1)
            throw std::system_error(errno, std::system_category(), "test fd creation");
    }

    ~Fd()
    {
        if (value_ != -1)
            close(value_);
    }

    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const { return value_; }

    void close_now() { close(std::exchange(value_, -1)); }

private:
    int value_;
};

struct SocketPair {
    int values[2];

    SocketPair()
    {
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, values) == -1)
            throw std::system_error(errno, std::system_category(), "socketpair");
    }

    ~SocketPair()
    {
        close(values[0]);
        close(values[1]);
    }

    SocketPair(const SocketPair &) = delete;
    SocketPair &operator=(const SocketPair &) = delete;
};

void consume(int fd)
{
    uint64_t value = 0;

    ASSERT_EQ(read(fd, &value, sizeof(value)), sizeof(value));
}

TEST(PollerTest, CapacityValidation)
{
    EXPECT_THROW(([] { snet::Poller poller(0); })(), std::invalid_argument);
    EXPECT_THROW(([] { snet::Poller poller(-1); })(), std::invalid_argument);
    snet::Poller poller(2);

    EXPECT_THROW(([&] { poller.set_max_events(0); })(), std::invalid_argument);
    ASSERT_EQ(poller.get_max_events(), 2);
    poller.set_timeout(0);

    ASSERT_TRUE(poller.poll().empty());
}

TEST(PollerTest, AddFailureRollback)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLEXCLUSIVE | EPOLLONESHOT | EPOLLIN);

    EXPECT_THROW(([&] { poller.update_channel(&channel); })(), std::system_error);
    channel.set_events(EPOLLIN);
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    poller.remove_channel(&channel);
}

TEST(PollerTest, ModifyFailurePreservesRegistration)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    poller.update_channel(&channel);
    // EPOLLEXCLUSIVE is not accepted by EPOLL_CTL_MOD.
    channel.set_events(EPOLLEXCLUSIVE | EPOLLIN);

    EXPECT_THROW(([&] { poller.update_channel(&channel); })(), std::system_error);
    ASSERT_EQ(poller.poll().size(), 1);
    ASSERT_NE(channel.get_revents() & EPOLLIN, 0u);
    channel.set_events(EPOLLIN);
    poller.update_channel(&channel);
    poller.remove_channel(&channel);
}

TEST(PollerTest, DuplicateFdRejected)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel owner(fd.get()), impostor(fd.get());

    owner.set_events(EPOLLIN);
    poller.update_channel(&owner);

    EXPECT_THROW(([&] { poller.update_channel(&impostor); })(), std::logic_error);
    EXPECT_THROW(([&] { poller.remove_channel(&impostor); })(), std::logic_error);
    const auto &batch = poller.poll();

    ASSERT_EQ(batch.size(), 1);
    ASSERT_EQ(batch[0], &owner);
    poller.remove_channel(&owner);
}

TEST(PollerTest, MaskUpdateChangesReadiness)
{
    Fd fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    poller.update_channel(&channel);

    ASSERT_TRUE(poller.poll().empty());
    channel.set_events(EPOLLOUT);
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    ASSERT_NE(channel.get_revents() & EPOLLOUT, 0u);
    channel.clear_events();
    poller.update_channel(&channel);

    ASSERT_TRUE(poller.poll().empty());
    poller.remove_channel(&channel);
}

TEST(PollerTest, RemovePendingChannel)
{
    Fd a(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC)), b(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller(2);

    poller.set_timeout(0);
    auto first = std::make_unique<snet::Channel>(a.get());
    auto second = std::make_unique<snet::Channel>(b.get());

    first->set_events(EPOLLIN);
    second->set_events(EPOLLIN);
    poller.update_channel(first.get());
    poller.update_channel(second.get());
    const auto &batch = poller.poll();

    ASSERT_EQ(batch.size(), 2);
    // The test does not assume epoll returns channels in registration order.
    auto *pending = batch[1];

    batch[0]->set_read_callback([&] {
        poller.remove_channel(pending);

        if (pending == first.get())
            first.reset();
        else
            second.reset();
    });
    batch[0]->handle_event();

    ASSERT_EQ(batch[1], nullptr);
    poller.remove_channel(batch[0]);
}

TEST(ChannelTest, RemoveCancelsRemainingCallbacks)
{
    SocketPair sockets;

    ASSERT_EQ(write(sockets.values[1], "x", 1), 1);
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(sockets.values[0]);

    channel.set_events(EPOLLIN | EPOLLOUT);
    int writes = 0;

    channel.set_read_callback([&] { poller.remove_channel(&channel); });
    channel.set_write_callback([&] { ++writes; });
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    ASSERT_EQ((channel.get_revents() & (EPOLLIN | EPOLLOUT)), (EPOLLIN | EPOLLOUT));
    channel.handle_event();

    ASSERT_EQ(writes, 0);
}

TEST(ChannelTest, ReentryRejected)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    int calls = 0;

    channel.set_read_callback([&] {
        ++calls;

        EXPECT_THROW(([&] { channel.handle_event(); })(), std::logic_error);
    });
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    channel.handle_event();

    ASSERT_EQ(calls, 1);
    poller.remove_channel(&channel);
}

TEST(ChannelTest, MaskChangeDoesNotCancelDispatch)
{
    SocketPair sockets;

    ASSERT_EQ(write(sockets.values[1], "x", 1), 1);
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(sockets.values[0]);

    channel.set_events(EPOLLIN | EPOLLOUT);
    std::string calls;

    channel.set_read_callback([&] {
        calls += 'R';
        channel.clear_events();
        poller.update_channel(&channel);
    });
    channel.set_write_callback([&] { calls += 'W'; });
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    ASSERT_EQ((channel.get_revents() & (EPOLLIN | EPOLLOUT)), (EPOLLIN | EPOLLOUT));
    channel.handle_event();

    ASSERT_EQ(calls, "RW");
    poller.remove_channel(&channel);
}

TEST(ChannelTest, CallbackCanReplaceLaterHandler)
{
    SocketPair sockets;

    ASSERT_EQ(write(sockets.values[1], "x", 1), 1);
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(sockets.values[0]);

    channel.set_events(EPOLLIN | EPOLLOUT);
    int original = 0, replacement = 0;

    channel.set_write_callback([&] { ++original; });
    channel.set_read_callback([&] { channel.set_write_callback([&] { ++replacement; }); });
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    channel.handle_event();

    ASSERT_EQ(original, 0);
    ASSERT_EQ(replacement, 1);
    poller.remove_channel(&channel);
}

TEST(ChannelTest, CallbackSelfReplacementRejected)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    int calls = 0;

    channel.set_read_callback([&] {
        ++calls;

        EXPECT_THROW(([&] { channel.set_read_callback({}); })(), std::logic_error);
    });
    poller.update_channel(&channel);

    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(poller.poll().size(), 1);
        channel.handle_event();
    }

    ASSERT_EQ(calls, 2);
    channel.set_read_callback({});
    poller.remove_channel(&channel);
}

TEST(ChannelTest, CallbackExceptionResetsGuards)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    channel.set_read_callback([] { throw std::runtime_error("callback failure"); });
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    EXPECT_THROW(([&] { channel.handle_event(); })(), std::runtime_error);
    int calls = 0;

    channel.set_read_callback([&] { ++calls; });

    ASSERT_EQ(poller.poll().size(), 1);
    channel.handle_event();

    ASSERT_EQ(calls, 1);
    poller.remove_channel(&channel);
}

TEST(ChannelTest, HangupWithoutInputDispatchesRead)
{
    int values[2];

    ASSERT_EQ(pipe2(values, O_NONBLOCK | O_CLOEXEC), 0);
    Fd input(values[0]), output(values[1]);
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(input.get());

    channel.set_events(EPOLLIN);
    int calls = 0;

    channel.set_read_callback([&] {
        char byte;

        ASSERT_EQ(read(input.get(), &byte, 1), 0);
        ++calls;
    });
    poller.update_channel(&channel);
    output.close_now();

    ASSERT_EQ(poller.poll().size(), 1);
    ASSERT_NE(channel.get_revents() & EPOLLHUP, 0u);
    ASSERT_EQ(channel.get_revents() & EPOLLIN, 0u);
    channel.handle_event();

    ASSERT_EQ(calls, 1);
    poller.remove_channel(&channel);
}

TEST(ChannelTest, HalfClosePreservesUnreadData)
{
    SocketPair sockets;

    ASSERT_EQ(write(sockets.values[1], "abc", 3), 3);
    ASSERT_EQ(shutdown(sockets.values[1], SHUT_WR), 0);
    snet::Poller poller;

    poller.set_timeout(0);
    snet::Channel channel(sockets.values[0]);

    channel.set_events(EPOLLIN | EPOLLRDHUP);
    int calls = 0;

    channel.set_read_callback([&] {
        char data[3];

        ASSERT_EQ(read(sockets.values[0], data, sizeof(data)), 3);
        ASSERT_EQ(std::string_view(data, sizeof(data)), "abc");
        ASSERT_EQ(read(sockets.values[0], data, sizeof(data)), 0);
        ++calls;
    });
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    ASSERT_NE(channel.get_revents() & EPOLLRDHUP, 0u);
    channel.handle_event();

    ASSERT_EQ(calls, 1);
    poller.remove_channel(&channel);
}

TEST(EventLoopTest, LoopReentryRejected)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::EventLoop loop;
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    int calls = 0;

    channel.set_read_callback([&] {
        EXPECT_THROW(([&] { loop.loop(); })(), std::logic_error);
        ++calls;
        loop.quit();
    });
    loop.update_channel(&channel);
    loop.loop();

    ASSERT_EQ(calls, 1);
    loop.remove_channel(&channel);
}

TEST(EventLoopTest, QuitFinishesCurrentBatch)
{
    Fd a(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC)), b(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::EventLoop loop;

    loop.set_max_events(2);
    snet::Channel first(a.get()), second(b.get());

    first.set_events(EPOLLIN);
    second.set_events(EPOLLIN);
    int calls = 0;
    auto callback = [&] {
        ++calls;
        loop.quit();
    };

    first.set_read_callback(callback);
    second.set_read_callback(callback);
    loop.update_channel(&first);
    loop.update_channel(&second);
    loop.loop();

    ASSERT_EQ(calls, 2);
    loop.remove_channel(&first);
    loop.remove_channel(&second);
}

TEST(EventLoopTest, ExceptionResumesEdgeTriggeredBatch)
{
    Fd a(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC)), b(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::EventLoop loop;

    loop.set_max_events(2);
    snet::Channel first(a.get()), second(b.get());

    first.set_events(EPOLLIN | EPOLLET);
    second.set_events(EPOLLIN | EPOLLET);
    int first_calls = 0, second_calls = 0, total = 0;
    auto callback = [&](int fd, int &calls) {
        consume(fd);
        ++calls;

        if (++total == 1)
            throw std::runtime_error("first callback failure");

        loop.quit();
    };

    first.set_read_callback([&] { callback(a.get(), first_calls); });
    second.set_read_callback([&] { callback(b.get(), second_calls); });
    loop.update_channel(&first);
    loop.update_channel(&second);

    EXPECT_THROW(([&] { loop.loop(); })(), std::runtime_error);
    ASSERT_EQ(total, 1);
    // A new epoll_wait cannot recover the already delivered EPOLLET event.
    loop.loop();

    ASSERT_EQ(total, 2);
    ASSERT_EQ(first_calls, 1);
    ASSERT_EQ(second_calls, 1);
    loop.remove_channel(&first);
    loop.remove_channel(&second);
}

TEST(EventLoopTest, RemovalBetweenRunsSkipsSavedChannel)
{
    Fd a(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC)), b(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    Fd wake(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::EventLoop loop;

    loop.set_max_events(2);
    auto first = std::make_unique<snet::Channel>(a.get());
    auto second = std::make_unique<snet::Channel>(b.get());
    snet::Channel stopper(wake.get());

    stopper.set_events(EPOLLIN);
    snet::Channel *throwing = nullptr;
    int calls = 0;
    auto callback = [&](snet::Channel *channel) {
        ++calls;
        throwing = channel;
        throw std::runtime_error("stop");
    };

    first->set_events(EPOLLIN | EPOLLET);
    second->set_events(EPOLLIN | EPOLLET);
    first->set_read_callback([&] { callback(first.get()); });
    second->set_read_callback([&] { callback(second.get()); });
    loop.update_channel(first.get());
    loop.update_channel(second.get());

    EXPECT_THROW(([&] { loop.loop(); })(), std::runtime_error);
    ASSERT_EQ(calls, 1);
    auto &pending = throwing == first.get() ? second : first;

    loop.remove_channel(pending.get());
    // Keep it alive so an erroneous dispatch fails deterministically, without UAF.
    loop.remove_channel(throwing);
    int stops = 0;

    stopper.set_read_callback([&] {
        ++stops;
        loop.quit();
    });
    loop.update_channel(&stopper);
    loop.loop();

    ASSERT_EQ(calls, 1);
    ASSERT_EQ(stops, 1);
    loop.remove_channel(&stopper);
}

volatile sig_atomic_t signal_received = 0;
void on_alarm(int) { signal_received = 1; }

// Every CTest scenario has its own process; restore signal state even on failure.
class Alarm
{
public:
    Alarm()
    {
        struct sigaction action {
        };

        action.sa_handler = on_alarm;
        sigemptyset(&action.sa_mask);

        if (sigaction(SIGALRM, &action, &previous_) == -1)
            throw std::system_error(errno, std::system_category(), "sigaction");

        itimerval timer{};
        timer.it_value.tv_usec = 10000;
        timer.it_interval.tv_usec = 10000;

        if (setitimer(ITIMER_REAL, &timer, &previous_timer_) == -1) {
            const int error = errno;

            sigaction(SIGALRM, &previous_, nullptr);
            throw std::system_error(error, std::system_category(), "setitimer");
        }
    }

    ~Alarm()
    {
        setitimer(ITIMER_REAL, &previous_timer_, nullptr);
        sigaction(SIGALRM, &previous_, nullptr);
    }

private:
    struct sigaction previous_ {
    };

    itimerval previous_timer_{};
};

TEST(PollerTest, InterruptedWaitReturnsEmptyBatch)
{
    Fd fd(eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Poller poller;
    snet::Channel channel(fd.get());

    channel.set_events(EPOLLIN);
    poller.update_channel(&channel);

    ASSERT_EQ(poller.poll().size(), 1);
    consume(fd.get());
    signal_received = 0;
    {
        Alarm alarm;

        ASSERT_TRUE(poller.poll().empty());
        ASSERT_EQ(signal_received, 1);
    }

    poller.remove_channel(&channel);
}

} // namespace

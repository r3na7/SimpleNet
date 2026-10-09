#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>

extern "C" int __real_epoll_wait(int, epoll_event *, int, int);
extern "C" int __real_getsockopt(int, int, int, void *, socklen_t *);
extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int selected = -1, adds = 0, dels = 0, attempts = 0, next_accept_error = 0;
bool fail_add = false, registered = false, fail_del = false;
int interrupts = 0, so_error = -1, so_failure = 0, selected_epoll = -1;
std::uint32_t injected_event = 0;
void *selected_pointer = nullptr, *second_pointer = nullptr;
int second_fd = -1;
struct Intercept {
    explicit Intercept(int fd)
    {
        selected = fd;
        adds = dels = attempts = 0;
        next_accept_error = 0;
        fail_add = registered = fail_del = false;
        interrupts = so_failure = 0;
        so_error = selected_epoll = -1;
        injected_event = 0;
        selected_pointer = second_pointer = nullptr;
        second_fd = -1;
    }

    ~Intercept()
    {
        selected = -1;
        fail_add = false;
    }
};
} // namespace

extern "C" int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event)
{
    if (fd == selected && op == EPOLL_CTL_DEL && fail_del) {
        errno = EIO;
        return -1;
    }

    if (fd == selected && op == EPOLL_CTL_ADD && fail_add) {
        fail_add = false;
        errno = ENOMEM;
        return -1;
    }

    int result = __real_epoll_ctl(epoll, op, fd, event);

    if (result == 0 && op == EPOLL_CTL_ADD && fd == second_fd)
        second_pointer = event->data.ptr;

    if (fd == selected && result == 0) {
        if (op == EPOLL_CTL_ADD) {
            ++adds;
            registered = true;
            selected_pointer = event->data.ptr;
            selected_epoll = epoll;
        }

        if (op == EPOLL_CTL_DEL) {
            ++dels;
            registered = false;
        }
    }

    return result;
}

TEST(AcceptorSyscallTest, FailedStartRetainsListenerAndCallback)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    Intercept guard(fd);
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int callbacks = 0;

    acceptor.on_accept([&](snet::Socket) { ++callbacks; });
    fail_add = true;

    EXPECT_THROW(acceptor.start(), std::system_error);
    EXPECT_EQ(adds, 0);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_NO_THROW(acceptor.start());
    EXPECT_EQ(adds, 1);
    auto peer = listener.connect();

    tcp_test::drive(loop, [&] { return callbacks == 1; });
}

TEST(AcceptorSyscallTest, PreStartPauseResume)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));

    acceptor.pause_accepting();

    EXPECT_THROW(acceptor.resume_accepting(), std::logic_error);
    acceptor.on_accept([](snet::Socket) {});
    acceptor.resume_accepting();

    EXPECT_EQ(adds, 0);
    acceptor.pause_accepting();
    acceptor.start();

    EXPECT_EQ(adds, 0);
    acceptor.resume_accepting();

    EXPECT_EQ(adds, 1);
    acceptor.resume_accepting();

    EXPECT_EQ(adds, 1);
    acceptor.pause_accepting();
    acceptor.pause_accepting();

    EXPECT_EQ(dels, 1);
}

TEST(AcceptorSyscallTest, FailedResumeRemainsPaused)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));

    acceptor.on_accept([](snet::Socket) {});
    acceptor.start();
    acceptor.pause_accepting();
    fail_add = true;

    EXPECT_THROW(acceptor.resume_accepting(), std::system_error);
    EXPECT_FALSE(registered);
    accept_test::once(loop);

    EXPECT_FALSE(registered);
    EXPECT_NO_THROW(acceptor.resume_accepting());
    EXPECT_TRUE(registered);
    EXPECT_EQ(adds, 2);
}

extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags)
{
    if (fd == selected) {
        ++attempts;

        if (interrupts > 0) {
            --interrupts;
            errno = EINTR;
            return -1;
        }

        if (next_accept_error) {
            errno = std::exchange(next_accept_error, 0);
            return -1;
        }
    }

    return __real_accept4(fd, address, size, flags);
}

TEST(AcceptorSyscallTest, DefaultBudgetIs32)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.start();
    std::vector<snet::Socket> peers;

    for (int i = 0; i < 33; ++i)
        peers.push_back(listener.connect());

    accept_test::once(loop);

    EXPECT_EQ(attempts, 32);
    EXPECT_EQ(received, 32);
    tcp_test::drive(loop, [&] { return received == 33; });
}

TEST(AcceptorSyscallTest, EagainEndsGroupWithoutNotification)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0, errors = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.on_error([&](auto &, std::error_code) { ++errors; });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EAGAIN;
    accept_test::once(loop);

    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(received, 0);
    EXPECT_EQ(errors, 0);
    tcp_test::drive(loop, [&] { return received == 1; });

    EXPECT_EQ(errors, 0);
}

extern "C" int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *size)
{
    if (fd == selected && level == SOL_SOCKET && option == SO_ERROR) {
        if (so_failure) {
            errno = std::exchange(so_failure, 0);
            return -1;
        }

        if (so_error >= 0) {
            *static_cast<int *>(value) = so_error;
            *size = sizeof(int);
            return 0;
        }
    }

    return __real_getsockopt(fd, level, option, value, size);
}

extern "C" int __wrap_epoll_wait(int epoll, epoll_event *events, int maximum, int timeout)
{
    if (epoll == selected_epoll && injected_event) {
        events[0] = {};
        events[0].events = std::exchange(injected_event, 0);
        events[0].data.ptr = selected_pointer;

        if (second_pointer && maximum >= 2) {
            events[1] = {};
            events[1].events = EPOLLIN;
            events[1].data.ptr = second_pointer;
            return 2;
        }

        return 1;
    }

    return __real_epoll_wait(epoll, events, maximum, timeout);
}

class PendingErrorTest : public testing::TestWithParam<int>
{
};

TEST_P(PendingErrorTest, SkipsOneFailedConnectionAndContinues)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0, errors = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.on_error([&](auto &, std::error_code) { ++errors; });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = GetParam();
    tcp_test::drive(loop, [&] { return received == 1; });

    EXPECT_EQ(errors, 0);
    EXPECT_GE(attempts, 2);
}

INSTANTIATE_TEST_SUITE_P(Linux, PendingErrorTest,
                         testing::Values(ECONNABORTED, ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH,
                                         EOPNOTSUPP, ENETUNREACH));

class ResourceErrorTest : public testing::TestWithParam<int>
{
};

TEST_P(ResourceErrorTest, PausesBeforeNotificationAndRequiresResume)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0, errors = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.on_error([&](auto &, std::error_code reason) {
        EXPECT_FALSE(registered);
        EXPECT_EQ(reason.value(), GetParam());
        ++errors;
        errno = EBADF;
    });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = GetParam();

    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_EQ(errors, 1);
    EXPECT_EQ(received, 0);
    EXPECT_EQ(attempts, 1);
    accept_test::once(loop);

    EXPECT_EQ(attempts, 1);
    acceptor.resume_accepting();
    tcp_test::drive(loop, [&] { return received == 1; });
}

INSTANTIATE_TEST_SUITE_P(Limits, ResourceErrorTest, testing::Values(EMFILE, ENFILE, ENOMEM, ENOBUFS));

TEST(AcceptorSyscallTest, EintrConsumesBudget)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket), {2});
    int received = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.start();
    auto peer = listener.connect();

    interrupts = 3;
    accept_test::once(loop);

    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(received, 0);
    tcp_test::drive(loop, [&] { return received == 1; });

    EXPECT_EQ(attempts, 4);
}

TEST(AcceptorSyscallTest, ErrorCallbackResumesButEndsCurrentGroup)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0, errors = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.on_error([&](auto &current, std::error_code) {
        ++errors;

        EXPECT_FALSE(registered);
        current.resume_accepting();
    });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EMFILE;

    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(received, 0);
    EXPECT_EQ(errors, 1);
    tcp_test::drive(loop, [&] { return received == 1; });
}

TEST(AcceptorSyscallTest, EmptyErrorCallbackStillPauses)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));

    acceptor.on_accept([](snet::Socket) {});
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EMFILE;

    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_FALSE(registered);
    accept_test::once(loop);

    EXPECT_EQ(attempts, 1);
}

TEST(AcceptorSyscallTest, ThrowingErrorCallbackKeepsPauseAndReplacement)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int first = 0, next = 0;
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;

    acceptor.on_accept([](snet::Socket) {});
    acceptor.on_error([&, lifetime](auto &self, std::error_code) {
        ++first;
        self.on_error([&](auto &current, std::error_code) {
            ++next;
            current.on_error({});
        });

        EXPECT_FALSE(weak.expired());
        throw std::runtime_error("resource callback");
    });
    lifetime.reset();
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EMFILE;

    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_FALSE(registered);
    EXPECT_TRUE(weak.expired());
    accept_test::once(loop);

    EXPECT_EQ(attempts, 1);
    acceptor.resume_accepting();
    next_accept_error = ENFILE;

    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_EQ(first, 1);
    EXPECT_EQ(next, 1);
    acceptor.resume_accepting();
    next_accept_error = ENOMEM;

    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_EQ(next, 1);
}

TEST(AcceptorSyscallTest, ClearReceiverFromErrorRequiresNewHandler)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));

    acceptor.on_accept([](snet::Socket) {});
    acceptor.on_error([](auto &self, std::error_code) {
        self.on_accept({});

        EXPECT_THROW(self.resume_accepting(), std::logic_error);
    });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EMFILE;

    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_FALSE(registered);
    int received = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    accept_test::once(loop);

    EXPECT_EQ(attempts, 1);
    acceptor.resume_accepting();
    tcp_test::drive(loop, [&] { return received == 1; });
}

TEST(AcceptorSyscallTest, CloseFromErrorNeverAcceptsQueuedClient)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int received = 0;

    acceptor.on_accept([&](snet::Socket) { ++received; });
    acceptor.on_error([](auto &self, std::error_code) { self.close(); });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EMFILE;

    EXPECT_NO_THROW(accept_test::once(loop));
    accept_test::once(loop);

    EXPECT_EQ(received, 0);
    EXPECT_EQ(attempts, 1);
}

TEST(AcceptorSyscallTest, UnexpectedAcceptErrorPropagatesAndRetainsListener)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    Intercept guard(fd);
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int errors = 0;

    acceptor.on_accept([](snet::Socket) {});
    acceptor.on_error([&](auto &, std::error_code) { ++errors; });
    acceptor.start();
    auto peer = listener.connect();

    next_accept_error = EINVAL;

    EXPECT_THROW(accept_test::once(loop), std::system_error);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errors, 0);
    acceptor.pause_accepting();
}

TEST(AcceptorSyscallTest, ListenerErrorPropagatesSoError)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int callbacks = 0;

    acceptor.on_accept([&](snet::Socket) { ++callbacks; });
    acceptor.on_error([&](auto &, std::error_code) { ++callbacks; });
    acceptor.start();
    so_error = ECONNRESET;
    injected_event = EPOLLERR | EPOLLIN;
    bool threw = false;

    try {
        accept_test::once(loop);
    } catch (const std::system_error &error) {
        threw = true;

        EXPECT_EQ(error.code().value(), ECONNRESET);
    }

    EXPECT_TRUE(threw);
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(attempts, 0);
    acceptor.close();
}

TEST(AcceptorSyscallTest, ListenerGetsockoptFailurePropagates)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));

    acceptor.on_accept([](snet::Socket) {});
    acceptor.start();
    so_failure = EBADF;
    injected_event = EPOLLERR;

    EXPECT_THROW(accept_test::once(loop), std::system_error);
    acceptor.close();
}

TEST(AcceptorSyscallTest, ListenerHangupWithoutSoErrorUsesEio)
{
    snet::EventLoop loop;

    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::Acceptor acceptor(loop, std::move(listener.socket));

    acceptor.on_accept([](snet::Socket) {});
    acceptor.start();
    so_error = 0;
    injected_event = EPOLLHUP;
    bool threw = false;

    try {
        accept_test::once(loop);
    } catch (const std::system_error &error) {
        threw = true;

        EXPECT_EQ(error.code().value(), EIO);
    }

    EXPECT_TRUE(threw);
    EXPECT_EQ(attempts, 0);
    acceptor.pause_accepting();
}

TEST(AcceptorSyscallTest, UnexpectedDelFailureTerminates)
{
    EXPECT_DEATH(
        {
            snet::EventLoop loop;

            accept_test::Listener listener;
            Intercept guard(listener.socket.get_fd());
            snet::Acceptor acceptor(loop, std::move(listener.socket));

            acceptor.on_accept([](snet::Socket) {});
            acceptor.start();
            fail_del = true;
            acceptor.close();
        },
        "Acceptor EPOLL_CTL_DEL fd=[0-9]+ errno=5");
}

TEST(AcceptorSyscallTest, SavedBatchCloseOtherAcceptorReuseFdThenThrow)
{
    snet::EventLoop loop;

    accept_test::Listener a, b;
    int old_fd = b.socket.get_fd();
    Intercept guard(a.socket.get_fd());

    second_fd = old_fd;
    snet::Acceptor first(loop, std::move(a.socket)), second(loop, std::move(b.socket));
    snet::Socket replacement;
    int second_calls = 0;

    second.on_accept([&](snet::Socket) { ++second_calls; });
    first.on_accept([&](snet::Socket) {
        second.close();
        auto fresh = tcp_test::make_socket(AF_INET);

        if (fresh.get_fd() != old_fd) {
            tcp_test::check(::dup2(fresh.get_fd(), old_fd), "dup2");
            replacement = snet::Socket(old_fd);
        } else
            replacement = std::move(fresh);

        first.pause_accepting();
        throw std::runtime_error("first");
    });
    first.start();
    second.start();
    auto pa = a.connect(), pb = b.connect();

    injected_event = EPOLLIN;

    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    accept_test::once(loop);

    EXPECT_EQ(second_calls, 0);
    EXPECT_EQ(attempts, 1);
    EXPECT_NE(::fcntl(replacement.get_fd(), F_GETFD), -1);
}

TEST(AcceptorSyscallTest, DestructorOutsideDispatchCancelsSavedRegistration)
{
    snet::EventLoop loop;

    accept_test::Listener a, b;
    Intercept guard(a.socket.get_fd());

    second_fd = b.socket.get_fd();
    snet::Acceptor first(loop, std::move(a.socket));
    auto second = std::make_unique<snet::Acceptor>(loop, std::move(b.socket));
    int second_calls = 0;

    second->on_accept([&](snet::Socket) { ++second_calls; });
    first.on_accept([](snet::Socket) { throw std::runtime_error("first"); });
    first.start();
    second->start();
    auto pa = a.connect(), pb = b.connect();

    injected_event = EPOLLIN;

    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    second.reset();
    accept_test::once(loop);

    EXPECT_EQ(second_calls, 0);
}

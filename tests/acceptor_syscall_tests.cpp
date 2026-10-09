#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>

extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int selected = -1, adds = 0, dels = 0, attempts = 0, next_accept_error = 0;
bool fail_add = false, registered = false;
struct Intercept {
    explicit Intercept(int fd)
    {
        selected = fd;
        adds = dels = attempts = 0;
        next_accept_error = 0;
        fail_add = registered = false;
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
    if (fd == selected && op == EPOLL_CTL_ADD && fail_add) {
        fail_add = false;
        errno = ENOMEM;
        return -1;
    }
    int result = __real_epoll_ctl(epoll, op, fd, event);
    if (fd == selected && result == 0) {
        if (op == EPOLL_CTL_ADD) {
            ++adds;
            registered = true;
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

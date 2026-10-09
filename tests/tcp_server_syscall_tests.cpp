#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>
extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int listener_fd = -1, adds = 0, client_adds = 0, last_accepted = -1;
bool fail_listener_add = false;
struct Intercept {
    explicit Intercept(int fd)
    {
        listener_fd = fd;
        adds = client_adds = 0;
        last_accepted = -1;
        fail_listener_add = false;
    }
    ~Intercept() { listener_fd = -1; }
};
} // namespace
extern "C" int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event)
{
    if (fd == listener_fd && op == EPOLL_CTL_ADD && fail_listener_add) {
        fail_listener_add = false;
        errno = ENOMEM;
        return -1;
    }
    int result = __real_epoll_ctl(epoll, op, fd, event);
    if (fd == listener_fd && op == EPOLL_CTL_ADD && result == 0)
        ++adds;
    if (listener_fd >= 0 && fd != listener_fd && op == EPOLL_CTL_ADD && result == 0)
        ++client_adds;
    return result;
}
TEST(TcpServerSyscallTest, StartFailureRetainsConfigurationAndListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &) { ++configured; });
    fail_listener_add = true;
    EXPECT_THROW(server.start(), std::system_error);
    EXPECT_EQ(adds, 0);
    EXPECT_NE(::fcntl(listener_fd, F_GETFD), -1);
    EXPECT_NO_THROW(server.start());
    EXPECT_EQ(adds, 1);
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
}
TEST(TcpServerSyscallTest, StopAcceptingBeforeStartIsPermanent)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    server.on_connection([](auto &) {});
    server.stop_accepting();
    server.stop_accepting();
    EXPECT_EQ(::fcntl(listener_fd, F_GETFD), -1);
    EXPECT_EQ(adds, 0);
    EXPECT_THROW(server.start(), std::logic_error);
}

extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags)
{
    int accepted = __real_accept4(fd, address, size, flags);
    if (fd == listener_fd && accepted >= 0)
        last_accepted = accepted;
    return accepted;
}
TEST(TcpServerSyscallTest, ConfigurationCloseSkipsClientRegistration)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.close();
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    EXPECT_EQ(client_adds, 0);
    ASSERT_GE(last_accepted, 0);
    EXPECT_EQ(::fcntl(last_accepted, F_GETFD), -1);
}

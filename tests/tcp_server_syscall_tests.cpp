#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int listener_fd = -1, adds = 0;
bool fail_listener_add = false;
struct Intercept {
    explicit Intercept(int fd)
    {
        listener_fd = fd;
        adds = 0;
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
    return result;
}
TEST(TcpServerSyscallTest, StartFailureRetainsConfigurationAndListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    Intercept guard(listener.socket.get_fd());
    snet::TcpServer server(loop, std::move(listener.socket));
    server.on_connection([](auto &) {});
    fail_listener_add = true;
    EXPECT_THROW(server.start(), std::system_error);
    EXPECT_EQ(adds, 0);
    EXPECT_NE(::fcntl(listener_fd, F_GETFD), -1);
    EXPECT_NO_THROW(server.start());
    EXPECT_EQ(adds, 1);
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

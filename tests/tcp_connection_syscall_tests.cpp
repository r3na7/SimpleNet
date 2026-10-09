#include "tcp_test_utils.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>

extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int target_fd = -1;
bool fail_add = false;
int successful_adds = 0;
struct Interception {
    explicit Interception(int fd)
    {
        target_fd = fd;
        successful_adds = 0;
    }
    ~Interception()
    {
        target_fd = -1;
        fail_add = false;
    }
};
} // namespace
extern "C" int __wrap_epoll_ctl(int epoll, int op, int fd, epoll_event *event)
{
    if (fd == target_fd && op == EPOLL_CTL_ADD && fail_add) {
        fail_add = false;
        errno = ENOMEM;
        return -1;
    }
    int result = __real_epoll_ctl(epoll, op, fd, event);
    if (fd == target_fd && op == EPOLL_CTL_ADD && result == 0)
        ++successful_adds;
    return result;
}
TEST(TcpSyscallTest, StartRetryAfterRegistrationFailure)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    Interception interception(fd);
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int closed = 0;
    connection.on_closed([&](auto &, std::error_code) { ++closed; });
    EXPECT_EQ(successful_adds, 0);
    fail_add = true;
    EXPECT_THROW(connection.start(), std::system_error);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(successful_adds, 0);
    EXPECT_NO_THROW(connection.start());
    EXPECT_EQ(successful_adds, 1);
    connection.close();
    tcp_test::drive(loop, [&] { return closed == 1; });
}

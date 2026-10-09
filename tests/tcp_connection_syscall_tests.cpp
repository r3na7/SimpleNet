#include "tcp_test_utils.hpp"
#include <algorithm>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <limits>
#include <sys/epoll.h>
#include <vector>

extern "C" ssize_t __real_send(int, const void *, std::size_t, int);
extern "C" int __real_shutdown(int, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int target_fd = -1;
bool fail_add = false;
int successful_adds = 0;
int ctl_calls = 0, send_calls = 0, shutdown_calls = 0;
int eagain_after = -1;
std::size_t send_prefix = std::numeric_limits<std::size_t>::max();
std::uint32_t last_events = 0;
bool saw_write_interest = false;
std::vector<std::size_t> send_sizes;

struct Interception {
    explicit Interception(int fd)
    {
        target_fd = fd;
        successful_adds = 0;
        ctl_calls = send_calls = shutdown_calls = 0;
        eagain_after = -1;
        send_prefix = std::numeric_limits<std::size_t>::max();
        last_events = 0;
        saw_write_interest = false;
        send_sizes.clear();
        send_sizes.reserve(256);
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
    if (fd == target_fd) {
        ++ctl_calls;
        last_events = event ? event->events : 0;
        saw_write_interest |= (last_events & EPOLLOUT) != 0;
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

extern "C" ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags)
{
    if (fd != target_fd)
        return __real_send(fd, data, size, flags);
    ++send_calls;
    send_sizes.push_back(size);
    if (eagain_after == send_calls - 1) {
        eagain_after = -1;
        errno = EAGAIN;
        return -1;
    }
    return __real_send(fd, data, std::min(size, send_prefix), flags);
}
extern "C" int __wrap_shutdown(int fd, int how)
{
    if (fd == target_fd)
        ++shutdown_calls;
    return __real_shutdown(fd, how);
}

TEST(TcpSyscallTest, PartialSendAndEagain)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    send_prefix = 3;
    eagain_after = 1;
    int changes = ctl_calls;
    auto result = connection.send(tcp_test::bytes("ABCDEFGHIJ"));
    EXPECT_EQ(result.accepted_bytes, 10u);
    EXPECT_EQ(send_calls, 0);
    EXPECT_EQ(ctl_calls, changes);
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 10;
    });
    EXPECT_EQ(received, "ABCDEFGHIJ");
    EXPECT_TRUE(saw_write_interest);
    EXPECT_EQ(last_events & EPOLLOUT, 0u);
    EXPECT_GE(send_calls, 5);
}

TEST(TcpSyscallTest, EmptyFinishPerformedByWork)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    connection.finish_sending();
    connection.finish_sending();
    EXPECT_EQ(shutdown_calls, 0);
    tcp_test::drive(loop, [&] {
        (void)pair.read();
        return pair.eof;
    });
    EXPECT_EQ(shutdown_calls, 1);
    EXPECT_EQ(send_calls, 0);
}

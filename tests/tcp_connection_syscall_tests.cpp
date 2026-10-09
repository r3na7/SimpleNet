#include "tcp_test_utils.hpp"
#include <algorithm>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <limits>
#include <sys/epoll.h>
#include <vector>

extern "C" ssize_t __real_recv(int, void *, std::size_t, int);
extern "C" ssize_t __real_send(int, const void *, std::size_t, int);
extern "C" int __real_shutdown(int, int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);
namespace
{
int target_fd = -1;
bool fail_add = false, fail_del = false, fail_mod = false;
int send_interrupts = 0, recv_interrupts = 0, shutdown_interrupts = 0;
snet::EventLoop *observed_loop = nullptr;
std::vector<std::pair<std::uint64_t, std::size_t>> send_trace;
int successful_adds = 0;
int ctl_calls = 0, send_calls = 0, shutdown_calls = 0, recv_calls = 0;
std::size_t recv_prefix = std::numeric_limits<std::size_t>::max();
int recv_eagain_after = -1, recv_error_after = -1;
std::vector<std::size_t> recv_sizes;
int eagain_after = -1;
std::size_t send_prefix = std::numeric_limits<std::size_t>::max();
std::uint32_t last_events = 0;
bool saw_write_interest = false;
std::vector<std::size_t> send_sizes;

struct Interception {
    explicit Interception(int fd)
    {
        target_fd = fd;
        fail_del = fail_mod = false;
        send_interrupts = recv_interrupts = shutdown_interrupts = 0;
        observed_loop = nullptr;
        send_trace.clear();
        send_trace.reserve(256);
        successful_adds = 0;
        ctl_calls = send_calls = shutdown_calls = recv_calls = 0;
        recv_prefix = std::numeric_limits<std::size_t>::max();
        recv_eagain_after = recv_error_after = -1;
        recv_sizes.clear();
        recv_sizes.reserve(256);
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
    if (fd == target_fd && op == EPOLL_CTL_DEL && fail_del) {
        errno = EIO;
        return -1;
    }
    if (fd == target_fd && op == EPOLL_CTL_MOD && fail_mod) {
        fail_mod = false;
        errno = ENOMEM;
        return -1;
    }
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
    int received = 0;
    connection.on_data([&](auto &current) {
        EXPECT_EQ(tcp_test::text(current.input_data()), "ok");
        ++received;
    });
    connection.on_closed([&](auto &, std::error_code) { ++closed; });
    EXPECT_EQ(successful_adds, 0);
    fail_add = true;
    EXPECT_THROW(connection.start(), std::system_error);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(successful_adds, 0);
    EXPECT_NO_THROW(connection.start());
    EXPECT_EQ(successful_adds, 1);
    pair.send("ok");
    tcp_test::drive(loop, [&] { return received == 1; });
    connection.close();
    tcp_test::drive(loop, [&] { return closed == 1; });
}

extern "C" ssize_t __wrap_send(int fd, const void *data, std::size_t size, int flags)
{
    if (fd != target_fd)
        return __real_send(fd, data, size, flags);
    ++send_calls;
    send_sizes.push_back(size);
    if (observed_loop)
        send_trace.emplace_back(observed_loop->iteration_id(), size);
    if (send_interrupts > 0) {
        --send_interrupts;
        errno = EINTR;
        return -1;
    }
    if (eagain_after == send_calls - 1) {
        eagain_after = -1;
        errno = EAGAIN;
        return -1;
    }
    return __real_send(fd, data, std::min(size, send_prefix), flags);
}
extern "C" int __wrap_shutdown(int fd, int how)
{
    if (fd == target_fd) {
        ++shutdown_calls;
        if (shutdown_interrupts > 0) {
            --shutdown_interrupts;
            errno = EINTR;
            return -1;
        }
    }
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

extern "C" ssize_t __wrap_recv(int fd, void *data, std::size_t size, int flags)
{
    if (fd != target_fd)
        return __real_recv(fd, data, size, flags);
    ++recv_calls;
    recv_sizes.push_back(size);
    if (recv_interrupts > 0) {
        --recv_interrupts;
        errno = EINTR;
        return -1;
    }
    if (recv_error_after == recv_calls - 1) {
        recv_error_after = -1;
        errno = ECONNRESET;
        return -1;
    }
    if (recv_eagain_after == recv_calls - 1) {
        recv_eagain_after = -1;
        errno = EAGAIN;
        return -1;
    }
    return __real_recv(fd, data, std::min(size, recv_prefix), flags);
}

TEST(TcpSyscallTest, DataGroupedUntilEagainIncludesOldBytes)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    recv_prefix = 2;
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("ABCDEF");
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "ABCDEF");
    EXPECT_GE(recv_calls, 4);
    pair.send("GHI");
    tcp_test::drive(loop, [&] { return calls == 2; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "ABCDEFGHI");
}

TEST(TcpSyscallTest, EagainWithoutNewDataHasNoNotification)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    recv_eagain_after = 0;
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("abc");
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(connection.input_data().empty());
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "abc");
}

TEST(TcpSyscallTest, InputLimitNeverUsesZeroRecv)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::ConnectionOptions options;
    options.input_limit = 4;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("abcdef");
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(recv_calls, 1);
    connection.consume_input(2);
    tcp_test::drive(loop, [&] { return calls == 2; });
    for (auto size : recv_sizes) {
        EXPECT_GT(size, 0u);
        EXPECT_LE(size, 4u);
    }
}

TEST(TcpSyscallTest, ReceiveCallBudgetCapsGroup)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::ConnectionOptions options;
    options.read_call_budget = 2;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    recv_prefix = 1;
    std::vector<std::string> groups;
    connection.on_data([&](auto &current) {
        groups.push_back(tcp_test::text(current.input_data()));
        current.consume_input(current.input_data().size());
    });
    pair.send("abcdef");
    tcp_test::drive(loop, [&] { return groups.size() == 3; });
    EXPECT_EQ(groups, (std::vector<std::string>{"ab", "cd", "ef"}));
}

TEST(TcpSyscallTest, PositiveReadThenErrorPreservesBytesAndReason)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    recv_prefix = 2;
    recv_error_after = 1;
    bool data = false, closed = false;
    connection.on_data([&](auto &current) {
        EXPECT_EQ(tcp_test::text(current.input_data()), "AB");
        auto result = current.send(tcp_test::bytes("reply"));
        EXPECT_EQ(result.status, snet::SendStatus::io_error);
        EXPECT_EQ(result.accepted_bytes, 0u);
        EXPECT_EQ(result.error.value(), ECONNRESET);
        current.close();
        data = true;
    });
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_TRUE(data);
        EXPECT_EQ(error.value(), ECONNRESET);
        closed = true;
    });
    pair.send("ABCDE");
    tcp_test::drive(loop, [&] { return closed; });
    EXPECT_EQ(tcp_test::text(connection.input_data()), "AB");
    EXPECT_EQ(connection.send({}).status, snet::SendStatus::closed);
}

TEST(TcpSyscallTest, ErrorDoesNotStopOtherConnection)
{
    snet::EventLoop loop;
    tcp_test::Pair bad, good;
    Interception interception(bad.accepted.get_fd());
    snet::TcpConnection first(loop, std::move(bad.accepted)), second(loop, std::move(good.accepted));
    first.start();
    second.start();
    recv_error_after = 0;
    bool closed = false;
    std::string data;
    first.on_closed([&](auto &, std::error_code error) {
        EXPECT_EQ(error.value(), ECONNRESET);
        closed = true;
    });
    second.on_data([&](auto &current) { data = tcp_test::text(current.input_data()); });
    bad.send("bad");
    good.send("ok");
    tcp_test::drive(loop, [&] { return closed && data == "ok"; });
    EXPECT_EQ(second.send(tcp_test::bytes("still alive")).status, snet::SendStatus::accepted);
}

TEST(TcpSyscallTest, SocketAndWorkShareWriteQuota)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    observed_loop = &loop;
    snet::ConnectionOptions options;
    options.write_byte_budget = 3;
    options.write_call_budget = 2;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    eagain_after = 0;
    connection.send(tcp_test::bytes("abcdefghi"));
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop(); // EAGAIN leaves real EPOLLOUT interest.
    snet::detail::LoopWork produce(loop, [&] { connection.send(tcp_test::bytes("jkl")); });
    produce.schedule();
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received.size() == 12;
    });
    EXPECT_EQ(received, "abcdefghijkl");
    for (std::size_t i = 0; i < send_trace.size();) {
        auto iteration = send_trace[i].first;
        std::size_t bytes = 0, calls = 0;
        while (i < send_trace.size() && send_trace[i].first == iteration) {
            bytes += send_trace[i++].second;
            ++calls;
        }
        EXPECT_LE(bytes, 3u);
        EXPECT_LE(calls, 2u);
    }
}

TEST(TcpSyscallTest, InterruptedReadConsumesAttempts)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::ConnectionOptions options;
    options.read_call_budget = 2;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    recv_interrupts = 3;
    pair.send("x");
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(recv_calls, 2);
    EXPECT_TRUE(connection.input_data().empty());
    tcp_test::drive(loop, [&] { return connection.input_data().size() == 1; });
    EXPECT_GE(recv_calls, 4);
}

TEST(TcpSyscallTest, InterruptedSendAndShutdownConsumeAttempts)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::ConnectionOptions options;
    options.write_call_budget = 2;
    snet::TcpConnection connection(loop, std::move(pair.accepted), options);
    connection.start();
    send_interrupts = 3;
    shutdown_interrupts = 3;
    connection.send(tcp_test::bytes("x"));
    connection.finish_sending();
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(send_calls, 2);
    EXPECT_EQ(shutdown_calls, 0);
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return pair.eof;
    });
    EXPECT_EQ(received, "x");
    EXPECT_EQ(send_calls, 4);
    EXPECT_EQ(shutdown_calls, 4);
}

TEST(TcpSyscallTest, PositiveReadErrorAndThrowStillClosesWithoutClosedCallback)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    Interception interception(fd);
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    recv_prefix = 1;
    recv_error_after = 1;
    int closed = 0;
    connection.on_closed([&](auto &, std::error_code) { ++closed; });
    connection.on_data([](auto &self) {
        EXPECT_EQ(tcp_test::text(self.input_data()), "a");
        throw std::runtime_error("app");
    });
    pair.send("ab");
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(closed, 0);
}

TEST(TcpSyscallTest, PausedInputAndBlockedOutputPeerReset)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    connection.pause_reading();
    eagain_after = 0;
    connection.send(tcp_test::bytes("pending"));
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    ASSERT_TRUE(saw_write_interest);
    linger reset{1, 0};
    tcp_test::check(::setsockopt(pair.peer.get_fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), "linger");
    pair.peer.close();
    int closed = 0;
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_TRUE(error);
        ++closed;
    });
    tcp_test::drive(loop, [&] { return closed == 1; });
    EXPECT_EQ(recv_calls, 0);
    EXPECT_LE(send_calls, 3);
    int previous = send_calls;
    stop.schedule();
    loop.loop();
    EXPECT_EQ(send_calls, previous);
}

TEST(TcpSyscallTest, UnexpectedDelFailureTerminatesWithDiagnostic)
{
    EXPECT_DEATH(
        {
            snet::EventLoop loop;
            tcp_test::Pair pair;
            Interception interception(pair.accepted.get_fd());
            snet::TcpConnection connection(loop, std::move(pair.accepted));
            connection.start();
            fail_del = true;
            connection.close();
        },
        "TcpConnection EPOLL_CTL_DEL fd=[0-9]+ errno=5");
}

TEST(TcpSyscallTest, PausedInputGracefulHangupDoesNotSpin)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    connection.pause_reading();
    eagain_after = 0;
    connection.send(tcp_test::bytes("pending"));
    connection.finish_sending();
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    tcp_test::check(::shutdown(pair.peer.get_fd(), SHUT_RDWR), "shutdown");
    tcp_test::drive(loop, [&] { return send_calls >= 2; });
    int calls = send_calls;
    for (int i = 0; i < 3; ++i) {
        stop.schedule();
        loop.loop();
    }
    EXPECT_EQ(send_calls, calls);
    EXPECT_EQ(last_events, 0u);
}

TEST(TcpSyscallTest, RestartRetriesWriteInterestAfterFailedMod)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    eagain_after = 0;
    fail_mod = true;
    connection.send(tcp_test::bytes("queued"));
    EXPECT_THROW(loop.loop(), std::system_error);
    EXPECT_EQ(send_calls, 1);
    EXPECT_TRUE(pair.read().empty());
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received == "queued";
    });
    EXPECT_EQ(received, "queued");
}

TEST(TcpSyscallTest, RestartRetriesWriteInterestAfterFailedAdd)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    Interception interception(pair.accepted.get_fd());
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.pause_reading();
    connection.start();
    eagain_after = 0;
    fail_add = true;
    connection.send(tcp_test::bytes("queued"));
    EXPECT_THROW(loop.loop(), std::system_error);
    EXPECT_EQ(send_calls, 1);
    EXPECT_TRUE(pair.read().empty());
    std::string received;
    tcp_test::drive(loop, [&] {
        received += pair.read();
        return received == "queued";
    });
    EXPECT_EQ(received, "queued");
}

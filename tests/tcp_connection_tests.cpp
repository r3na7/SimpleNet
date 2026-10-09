#include "tcp_test_utils.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <type_traits>

using tcp_test::bytes;
static_assert(!std::is_copy_constructible_v<snet::TcpConnection>);
static_assert(!std::is_move_constructible_v<snet::TcpConnection>);
static_assert(!std::is_copy_assignable_v<snet::TcpConnection>);
static_assert(!std::is_move_assignable_v<snet::TcpConnection>);
static_assert(std::is_nothrow_destructible_v<snet::TcpConnection>);

TEST(TcpConnectionTest, InvalidOptionsCloseSocket)
{
    snet::EventLoop loop;
    using Field = std::size_t snet::ConnectionOptions::*;
    const Field fields[]{&snet::ConnectionOptions::input_limit,       &snet::ConnectionOptions::output_limit,
                         &snet::ConnectionOptions::read_byte_budget,  &snet::ConnectionOptions::read_call_budget,
                         &snet::ConnectionOptions::write_byte_budget, &snet::ConnectionOptions::write_call_budget};
    for (auto field : fields) {
        tcp_test::Pair pair;
        int fd = pair.accepted.get_fd();
        snet::ConnectionOptions options;
        options.*field = 0;
        EXPECT_THROW(snet::TcpConnection(loop, std::move(pair.accepted), options), std::invalid_argument);
        errno = 0;
        EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
    }
    for (auto threshold : {std::size_t{65536}, std::size_t{65537}}) {
        tcp_test::Pair pair;
        snet::ConnectionOptions options;
        options.output_low_watermark = threshold;
        EXPECT_THROW(snet::TcpConnection(loop, std::move(pair.accepted), options), std::invalid_argument);
    }
}

TEST(TcpConnectionTest, EmptySocketRejected)
{
    snet::EventLoop loop;
    EXPECT_THROW(snet::TcpConnection(loop, snet::Socket{}), std::invalid_argument);
}

TEST(TcpConnectionTest, ConstructedButNotStarted)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int calls = 0;
    connection.on_data([&](auto &) { ++calls; });
    pair.send("abc");
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 0);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
}

TEST(TcpConnectionTest, StartAndDestructorRemoveRegistration)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    {
        snet::TcpConnection connection(loop, std::move(pair.accepted));
        connection.start();
    }
    errno = 0;
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    int replacement = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    tcp_test::check(replacement, "socket");
    snet::Socket owner(replacement);
    tcp_test::check(::dup2(replacement, fd), "dup2");
    snet::Socket reused;
    if (replacement != fd)
        reused = snet::Socket(fd);
    snet::Channel channel(fd);
    channel.set_events(EPOLLIN);
    EXPECT_NO_THROW(loop.update_channel(&channel));
    EXPECT_NO_THROW(loop.remove_channel(&channel));
}

TEST(TcpConnectionTest, RepeatStartRejected)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.start();
    EXPECT_THROW(connection.start(), std::logic_error);
}

TEST(TcpConnectionTest, StartAfterCloseRejected)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    connection.close();
    EXPECT_THROW(connection.start(), std::logic_error);
    EXPECT_EQ(connection.send({}).status, snet::SendStatus::closed);
}

TEST(TcpConnectionTest, SendAndFinishBeforeStartRejected)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    EXPECT_THROW(connection.send(bytes("abc")), std::logic_error);
    EXPECT_THROW(connection.send({}), std::logic_error);
    EXPECT_THROW(connection.finish_sending(), std::logic_error);
}

TEST(TcpConnectionTest, CloseBeforeStartDefersNotification)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int fd = pair.accepted.get_fd();
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    int calls = 0;
    connection.on_closed([&](auto &, std::error_code error) {
        EXPECT_FALSE(error);
        ++calls;
    });
    connection.close();
    connection.close();
    EXPECT_EQ(calls, 0);
    errno = 0;
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    tcp_test::drive(loop, [&] { return calls == 1; });
    EXPECT_EQ(calls, 1);
}

TEST(TcpConnectionTest, DestructorCancelsPendingClose)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    int calls = 0;
    {
        snet::TcpConnection connection(loop, std::move(pair.accepted));
        connection.on_closed([&](auto &, std::error_code) { ++calls; });
        connection.close();
    }
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    stop.schedule();
    loop.loop();
    EXPECT_EQ(calls, 0);
}

TEST(TcpConnectionTest, ClosedCallbackRetainedDuringSelfClear)
{
    snet::EventLoop loop;
    tcp_test::Pair pair;
    snet::TcpConnection connection(loop, std::move(pair.accepted));
    auto token = std::make_shared<int>(7);
    std::weak_ptr<int> observed(token);
    bool called = false;
    connection.on_closed([&, token](auto &current, std::error_code) {
        current.on_closed({});
        EXPECT_FALSE(observed.expired());
        called = true;
    });
    token.reset();
    connection.close();
    tcp_test::drive(loop, [&] { return called; });
    EXPECT_TRUE(observed.expired());
}

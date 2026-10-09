#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>
#include <type_traits>
static_assert(!std::is_copy_constructible_v<snet::TcpServer>);
static_assert(!std::is_move_constructible_v<snet::TcpServer>);
static_assert(!std::is_copy_assignable_v<snet::TcpServer>);
static_assert(!std::is_move_assignable_v<snet::TcpServer>);

TEST(TcpServerTest, InvalidOptionsCloseListener)
{
    snet::EventLoop loop;
    using Field = std::size_t snet::ConnectionOptions::*;
    for (Field field : {&snet::ConnectionOptions::input_limit, &snet::ConnectionOptions::output_limit,
                        &snet::ConnectionOptions::read_byte_budget, &snet::ConnectionOptions::read_call_budget,
                        &snet::ConnectionOptions::write_byte_budget, &snet::ConnectionOptions::write_call_budget}) {
        accept_test::Listener listener;
        int fd = listener.socket.get_fd();
        snet::TcpServerOptions options;
        options.connection.*field = 0;
        EXPECT_THROW(snet::TcpServer(loop, std::move(listener.socket), options), std::invalid_argument);
        EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    }
    for (std::size_t threshold : {65536u, 65537u}) {
        accept_test::Listener listener;
        int fd = listener.socket.get_fd();
        snet::TcpServerOptions options;
        options.connection.output_low_watermark = threshold;
        EXPECT_THROW(snet::TcpServer(loop, std::move(listener.socket), options), std::invalid_argument);
        EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    }
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    snet::TcpServerOptions options;
    options.acceptor.accept_call_budget = 0;
    EXPECT_THROW(snet::TcpServer(loop, std::move(listener.socket), options), std::invalid_argument);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
}
TEST(TcpServerTest, EmptyListenerRejected)
{
    snet::EventLoop loop;
    EXPECT_THROW(snet::TcpServer(loop, snet::Socket{}), std::invalid_argument);
}
TEST(TcpServerTest, ConstructedButNotStarted)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int called = 0;
    server.on_connection([&](auto &) { ++called; });
    auto peer = listener.connect();
    accept_test::once(loop);
    EXPECT_EQ(called, 0);
}
TEST(TcpServerTest, StartRequiresConfigurationAndRepeatRejected)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    EXPECT_THROW(server.start(), std::logic_error);
    EXPECT_THROW(server.resume_accepting(), std::logic_error);
    server.on_connection([](auto &) {});
    EXPECT_NO_THROW(server.resume_accepting());
    EXPECT_NO_THROW(server.start());
    EXPECT_THROW(server.start(), std::logic_error);
}
TEST(TcpServerTest, StopBeforeStartIsPermanent)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    snet::TcpServer server(loop, std::move(listener.socket));
    server.on_connection([](auto &) {});
    server.stop();
    server.stop();
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_THROW(server.start(), std::logic_error);
    EXPECT_THROW(server.resume_accepting(), std::logic_error);
}
TEST(TcpServerTest, CloseConnectionsBeforeStartDoesNotStopListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    server.close_connections();
    server.on_connection([](auto &) {});
    EXPECT_NO_THROW(server.start());
}
TEST(TcpServerTest, DestructorReleasesListenerAndCallsNoApplication)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    int called = 0;
    {
        snet::TcpServer server(loop, std::move(listener.socket));
        server.on_connection([&](auto &) { ++called; });
        server.on_accept_error([&](auto &, std::error_code) { ++called; });
        server.start();
    }
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    accept_test::once(loop);
    EXPECT_EQ(called, 0);
}

namespace
{
std::string peer_read(snet::Socket &peer, bool *eof = nullptr)
{
    std::string result;
    char data[256];
    for (;;) {
        auto n = ::recv(peer.get_fd(), data, sizeof(data), MSG_DONTWAIT);
        if (n > 0) {
            result.append(data, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            if (eof)
                *eof = true;
            return result;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return result;
        tcp_test::check(-1, "recv peer");
    }
}
void two_clients(int family)
{
    snet::EventLoop loop;
    accept_test::Listener listener(family);
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, closed = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        EXPECT_THROW(connection.send({}), std::logic_error);
        EXPECT_THROW(connection.finish_sending(), std::logic_error);
        connection.on_data([](auto &current) {
            auto result = current.send(current.input_data());
            current.consume_input(result.accepted_bytes);
        });
        connection.on_eof([](auto &current) { current.finish_sending(); });
        connection.on_closed([&](auto &, std::error_code error) {
            EXPECT_FALSE(error);
            ++closed;
        });
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    EXPECT_EQ(configured, 0);
    ASSERT_EQ(::send(a.get_fd(), "one", 3, MSG_NOSIGNAL), 3);
    ASSERT_EQ(::send(b.get_fd(), "two", 3, MSG_NOSIGNAL), 3);
    tcp_test::check(::shutdown(a.get_fd(), SHUT_WR), "shutdown");
    tcp_test::check(::shutdown(b.get_fd(), SHUT_WR), "shutdown");
    std::string first, second;
    bool aeof = false, beof = false;
    tcp_test::drive(loop, [&] {
        first += peer_read(a, &aeof);
        second += peer_read(b, &beof);
        return closed == 2 && aeof && beof;
    });
    EXPECT_EQ(configured, 2);
    EXPECT_EQ(first, "one");
    EXPECT_EQ(second, "two");
}
} // namespace
TEST(TcpServerTest, RealIpv4TwoClientsConfiguredBeforeStart) { two_clients(AF_INET); }
TEST(TcpServerTest, RealIpv6TwoClientsConfiguredBeforeStart) { two_clients(AF_INET6); }
TEST(TcpServerTest, ConnectionOptionsPropagated)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServerOptions options;
    options.connection.input_limit = 4;
    options.connection.output_limit = 4;
    options.connection.output_low_watermark = 0;
    snet::TcpServer server(loop, std::move(listener.socket), options);
    int data = 0;
    server.on_connection([&](auto &connection) {
        connection.on_data([&](auto &current) {
            ++data;
            EXPECT_EQ(current.input_data().size(), 4u);
            auto result = current.send(tcp_test::bytes("abcdef"));
            EXPECT_EQ(result.accepted_bytes, 4u);
            EXPECT_EQ(result.status, snet::SendStatus::would_block);
        });
    });
    server.start();
    auto peer = listener.connect();
    ASSERT_EQ(::send(peer.get_fd(), "123456", 6, MSG_NOSIGNAL), 6);
    std::string reply;
    tcp_test::drive(loop, [&] {
        reply += peer_read(peer);
        return reply.size() == 4;
    });
    EXPECT_EQ(data, 1);
    EXPECT_EQ(reply, "abcd");
}
TEST(TcpServerTest, ConfigurationCloseNotifiesBeforeOwnerRemoval)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::weak_ptr<int> weak;
    int closed = 0;
    server.on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_closed([&, token](auto &, std::error_code) {
            EXPECT_FALSE(weak.expired());
            ++closed;
        });
        connection.close();
        EXPECT_FALSE(weak.expired());
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return weak.expired() && closed == 1; });
    EXPECT_EQ(closed, 1);
}
TEST(TcpServerTest, MissingOnClosedStillReleasesOwner)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::weak_ptr<int> weak;
    int configured = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_data([token](auto &) {});
        connection.close();
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1 && weak.expired(); });
}
TEST(TcpServerTest, UserClosedReplacementDoesNotBreakCleanup)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::weak_ptr<int> weak;
    int old = 0, next = 0;
    server.on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_data([token](auto &) {});
        connection.on_closed([&](auto &, std::error_code) { ++old; });
        connection.on_closed([&](auto &, std::error_code) { ++next; });
        connection.close();
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return next == 1 && weak.expired(); });
    EXPECT_EQ(old, 0);
}
TEST(TcpServerTest, RetainedInputDoesNotPreventRemoval)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::weak_ptr<int> weak;
    bool closed = false;
    server.on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_data([token](auto &current) { current.close(); });
        connection.on_closed([&](auto &current, std::error_code) {
            EXPECT_EQ(tcp_test::text(current.input_data()), "data");
            closed = true;
        });
    });
    server.start();
    auto peer = listener.connect();
    ASSERT_EQ(::send(peer.get_fd(), "data", 4, MSG_NOSIGNAL), 4);
    tcp_test::drive(loop, [&] { return closed && weak.expired(); });
}
TEST(TcpServerTest, DestructorClosesActiveAndCancelsNotifications)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int configured = 0, closed = 0;
    std::weak_ptr<int> weak;
    auto server = std::make_unique<snet::TcpServer>(loop, std::move(listener.socket));
    server->on_connection([&](auto &connection) {
        ++configured;
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_data([token](auto &) {});
        connection.on_closed([&](auto &, std::error_code) { ++closed; });
    });
    server->start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    EXPECT_FALSE(weak.expired());
    server.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(closed, 0);
    accept_test::once(loop);
    EXPECT_EQ(closed, 0);
}

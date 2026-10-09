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

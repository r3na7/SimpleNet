#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>
#include <sys/eventfd.h>
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

TEST(TcpServerTest, ConfigurationThrowClosesAndRemovesOnlyNewOwner)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::weak_ptr<int> weak;
    int closed = 0;
    server.on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_closed([&, token](auto &, std::error_code) { ++closed; });
        throw std::runtime_error("configuration");
    });
    server.start();
    auto peer = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(closed, 0);
}
TEST(TcpServerTest, DataCloseThenThrowRemovesAfterUnwind)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::weak_ptr<int> weak;
    int closed = 0;
    server.on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_data([&, token](auto &current) {
            current.close();
            EXPECT_FALSE(weak.expired());
            throw std::runtime_error("data");
        });
        connection.on_closed([&](auto &, std::error_code) { ++closed; });
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return !weak.expired(); });
    ASSERT_EQ(::send(peer.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(closed, 0);
}
TEST(TcpServerTest, ThrowingClosedIsNotRetriedAndOwnerRemoved)
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
            ++closed;
            throw std::runtime_error("closed");
        });
        connection.close();
    });
    server.start();
    auto peer = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(closed, 1);
    accept_test::once(loop);
    EXPECT_EQ(closed, 1);
}
TEST(TcpServerTest, UnrelatedExceptionCancelsClosedButPreservesLiveOutput)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, closed = 0;
    snet::TcpConnection *live = nullptr;
    std::weak_ptr<int> weak;
    server.on_connection([&](auto &connection) {
        if (++configured == 1) {
            auto token = std::make_shared<int>(0);
            weak = token;
            connection.on_closed([&, token](auto &, std::error_code) { ++closed; });
            connection.close();
        } else
            live = &connection;
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    snet::detail::LoopWork fail(loop, [] { throw std::runtime_error("unrelated"); });
    fail.schedule();
    EXPECT_THROW(loop.loop(), std::runtime_error);
    EXPECT_EQ(configured, 2);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(closed, 0);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(live->send(tcp_test::bytes("live")).status, snet::SendStatus::accepted);
    std::string reply;
    tcp_test::drive(loop, [&] {
        reply += peer_read(b);
        return reply == "live";
    });
}
TEST(TcpServerTest, QuitAndSmallWorkBudgetRetainPendingOwners)
{
    snet::EventLoop loop;
    loop.set_work_budget(1);
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::vector<std::weak_ptr<int>> lifetimes;
    int closed = 0;
    server.on_connection([&](auto &connection) {
        auto token = std::make_shared<int>(0);
        lifetimes.push_back(token);
        connection.on_closed([&, token](auto &, std::error_code) { ++closed; });
        connection.close();
    });
    server.start();
    std::vector<snet::Socket> peers;
    for (int i = 0; i < 6; ++i)
        peers.push_back(listener.connect());
    accept_test::once(loop);
    ASSERT_EQ(lifetimes.size(), 6u);
    EXPECT_EQ(closed, 0);
    for (auto &weak : lifetimes)
        EXPECT_FALSE(weak.expired());
    tcp_test::drive(loop, [&] { return closed == 6; });
    for (auto &weak : lifetimes)
        EXPECT_TRUE(weak.expired());
}
TEST(TcpServerTest, ClosedCallbackClosesAnotherWithoutNestedNotification)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::vector<snet::TcpConnection *> connections;
    std::vector<std::weak_ptr<int>> lifetimes;
    int closed = 0;
    bool inside = false;
    server.on_connection([&](auto &connection) {
        connections.push_back(&connection);
        auto token = std::make_shared<int>(0);
        lifetimes.push_back(token);
        const int index = static_cast<int>(connections.size());
        connection.on_closed([&, token, index](auto &, std::error_code) {
            EXPECT_FALSE(inside);
            inside = true;
            ++closed;
            if (index == 1) {
                connections[1]->close();
                EXPECT_EQ(closed, 1);
            }
            inside = false;
        });
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return connections.size() == 2; });
    connections[0]->close();
    tcp_test::drive(loop, [&] { return closed == 2; });
    for (auto &weak : lifetimes)
        EXPECT_TRUE(weak.expired());
}
TEST(TcpServerTest, DestructorWithPendingCloseCallsNothing)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    std::weak_ptr<int> weak;
    int configured = 0, closed = 0;
    auto server = std::make_unique<snet::TcpServer>(loop, std::move(listener.socket));
    server->on_connection([&](auto &connection) {
        ++configured;
        auto token = std::make_shared<int>(0);
        weak = token;
        connection.on_closed([&, token](auto &, std::error_code) { ++closed; });
        connection.close();
    });
    loop.set_work_budget(1);
    server->start();
    auto peer = listener.connect();
    accept_test::once(loop);
    EXPECT_EQ(configured, 1);
    EXPECT_EQ(closed, 0);
    EXPECT_FALSE(weak.expired());
    server.reset();
    EXPECT_TRUE(weak.expired());
    accept_test::once(loop);
    EXPECT_EQ(closed, 0);
}
TEST(TcpServerTest, StableEntriesAcrossRehash)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::vector<snet::TcpConnection *> addresses;
    std::vector<std::weak_ptr<int>> lifetimes;
    std::vector<snet::Socket> peers;
    server.on_connection([&](auto &connection) {
        addresses.push_back(&connection);
        auto token = std::make_shared<int>(0);
        lifetimes.push_back(token);
        auto *original = &connection;
        connection.on_data([original, token](auto &current) {
            EXPECT_EQ(&current, original);
            auto result = current.send(current.input_data());
            current.consume_input(result.accepted_bytes);
        });
    });
    server.start();
    for (int batch = 0; batch < 32; ++batch) {
        for (int i = 0; i < 16; ++i)
            peers.push_back(listener.connect());
        tcp_test::drive(loop, [&] { return addresses.size() == peers.size(); });
    }
    ASSERT_EQ(addresses.size(), 512u);
    for (std::size_t i : {0u, 31u, 255u, 511u})
        addresses[i]->close();
    tcp_test::drive(loop, [&] {
        return lifetimes[0].expired() && lifetimes[31].expired() && lifetimes[255].expired() &&
               lifetimes[511].expired();
    });
    EXPECT_FALSE(lifetimes[1].expired());
    ASSERT_EQ(::send(peers[1].get_fd(), "x", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        reply += peer_read(peers[1]);
        return reply == "x";
    });
}

TEST(TcpServerTest, StopAcceptingKeepsExistingClientActive)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.on_data([](auto &current) {
            current.send(tcp_test::bytes("ok"));
            current.consume_input(current.input_data().size());
        });
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    server.stop_accepting();
    server.stop_accepting();
    EXPECT_THROW(server.resume_accepting(), std::logic_error);
    EXPECT_THROW(listener.connect(), std::system_error);
    ASSERT_EQ(::send(peer.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        reply += peer_read(peer);
        return reply == "ok";
    });
}
TEST(TcpServerTest, CloseConnectionsKeepsListenerAccepting)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, closed = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.on_closed([&](auto &, std::error_code) { ++closed; });
    });
    server.start();
    auto first = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    server.close_connections();
    server.close_connections();
    tcp_test::drive(loop, [&] { return closed == 1; });
    auto second = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 2; });
    EXPECT_EQ(closed, 1);
}
TEST(TcpServerTest, StopFromDataDoesNotQuitSharedLoop)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    bool stopped = false;
    server.on_connection([&](auto &connection) {
        connection.on_data([&](auto &) {
            server.stop();
            server.stop();
            stopped = true;
        });
    });
    server.start();
    auto peer = listener.connect();
    accept_test::once(loop);
    snet::Socket event(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    snet::Channel channel(event.get_fd());
    int unrelated = 0;
    channel.set_events(EPOLLIN);
    channel.set_read_callback([&] {
        std::uint64_t value;
        ::read(event.get_fd(), &value, sizeof(value));
        ++unrelated;
    });
    loop.update_channel(&channel);
    std::uint64_t one = 1;
    ASSERT_EQ(::write(event.get_fd(), &one, sizeof(one)), sizeof(one));
    int phases = 0;
    snet::detail::LoopWork *self = nullptr;
    snet::detail::LoopWork follow(loop, [&] {
        if (++phases == 2)
            loop.quit();
        else
            self->schedule();
    });
    self = &follow;
    ASSERT_EQ(::send(peer.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    follow.schedule();
    loop.loop();
    EXPECT_TRUE(stopped);
    EXPECT_EQ(unrelated, 1);
    EXPECT_EQ(phases, 2);
    loop.remove_channel(&channel);
}
TEST(TcpServerTest, StopAcceptingFromConfigurationStartsCurrentClient)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    server.on_connection([&](auto &connection) {
        server.stop_accepting();
        connection.on_data([](auto &current) {
            current.send(tcp_test::bytes("ok"));
            current.consume_input(current.input_data().size());
        });
    });
    server.start();
    auto peer = listener.connect();
    ASSERT_EQ(::send(peer.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string reply;
    tcp_test::drive(loop, [&] {
        reply += peer_read(peer);
        return reply == "ok";
    });
    EXPECT_THROW(listener.connect(), std::system_error);
}
TEST(TcpServerTest, CloseAndStopFromConfigurationCloseCurrentBeforeStart)
{
    for (bool permanent : {false, true}) {
        snet::EventLoop loop;
        accept_test::Listener listener;
        snet::TcpServer server(loop, std::move(listener.socket));
        int closed = 0;
        server.on_connection([&](auto &connection) {
            connection.on_closed([&](auto &, std::error_code) { ++closed; });
            if (permanent)
                server.stop();
            else
                server.close_connections();
        });
        server.start();
        auto peer = listener.connect();
        tcp_test::drive(loop, [&] { return closed == 1; });
        if (permanent)
            EXPECT_THROW(listener.connect(), std::system_error);
        else {
            auto second = listener.connect();
            tcp_test::drive(loop, [&] { return closed == 2; });
        }
    }
}
TEST(TcpServerTest, CloseConnectionsFromClosedCallbackIsDeferred)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0, closed = 0;
    bool inside = false;
    snet::TcpConnection *first = nullptr;
    server.on_connection([&](auto &connection) {
        if (++configured == 1)
            first = &connection;
        connection.on_closed([&](auto &, std::error_code) {
            EXPECT_FALSE(inside);
            inside = true;
            ++closed;
            server.close_connections();
            inside = false;
        });
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 2; });
    first->close();
    tcp_test::drive(loop, [&] { return closed == 2; });
}
TEST(TcpServerTest, SelfReplaceConfigurationRetainsCallable)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int first = 0, next = 0;
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> weak = token;
    server.on_connection([&, token](auto &) {
        ++first;
        server.on_connection([&](auto &) { ++next; });
        EXPECT_FALSE(weak.expired());
    });
    token.reset();
    server.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return first + next == 2; });
    EXPECT_EQ(first, 1);
    EXPECT_EQ(next, 1);
    EXPECT_TRUE(weak.expired());
}
TEST(TcpServerTest, MutableConfigurationStatePersists)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    std::vector<int> values;
    server.on_connection([&, count = 0](auto &) mutable { values.push_back(++count); });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return values.size() == 2; });
    EXPECT_EQ(values, (std::vector<int>{1, 2}));
}
TEST(TcpServerTest, ConfigurationReplacementSurvivesThrow)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int first = 0, next = 0;
    server.on_connection([&](auto &) {
        ++first;
        server.on_connection([&](auto &) { ++next; });
        throw std::runtime_error("configuration");
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    tcp_test::drive(loop, [&] { return next == 1; });
    EXPECT_EQ(first, 1);
}
TEST(TcpServerTest, ClearConfigurationPausesAndReinstallRequiresResume)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int first = 0, next = 0;
    server.on_connection([&](auto &) {
        ++first;
        server.on_connection({});
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    EXPECT_NO_THROW(accept_test::once(loop));
    EXPECT_EQ(first, 1);
    EXPECT_THROW(server.resume_accepting(), std::logic_error);
    server.on_connection([&](auto &) { ++next; });
    accept_test::once(loop);
    EXPECT_EQ(next, 0);
    server.resume_accepting();
    tcp_test::drive(loop, [&] { return next == 1; });
}
TEST(TcpServerTest, ResumeInsideConfigurationRecognizesCurrentCallable)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &) {
        ++configured;
        EXPECT_NO_THROW(server.resume_accepting());
    });
    server.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
}
TEST(TcpServerTest, ReplacementDoesNotChangeExistingClientHandlers)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServer server(loop, std::move(listener.socket));
    int configured = 0;
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.on_data([](auto &current) {
            current.send(tcp_test::bytes("A"));
            current.consume_input(current.input_data().size());
        });
    });
    server.start();
    auto a = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 1; });
    server.on_connection([&](auto &connection) {
        ++configured;
        connection.on_data([](auto &current) {
            current.send(tcp_test::bytes("B"));
            current.consume_input(current.input_data().size());
        });
    });
    auto b = listener.connect();
    tcp_test::drive(loop, [&] { return configured == 2; });
    ASSERT_EQ(::send(a.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    ASSERT_EQ(::send(b.get_fd(), "q", 1, MSG_NOSIGNAL), 1);
    std::string ar, br;
    tcp_test::drive(loop, [&] {
        ar += peer_read(a);
        br += peer_read(b);
        return ar == "A" && br == "B";
    });
}

TEST(TcpServerTest, TwoSlowEchoClientsPumpRetainedInput)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::TcpServerOptions options;
    options.connection.input_limit = 8;
    options.connection.output_limit = 4;
    options.connection.output_low_watermark = 0;
    snet::TcpServer server(loop, std::move(listener.socket), options);
    int closed = 0, pauses = 0;
    server.on_connection([&](auto &connection) {
        auto eof = std::make_shared<bool>(false);
        auto pump = [&, eof](auto &current) {
            auto result = current.send(current.input_data());
            current.consume_input(result.accepted_bytes);
            if (!current.input_data().empty()) {
                ++pauses;
                current.pause_reading();
            } else {
                current.resume_reading();
                if (*eof)
                    current.finish_sending();
            }
        };
        connection.on_data(pump);
        connection.on_output_available(pump);
        connection.on_eof([eof, pump](auto &current) {
            *eof = true;
            pump(current);
        });
        connection.on_closed([&](auto &, std::error_code reason) {
            EXPECT_FALSE(reason);
            ++closed;
        });
    });
    server.start();
    auto a = listener.connect(), b = listener.connect();
    const std::string first(129, 'A'), second(131, 'B');
    ASSERT_EQ(::send(a.get_fd(), first.data(), first.size(), MSG_NOSIGNAL), static_cast<ssize_t>(first.size()));
    ASSERT_EQ(::send(b.get_fd(), second.data(), second.size(), MSG_NOSIGNAL), static_cast<ssize_t>(second.size()));
    tcp_test::check(::shutdown(a.get_fd(), SHUT_WR), "shutdown");
    tcp_test::check(::shutdown(b.get_fd(), SHUT_WR), "shutdown");
    std::string ar, br;
    bool aeof = false, beof = false;
    tcp_test::drive(loop, [&] {
        ar += peer_read(a, &aeof);
        br += peer_read(b, &beof);
        return closed == 2 && aeof && beof;
    });
    EXPECT_EQ(ar, first);
    EXPECT_EQ(br, second);
    EXPECT_GT(pauses, 0);
}

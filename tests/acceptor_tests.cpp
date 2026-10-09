#include "acceptor_test_utils.hpp"
#include <gtest/gtest.h>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<snet::Acceptor>);
static_assert(!std::is_move_constructible_v<snet::Acceptor>);
static_assert(!std::is_copy_assignable_v<snet::Acceptor>);
static_assert(!std::is_move_assignable_v<snet::Acceptor>);

TEST(AcceptorTest, InvalidBudgetClosesListener)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    EXPECT_THROW(snet::Acceptor(loop, std::move(listener.socket), {0}), std::invalid_argument);
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
TEST(AcceptorTest, EmptySocketRejected)
{
    snet::EventLoop loop;
    EXPECT_THROW(snet::Acceptor(loop, snet::Socket{}), std::invalid_argument);
}
TEST(AcceptorTest, StartRequiresReceiverAndRetainsSocket)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    EXPECT_THROW(acceptor.start(), std::logic_error);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    acceptor.on_accept([](snet::Socket) {});
    EXPECT_NO_THROW(acceptor.start());
    EXPECT_THROW(acceptor.start(), std::logic_error);
}
TEST(AcceptorTest, StartAfterCloseRejected)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    acceptor.on_accept([](snet::Socket) {});
    acceptor.close();
    EXPECT_THROW(acceptor.start(), std::logic_error);
    EXPECT_THROW(acceptor.resume_accepting(), std::logic_error);
    EXPECT_NO_THROW(acceptor.pause_accepting());
}
TEST(AcceptorTest, DestructorClosesAndUnregisters)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    {
        snet::Acceptor acceptor(loop, std::move(listener.socket));
        acceptor.on_accept([](snet::Socket) {});
        acceptor.start();
    }
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    auto fresh = tcp_test::make_socket(AF_INET);
    snet::Channel channel(fresh.get_fd());
    channel.set_events(EPOLLIN);
    EXPECT_NO_THROW(loop.update_channel(&channel));
    loop.remove_channel(&channel);
}
TEST(AcceptorTest, CloseIsIdempotentAndCallsNoHandlers)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    int fd = listener.socket.get_fd();
    int callbacks = 0;
    {
        snet::Acceptor acceptor(loop, std::move(listener.socket));
        acceptor.on_accept([&](snet::Socket) { ++callbacks; });
        acceptor.on_error([&](auto &, std::error_code) { ++callbacks; });
        acceptor.start();
        acceptor.close();
        acceptor.close();
        EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
        accept_test::once(loop);
    }
    EXPECT_EQ(callbacks, 0);
}

namespace
{
void accepts_family(int family)
{
    snet::EventLoop loop;
    accept_test::Listener listener(family);
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    std::vector<snet::Socket> accepted, peers;
    acceptor.on_accept([&](snet::Socket socket) {
        EXPECT_NE(::fcntl(socket.get_fd(), F_GETFL) & O_NONBLOCK, 0);
        EXPECT_NE(::fcntl(socket.get_fd(), F_GETFD) & FD_CLOEXEC, 0);
        accepted.push_back(std::move(socket));
    });
    acceptor.start();
    for (int i = 0; i < 3; ++i)
        peers.push_back(listener.connect());
    tcp_test::drive(loop, [&] { return accepted.size() == 3; });
    EXPECT_NE(accepted[0].get_fd(), accepted[1].get_fd());
    EXPECT_NE(accepted[1].get_fd(), accepted[2].get_fd());
    for (auto &peer : peers)
        ASSERT_EQ(::send(peer.get_fd(), "x", 1, MSG_NOSIGNAL), 1);
    int received = 0;
    tcp_test::drive(loop, [&] {
        for (auto &socket : accepted) {
            char byte;
            auto n = ::recv(socket.get_fd(), &byte, 1, MSG_DONTWAIT);
            if (n == 1) {
                EXPECT_EQ(byte, 'x');
                ++received;
            }
        }
        return received == 3;
    });
    acceptor.close();
    for (auto &socket : accepted)
        EXPECT_NE(::fcntl(socket.get_fd(), F_GETFD), -1);
}
} // namespace
TEST(AcceptorTest, AcceptIpv4AndFlags) { accepts_family(AF_INET); }
TEST(AcceptorTest, AcceptIpv6AndFlags) { accepts_family(AF_INET6); }
TEST(AcceptorTest, ReceiverDeclinesClosesFd)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int accepted = -1;
    acceptor.on_accept([&](snet::Socket socket) { accepted = socket.get_fd(); });
    acceptor.start();
    auto peer = listener.connect();
    tcp_test::drive(loop, [&] { return accepted >= 0; });
    EXPECT_EQ(::fcntl(accepted, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
TEST(AcceptorTest, ReceiverThrowsClosesFdAndRestartDoesNotReplay)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int calls = 0, first_fd = -1;
    acceptor.on_accept([&](snet::Socket socket) {
        if (++calls == 1) {
            first_fd = socket.get_fd();
            throw std::runtime_error("receiver");
        }
    });
    acceptor.start();
    auto first = listener.connect(), second = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(::fcntl(first_fd, F_GETFD), -1);
    tcp_test::drive(loop, [&] { return calls == 2; });
}
TEST(AcceptorTest, ReceiverKeepsSocketThenThrows)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Socket saved;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    acceptor.on_accept([&](snet::Socket socket) {
        saved = std::move(socket);
        throw std::runtime_error("receiver");
    });
    acceptor.start();
    auto peer = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    EXPECT_TRUE(saved.is_open());
    EXPECT_NE(::fcntl(saved.get_fd(), F_GETFD), -1);
}
TEST(AcceptorTest, PauseInsideAcceptStopsGroup)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int calls = 0;
    acceptor.on_accept([&](snet::Socket) {
        ++calls;
        acceptor.pause_accepting();
    });
    acceptor.start();
    auto first = listener.connect(), second = listener.connect();
    accept_test::once(loop);
    EXPECT_EQ(calls, 1);
    accept_test::once(loop);
    EXPECT_EQ(calls, 1);
    acceptor.resume_accepting();
    accept_test::once(loop);
    EXPECT_EQ(calls, 2);
}
TEST(AcceptorTest, CloseInsideAcceptStopsGroup)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int calls = 0;
    acceptor.on_accept([&](snet::Socket) {
        ++calls;
        acceptor.close();
    });
    acceptor.start();
    auto first = listener.connect(), second = listener.connect();
    accept_test::once(loop);
    EXPECT_EQ(calls, 1);
    accept_test::once(loop);
    EXPECT_EQ(calls, 1);
}
TEST(AcceptorTest, ClearInsideAcceptRequiresNewHandlerAndExplicitResume)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int first = 0, next = 0;
    acceptor.on_accept([&](snet::Socket) {
        ++first;
        acceptor.on_accept({});
    });
    acceptor.start();
    auto a = listener.connect(), b = listener.connect();
    accept_test::once(loop);
    EXPECT_EQ(first, 1);
    EXPECT_THROW(acceptor.resume_accepting(), std::logic_error);
    acceptor.on_accept([&](snet::Socket) { ++next; });
    accept_test::once(loop);
    EXPECT_EQ(next, 0);
    acceptor.resume_accepting();
    tcp_test::drive(loop, [&] { return next == 1; });
}
TEST(AcceptorTest, BudgetOneLetsOtherWorkRun)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket), {1});
    int calls = 0;
    acceptor.on_accept([&](snet::Socket) { ++calls; });
    acceptor.start();
    auto a = listener.connect(), b = listener.connect(), c = listener.connect();
    accept_test::once(loop);
    EXPECT_EQ(calls, 1);
    tcp_test::drive(loop, [&] { return calls == 3; });
}

TEST(AcceptorTest, PauseResumeInsideReceiverKeepsCurrentHandlerInstalled)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int calls = 0;
    acceptor.on_accept([&](snet::Socket) {
        ++calls;
        acceptor.pause_accepting();
        EXPECT_NO_THROW(acceptor.resume_accepting());
    });
    acceptor.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return calls == 2; });
}

TEST(AcceptorTest, SelfReplaceReceiverRetainsCallable)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int first = 0, next = 0;
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    acceptor.on_accept([&, lifetime](snet::Socket) {
        ++first;
        acceptor.on_accept([&](snet::Socket) { ++next; });
        EXPECT_FALSE(weak.expired());
    });
    lifetime.reset();
    acceptor.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return first + next == 2; });
    EXPECT_EQ(first, 1);
    EXPECT_EQ(next, 1);
    EXPECT_TRUE(weak.expired());
}
TEST(AcceptorTest, MutableReceiverStatePersists)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    std::vector<int> values;
    acceptor.on_accept([&, count = 0](snet::Socket) mutable { values.push_back(++count); });
    acceptor.start();
    auto a = listener.connect(), b = listener.connect();
    tcp_test::drive(loop, [&] { return values.size() == 2; });
    EXPECT_EQ(values, (std::vector<int>{1, 2}));
}
TEST(AcceptorTest, ReceiverReplacementSurvivesThrow)
{
    snet::EventLoop loop;
    accept_test::Listener listener;
    snet::Acceptor acceptor(loop, std::move(listener.socket));
    int first = 0, next = 0;
    acceptor.on_accept([&](snet::Socket) {
        ++first;
        acceptor.on_accept([&](snet::Socket) { ++next; });
        throw std::runtime_error("receiver");
    });
    acceptor.start();
    auto a = listener.connect(), b = listener.connect();
    EXPECT_THROW(accept_test::once(loop), std::runtime_error);
    tcp_test::drive(loop, [&] { return next == 1; });
    EXPECT_EQ(first, 1);
}

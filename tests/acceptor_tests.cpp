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

#include "simplenet/Simplenet.hpp"

#include <cerrno>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <stdexcept>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

static_assert(!std::is_copy_constructible_v<snet::Socket>);
static_assert(!std::is_copy_assignable_v<snet::Socket>);
static_assert(std::is_nothrow_move_constructible_v<snet::Socket>);
static_assert(std::is_nothrow_move_assignable_v<snet::Socket>);
static_assert(std::is_nothrow_destructible_v<snet::Socket>);

namespace
{
// Cleanup also catches leaked descriptors when the implementation under test fails.
class SocketTest : public ::testing::Test
{
protected:
    int make_fd()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            throw std::runtime_error("test socket failed");
        try {
            descriptors_.push_back(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
        return fd;
    }
    void expect_closed(int fd)
    {
        errno = 0;
        EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
    }
    void TearDown() override
    {
        for (int fd : descriptors_)
            ::close(fd);
    }

private:
    std::vector<int> descriptors_;
};

TEST_F(SocketTest, EmptySocket)
{
    snet::Socket socket;
    EXPECT_FALSE(socket.is_open());
    EXPECT_EQ(socket.get_fd(), -1);
    EXPECT_EQ(socket.release(), -1);
    socket.close();
    EXPECT_FALSE(socket.is_open());
}

TEST_F(SocketTest, NegativeDescriptorRejected)
{
    EXPECT_THROW(snet::Socket(-1), std::invalid_argument);
    EXPECT_THROW(snet::Socket(-2), std::invalid_argument);
}

TEST_F(SocketTest, DestructorClosesDescriptor)
{
    int fd = make_fd();
    {
        snet::Socket socket(fd);
        EXPECT_TRUE(socket.is_open());
    }
    expect_closed(fd);
}

TEST_F(SocketTest, UnwindClosesDescriptor)
{
    int fd = make_fd();
    try {
        snet::Socket socket(fd);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error &) {
    }
    expect_closed(fd);
}

TEST_F(SocketTest, MoveConstructorTransfersOwnership)
{
    int fd = make_fd();
    snet::Socket source(fd);
    {
        snet::Socket destination(std::move(source));
        EXPECT_EQ(source.get_fd(), -1);
        EXPECT_FALSE(source.is_open());
        EXPECT_EQ(destination.get_fd(), fd);
        source.close();
        EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    }
    expect_closed(fd);
}

TEST_F(SocketTest, MoveAssignmentClosesDestination)
{
    int source_fd = make_fd();
    int old_fd = make_fd();
    snet::Socket source(source_fd);
    {
        snet::Socket destination(old_fd);
        EXPECT_EQ(&(destination = std::move(source)), &destination);
        EXPECT_EQ(source.get_fd(), -1);
        EXPECT_EQ(destination.get_fd(), source_fd);
        expect_closed(old_fd);
        source.close();
        EXPECT_NE(::fcntl(source_fd, F_GETFD), -1);
    }
    expect_closed(source_fd);
}

TEST_F(SocketTest, SelfMovePreservesOwnership)
{
    int fd = make_fd();
    snet::Socket socket(fd);
    auto *alias = &socket;
    socket = std::move(*alias);
    EXPECT_EQ(socket.get_fd(), fd);
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
}

TEST_F(SocketTest, EmptyMoves)
{
    snet::Socket empty;
    snet::Socket other(std::move(empty));
    EXPECT_FALSE(other.is_open());
    int fd = make_fd();
    snet::Socket destination(fd);
    destination = std::move(other);
    EXPECT_FALSE(destination.is_open());
    EXPECT_FALSE(other.is_open());
    expect_closed(fd);
    snet::Socket source(make_fd());
    int source_fd = source.get_fd();
    empty = std::move(source);
    EXPECT_EQ(empty.get_fd(), source_fd);
    EXPECT_FALSE(source.is_open());
}

TEST_F(SocketTest, ReleaseTransfersOwnership)
{
    int fd = make_fd();
    {
        snet::Socket socket(fd);
        EXPECT_EQ(socket.release(), fd);
        EXPECT_FALSE(socket.is_open());
        EXPECT_EQ(socket.release(), -1);
        socket.close();
    }
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(::close(fd), 0);
}

TEST_F(SocketTest, CloseIsIdempotent)
{
    int fd = make_fd();
    snet::Socket socket(fd);
    socket.close();
    EXPECT_FALSE(socket.is_open());
    EXPECT_EQ(socket.get_fd(), -1);
    expect_closed(fd);
    socket.close();
    EXPECT_EQ(socket.release(), -1);
}

TEST_F(SocketTest, CloseDoesNotTouchReusedDescriptor)
{
    int fd = make_fd();
    {
        snet::Socket socket(fd);
        socket.close();
        // Create a distinct socket and deliberately reuse the old descriptor number.
        int replacement = make_fd();
        ASSERT_EQ(::dup2(replacement, fd), fd);
        socket.close();
        EXPECT_NE(::fcntl(fd, F_GETFD), -1);
    }
    EXPECT_NE(::fcntl(fd, F_GETFD), -1);
}

TEST_F(SocketTest, ZeroDescriptorOwned)
{
    EXPECT_EXIT(
        {
            int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0 || ::dup2(fd, 0) != 0)
                ::_exit(1);
            if (fd != 0)
                ::close(fd);
            snet::Socket first(0);
            if (!first.is_open() || first.get_fd() != 0 || first.release() != 0)
                ::_exit(2);
            snet::Socket source(0);
            snet::Socket destination(std::move(source));
            if (source.is_open() || destination.get_fd() != 0)
                ::_exit(3);
            destination.close();
            errno = 0;
            if (::fcntl(0, F_GETFD) != -1 || errno != EBADF)
                ::_exit(4);
            ::_exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}
} // namespace

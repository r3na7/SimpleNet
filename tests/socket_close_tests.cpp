#include "simplenet/Socket.hpp"

#include <cerrno>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

extern "C" int __real_close(int fd);

namespace
{
int intercepted_fd = -1;
int injected_errno = 0;
int close_attempts = 0;

// Test-only seam: actually release the fd, then simulate a late Linux close error.
class CloseError
{
public:
    explicit CloseError(int error)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

        if (fd_ < 0)
            throw std::runtime_error("test socket failed");

        intercepted_fd = fd_;
        injected_errno = error;
        close_attempts = 0;
    }

    ~CloseError()
    {
        intercepted_fd = -1;

        if (::fcntl(fd_, F_GETFD) != -1)
            ::close(fd_);
    }

    int fd() const { return fd_; }

private:
    int fd_;
};

class SocketCloseErrorTest : public ::testing::TestWithParam<int>
{
};

TEST_P(SocketCloseErrorTest, CloseErrorIsNotRetried)
{
    CloseError error(GetParam());

    {
        snet::Socket socket(error.fd());

        socket.close();

        EXPECT_EQ(close_attempts, 1);
        EXPECT_EQ(socket.get_fd(), -1);
        EXPECT_FALSE(socket.is_open());
        errno = 0;

        EXPECT_EQ(::fcntl(error.fd(), F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
        socket.close();

        EXPECT_EQ(close_attempts, 1);
    }

    EXPECT_EQ(close_attempts, 1);
}

INSTANTIATE_TEST_SUITE_P(LinuxErrors, SocketCloseErrorTest, ::testing::Values(EINTR, EIO));

TEST(SocketCloseTest, ClosePreservesErrno)
{
    CloseError error(EINTR);
    snet::Socket socket(error.fd());

    errno = EDOM;
    socket.close();
    const int observed_errno = errno;

    EXPECT_EQ(observed_errno, EDOM);
    EXPECT_EQ(close_attempts, 1);
    EXPECT_FALSE(socket.is_open());
}

TEST(SocketCloseTest, DestructorPreservesErrno)
{
    CloseError error(EIO);

    {
        snet::Socket socket(error.fd());

        errno = ERANGE;
    }

    const int observed_errno = errno;

    EXPECT_EQ(observed_errno, ERANGE);
    EXPECT_EQ(close_attempts, 1);
}
} // namespace

extern "C" int __wrap_close(int fd)
{
    if (fd != intercepted_fd || intercepted_fd < 0)
        return __real_close(fd);

    ++close_attempts;
    __real_close(fd);
    errno = injected_errno;
    return -1;
}

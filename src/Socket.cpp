#include "Socket.hpp"

#include <cerrno>
#include <stdexcept>
#include <unistd.h>
#include <utility>

snet::Socket::Socket(int fd) : fd_(fd)
{
    if (fd < 0)
        throw std::invalid_argument("Socket descriptor must be nonnegative");
}

snet::Socket::~Socket() noexcept { close(); }

snet::Socket::Socket(Socket &&other) noexcept : fd_(other.release()) {}

snet::Socket &snet::Socket::operator=(Socket &&other) noexcept
{
    if (this != &other) {
        close();
        fd_ = other.release();
    }
    return *this;
}

int snet::Socket::get_fd() const noexcept { return fd_; }

bool snet::Socket::is_open() const noexcept { return fd_ >= 0; }

void snet::Socket::close() noexcept
{
    const int fd = release();
    if (fd >= 0) {
        const int saved_errno = errno;
        // Linux releases the descriptor even on EINTR; retry could close a reused fd.
        ::close(fd);
        errno = saved_errno;
    }
}

int snet::Socket::release() noexcept { return std::exchange(fd_, -1); }

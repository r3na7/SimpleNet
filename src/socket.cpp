#include "socket.hpp"

#include <unistd.h>

snet::Socket::Socket(int fd) noexcept : _fd(fd) {}

snet::Socket::Socket(Socket &&other) noexcept : _fd(other._fd)
{
    other._fd = -1;
}

snet::Socket &snet::Socket::operator=(Socket &&other) noexcept
{
    _fd = other._fd;
    other._fd = -1;    
}

int snet::Socket::fd() const noexcept
{
    return _fd;
}

snet::Socket::~Socket()
{
    if (_fd != -1) {
        close(_fd);
    }
}

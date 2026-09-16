#pragma once

namespace snet {


class Socket {
public:

    explicit Socket(int fd) noexcept;
    
    Socket(const Socket& other) = delete;
    
    Socket(Socket&& other) noexcept;

    
    Socket& operator=(const Socket& other) = delete;

    Socket& operator=(Socket&& other) noexcept;

    int fd() const noexcept;

    ~Socket();

private:
    int _fd;
};



} // namespace snet
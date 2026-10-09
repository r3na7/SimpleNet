#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <poll.h>
#include <simplenet/Simplenet.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace tcp_test
{
inline std::span<const char> bytes(std::string_view text) { return {text.data(), text.size()}; }

inline std::string text(std::span<const char> data)
{
    return data.empty() ? std::string{} : std::string(data.data(), data.size());
}

inline void check(int result, const char *call)
{
    if (result == -1)
        throw std::system_error(errno, std::system_category(), call);
}

inline snet::Socket make_socket(int family)
{
    int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);

    check(fd, "socket");
    return snet::Socket(fd);
}

struct Pair {
    snet::Socket peer;
    snet::Socket accepted;
    bool eof = false;

    explicit Pair(int family = AF_INET)
    {
        auto listener = make_socket(family);

        sockaddr_storage address{};
        socklen_t size;

        if (family == AF_INET) {
            sockaddr_in v4{};
            v4.sin_family = AF_INET;
            v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            size = sizeof(v4);
            std::memcpy(&address, &v4, size);
        } else {
            sockaddr_in6 v6{};
            v6.sin6_family = AF_INET6;
            v6.sin6_addr = in6addr_loopback;
            size = sizeof(v6);
            std::memcpy(&address, &v6, size);
        }

        check(::bind(listener.get_fd(), reinterpret_cast<sockaddr *>(&address), size), "bind");
        check(::getsockname(listener.get_fd(), reinterpret_cast<sockaddr *>(&address), &size), "getsockname");
        check(::listen(listener.get_fd(), 1), "listen");
        peer = make_socket(family);
        check(::connect(peer.get_fd(), reinterpret_cast<sockaddr *>(&address), size), "connect");
        int fd = ::accept4(listener.get_fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

        check(fd, "accept4");
        accepted = snet::Socket(fd);
    }

    void send(std::string_view data)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        while (!data.empty()) {
            auto n = ::send(peer.get_fd(), data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

            if (n > 0) {
                data.remove_prefix(static_cast<std::size_t>(n));
                continue;
            }

            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("peer send deadline");

                pollfd event{peer.get_fd(), POLLOUT, 0};
                check(::poll(&event, 1, 10), "poll");
                continue;
            }

            check(static_cast<int>(n), "send");
            throw std::runtime_error("zero peer send");
        }
    }

    std::string read()
    {
        std::string result;
        char data[4096];

        for (;;) {
            auto n = ::recv(peer.get_fd(), data, sizeof(data), MSG_DONTWAIT);

            if (n > 0) {
                result.append(data, static_cast<std::size_t>(n));
                continue;
            }

            if (n == 0) {
                eof = true;
                return result;
            }

            if (errno == EINTR)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return result;

            check(-1, "recv");
        }
    }
};

inline void drive(snet::EventLoop &loop, const std::function<bool()> &done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    snet::detail::LoopWork *action = nullptr;
    snet::detail::LoopWork tick(loop, [&] {
        if (done()) {
            loop.quit();
            return;
        }

        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("loop test deadline");

        action->schedule();
    });
    action = &tick;
    loop.set_timeout(0);
    tick.schedule();
    loop.loop();
}
} // namespace tcp_test

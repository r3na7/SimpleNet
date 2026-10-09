#pragma once
#include "tcp_test_utils.hpp"
#include <fcntl.h>

namespace accept_test
{
struct Listener {
    snet::Socket socket;
    sockaddr_storage address{};
    socklen_t size;
    explicit Listener(int family = AF_INET) : socket(tcp_test::make_socket(family))
    {
        tcp_test::check(::fcntl(socket.get_fd(), F_SETFL, O_NONBLOCK), "nonblock");
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
        tcp_test::check(::bind(socket.get_fd(), reinterpret_cast<sockaddr *>(&address), size), "bind");
        tcp_test::check(::getsockname(socket.get_fd(), reinterpret_cast<sockaddr *>(&address), &size), "getsockname");
        tcp_test::check(::listen(socket.get_fd(), 128), "listen");
    }
    snet::Socket connect()
    {
        auto peer = tcp_test::make_socket(address.ss_family);
        tcp_test::check(::connect(peer.get_fd(), reinterpret_cast<sockaddr *>(&address), size), "connect");
        return peer;
    }
};
inline void once(snet::EventLoop &loop)
{
    snet::detail::LoopWork stop(loop, [&] { loop.quit(); });
    loop.set_timeout(0);
    stop.schedule();
    loop.loop();
}
} // namespace accept_test

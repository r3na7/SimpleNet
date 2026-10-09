#include <array>
#include <cerrno>
#include <iostream>
#include <simplenet/Simplenet.hpp>
#include <sys/socket.h>
#include <system_error>
#include <utility>

int main()
{
    try {
        snet::Buffer bytes;
        bytes.append(std::array<char, 4>{'A', 'B', 'C', 'D'});
        bytes.consume(2);
        if (bytes.readable_size() != 2 || bytes.data()[0] != 'C' || bytes.data()[1] != 'D')
            return 2;
        const auto address = snet::Address::IPv4("127.0.0.1", 0);
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd == -1)
            throw std::system_error(errno, std::generic_category(), "socket");
        snet::Socket listener(fd);
        if (::bind(fd, address.data(), address.size()) == -1 || ::listen(fd, 8) == -1)
            throw std::system_error(errno, std::generic_category(), "listener setup");
        snet::EventLoop loop;
        snet::TcpServer server(loop, std::move(listener));
        server.on_connection([](snet::TcpConnection &) {});
        server.start();
        server.stop();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

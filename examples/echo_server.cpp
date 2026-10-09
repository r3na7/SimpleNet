#include "signal_stop.hpp"
#include <charconv>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace
{
constexpr const char *usage = "Usage: snet_echo_server [--host NUMERIC_IP] [--port 0..65535] [--help]\n";
struct Arguments {
    std::string host = "127.0.0.1";
    unsigned port = 5555;
    bool help = false;
};
Arguments parse(int argc, char **argv)
{
    Arguments result;
    bool host_seen = false, port_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help" && argc == 2) {
            result.help = true;
            return result;
        }
        if ((flag != "--host" && flag != "--port") || i + 1 == argc)
            throw std::invalid_argument("Unknown argument or missing value");
        const std::string_view value(argv[++i]);
        if (flag == "--host") {
            if (host_seen)
                throw std::invalid_argument("Duplicate host");
            host_seen = true;
            result.host = value;
        } else {
            if (port_seen)
                throw std::invalid_argument("Duplicate port");
            port_seen = true;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.port);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result.port > 65535)
                throw std::invalid_argument("Invalid port");
        }
    }
    return result;
}
void checked(int result, const char *operation)
{
    if (result == -1)
        throw std::system_error(errno, std::generic_category(), operation);
}
snet::Socket listen_socket(const snet::Address &address, unsigned &port)
{
    const int fd = ::socket(address.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    checked(fd, "socket");
    snet::Socket socket(fd);
    const int enabled = 1;
    checked(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)), "SO_REUSEADDR");
    if (address.family() == AF_INET6)
        checked(::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled)), "IPV6_V6ONLY");
    checked(::bind(fd, address.data(), address.size()), "bind");
    checked(::listen(fd, 128), "listen");
    sockaddr_storage actual{};
    socklen_t size = sizeof(actual);
    checked(::getsockname(fd, reinterpret_cast<sockaddr *>(&actual), &size), "getsockname");
    if (address.family() == AF_INET)
        port = ntohs(reinterpret_cast<const sockaddr_in *>(&actual)->sin_port);
    else
        port = ntohs(reinterpret_cast<const sockaddr_in6 *>(&actual)->sin6_port);
    return socket;
}
struct EchoState {
    bool eof = false, finishing = false;
};
void configure_echo(snet::TcpConnection &client)
{
    auto state = std::make_shared<EchoState>();
    auto pump = [state](snet::TcpConnection &current) {
        if (state->finishing)
            return;
        const auto sent = current.send(current.input_data());
        if (sent.status != snet::SendStatus::accepted && sent.status != snet::SendStatus::would_block)
            return;
        current.consume_input(sent.accepted_bytes);
        if (!current.input_data().empty()) {
            current.pause_reading();
        } else if (state->eof) {
            state->finishing = true;
            current.finish_sending();
        } else {
            current.resume_reading();
        }
    };
    client.on_data(pump);
    client.on_output_available(pump);
    client.on_eof([state, pump](auto &current) {
        state->eof = true;
        pump(current);
    });
    client.on_closed([](auto &, std::error_code error) {
        if (error)
            std::cerr << "Client closed: " << error.message() << '\n';
    });
}
} // namespace
int main(int argc, char **argv)
{
    Arguments args;
    std::unique_ptr<snet::Address> address;
    try {
        args = parse(argc, argv);
        if (args.help) {
            std::cout << usage;
            return 0;
        }
        address = std::make_unique<snet::Address>(args.host.find(':') == std::string::npos
                                                      ? snet::Address::IPv4(args.host, args.port)
                                                      : snet::Address::IPv6(args.host, args.port));
    } catch (const std::invalid_argument &error) {
        std::cerr << error.what() << '\n' << usage;
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    try {
        snet::EventLoop loop;
        snet::TcpServer *active = nullptr;
        demo::SignalStop signals(loop, [&] {
            if (active)
                active->stop();
            loop.quit();
        });
        auto listener = listen_socket(*address, args.port);
        snet::TcpServerOptions options;
        options.connection.input_limit = 64 * 1024;
        options.connection.output_limit = 8 * 1024;
        options.connection.output_low_watermark = 4 * 1024;
        snet::TcpServer server(loop, std::move(listener), options);
        active = &server;
        server.on_connection(configure_echo);
        server.on_accept_error(
            [](auto &, std::error_code error) { throw std::system_error(error, "Listener resource exhaustion"); });
        server.start();
        std::cout << "READY " << args.host << ' ' << args.port << std::endl;
        loop.loop();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

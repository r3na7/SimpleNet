#include <cerrno>
#include <iostream>
#include <memory>
#include <simplenet/Simplenet.hpp>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>

int main()
{
    try {
        auto check = [](int result) {
            if (result == -1)
                throw std::system_error(errno, std::generic_category());
            return result;
        };

        snet::Socket listener(check(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)));
        int reuse = 1;
        check(::setsockopt(listener.get_fd(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

        auto address = snet::Address::IPv4("127.0.0.1", 5555);
        check(::bind(listener.get_fd(), address.data(), address.size()));
        check(::listen(listener.get_fd(), 128));

        snet::EventLoop loop;
        snet::TcpServer server(loop, std::move(listener));
        server.on_connection([](snet::TcpConnection &client) {
            std::cout << "Client connected." << std::endl;
            auto eof = std::make_shared<bool>(false);

            auto echo = [eof](snet::TcpConnection &c) {
                auto sent = c.send(c.input_data());
                if (sent.status != snet::SendStatus::accepted && sent.status != snet::SendStatus::would_block)
                    return;

                c.consume_input(sent.accepted_bytes);
                if (!c.input_data().empty())
                    c.pause_reading();
                else if (*eof)
                    c.finish_sending();
                else
                    c.resume_reading();
            };

            client.on_data([echo](auto &c) {
                auto data = c.input_data();
                std::cout << "Received: " << std::string_view(data.data(), data.size()) << std::flush;
                echo(c);
            });
            client.on_output_available(echo);
            client.on_eof([eof, echo](auto &c) {
                *eof = true;
                echo(c);
            });
            client.on_closed([](auto &, std::error_code) { std::cout << "Client disconnected." << std::endl; });
        });
        server.on_accept_error([](auto &, std::error_code error) { throw std::system_error(error); });

        server.start();
        std::cout << "Server listening on 127.0.0.1:5555" << std::endl;
        loop.loop();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

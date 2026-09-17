#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <string>

namespace snet
{

class Address
{
public:

    static Address IPv4(std::string ip, uint16_t port);

    static Address IPv6(std::string ip, uint16_t port);

    static Address Unix(std::string path);

private:

    Address(const sockaddr* addr, socklen_t length);

    sockaddr_storage _address{};
    socklen_t _length = 0;
};

}
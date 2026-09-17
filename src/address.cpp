#include "address.hpp"

#include <cstdint>
#include <arpa/inet.h>
#include <sys/un.h>

#include <stdexcept>
#include <system_error>
#include <cstring>

snet::Address::Address(const sockaddr *addr, socklen_t length)
{

    if (addr == nullptr)
    {
        throw std::invalid_argument("Address pointer is null");
    }

    if (length > sizeof(_address))
    {
        throw std::length_error("Address too large");
    }

    std::memcpy(&_address, addr, length);
    _length = length;
}

snet::Address snet::Address::IPv4(std::string ip, uint16_t port)
{
    sockaddr_in ipv4_address{0};

    ipv4_address.sin_family = AF_INET;

    ipv4_address.sin_port = htons(port);

    int inet_pton_result = inet_pton(AF_INET, ip.c_str(), &ipv4_address.sin_addr);
    int error = errno;

    if (inet_pton_result == 0)
    {
        throw std::invalid_argument("Invalid IPv4 address");
    }
    else if (inet_pton_result == -1)
    {
        throw std::system_error(error, std::system_category(), "inet_pton");
    }

    return Address(reinterpret_cast<sockaddr *>(&ipv4_address), sizeof(sockaddr_in));
}

snet::Address snet::Address::IPv6(std::string ip, uint16_t port)
{
    sockaddr_in6 ipv6_address{0};

    ipv6_address.sin6_family = AF_INET6;

    ipv6_address.sin6_port = htons(port);

    int inet_pton_result = inet_pton(AF_INET6, ip.c_str(), &ipv6_address.sin6_addr);
    int error = errno;

    if (inet_pton_result == 0)
    {
        throw std::invalid_argument("Invalid IPv6 address");
    }
    else if (inet_pton_result == -1)
    {
        throw std::system_error(error, std::system_category(), "inet_pton");
    }

    return Address(reinterpret_cast<sockaddr *>(&ipv6_address), sizeof(sockaddr_in6));
}

snet::Address snet::Address::Unix(std::string path)
{
    sockaddr_un unix_address{0};

    unix_address.sun_family = AF_UNIX;

    if (path.size() >= sizeof(unix_address.sun_path))
    {
        throw std::invalid_argument("Unix address is too long");
    }

    std::memcpy(unix_address.sun_path, path.data(), path.size());

    unix_address.sun_path[path.size()] = '\0';

    socklen_t length = offsetof(sockaddr_un, sun_path) + path.size() + 1;

    return Address(reinterpret_cast<sockaddr *>(&unix_address), length);
}

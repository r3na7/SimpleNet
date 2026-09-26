#pragma once

#include <cstdint>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>

namespace snet
{

class Address
{
public:
    static Address IPv4(std::string ip, uint16_t port);

    static Address IPv6(std::string ip, uint16_t port);

    static Address Unix(std::string path);

    sa_family_t family() const noexcept;

    const sockaddr *data() const noexcept;

    socklen_t size() const noexcept;

private:
    Address(const sockaddr *addr, socklen_t length);

    sockaddr_storage address_{};
    socklen_t length_ = 0;
};

} // namespace snet
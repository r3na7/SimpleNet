#pragma once

/**
 * @file Address.hpp
 * @brief Socket address values for Linux system calls.
 */

#include <cstdint>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>

namespace snet
{

/**
 * @brief Stores a copy of an IPv4, IPv6, or Unix domain socket address.
 *
 * Contains sockaddr_storage and the actual address length. Does not own a socket
 * or perform DNS resolution, bind(), or connect(). Objects can be copied and
 * moved; each instance stores its own data. Ports are accepted in host byte order
 * and stored in network byte order.
 */
class Address
{
public:
    /**
     * @brief Creates an AF_INET address from a numeric IPv4 string.
     * @param ip Address in inet_pton() format, such as "127.0.0.1"; not a hostname.
     * @param port Port in host byte order; zero is allowed.
     * @return An address of length sizeof(sockaddr_in).
     * @throws std::invalid_argument The string is not an IPv4 address.
     * @throws std::system_error inet_pton() failed.
     */
    static Address IPv4(std::string ip, uint16_t port);

    /**
     * @brief Creates an AF_INET6 address from a numeric IPv6 string.
     * @param ip Address in inet_pton() format, such as "::1"; not a hostname.
     * @param port Port in host byte order; zero is allowed.
     * @return An address of length sizeof(sockaddr_in6), with zero flowinfo and scope_id.
     * @throws std::invalid_argument The string is not an IPv6 address.
     * @throws std::system_error inet_pton() failed.
     * @note Zone IDs in the string and scope_id configuration are not supported by this API.
     */
    static Address IPv6(std::string ip, uint16_t port);

    /**
     * @brief Copies path bytes into an AF_UNIX address and appends a null byte.
     * @param path Path bytes; the length must be less than sizeof(sockaddr_un::sun_path).
     * @return An address of length offsetof(sockaddr_un, sun_path) + path.size() + 1.
     * @throws std::invalid_argument The path is too long.
     * @note Path existence and accessibility are not checked; empty strings and embedded
     *       null bytes are not rejected. If the first byte is null, Linux interprets
     *       the address as abstract; the appended null byte is also part of its name.
     *       There is no separate API for abstract addresses.
     */
    static Address Unix(std::string path);

    /**
     * @brief Returns the address family.
     * @return AF_INET, AF_INET6, or AF_UNIX.
     */
    sa_family_t family() const noexcept;

    /**
     * @brief Provides an address for bind(), connect(), and similar calls.
     * @return A non-owning pointer to internal storage; pass it together with size().
     *         Valid until this object is destroyed; assignment may change the data.
     *         A copy of the object has its own storage.
     */
    const sockaddr *data() const noexcept;

    /**
     * @brief Returns the address length for system calls.
     * @return The number of meaningful bytes available through data().
     */
    socklen_t size() const noexcept;

private:
    Address(const sockaddr *addr, socklen_t length);

    sockaddr_storage address_{};
    socklen_t length_ = 0;
};

} // namespace snet
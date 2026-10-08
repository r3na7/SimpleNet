#pragma once

/**
 * @file Socket.hpp
 * @brief Exclusive ownership of an already-open Linux socket descriptor.
 */

namespace snet
{
/**
 * @brief A move-only owner of a socket descriptor; -1 represents an empty object.
 * @pre One owner per descriptor; no concurrent access to this object.
 * @warning Remove any associated Channel from its Poller before closing or destroying
 * this socket, or overwriting it through move assignment. This class never changes registrations.
 * @note Does not create sockets or change their flags. The caller supplies an open socket.
 */
class Socket
{
public:
    /// @brief Creates an empty owner with descriptor -1.
    Socket() noexcept = default;
    /**
     * @brief Takes exclusive ownership of an already-open descriptor.
     * @param fd A nonnegative socket descriptor, including zero.
     * @pre fd refers to an open socket and ownership is transferred exclusively on success.
     * @throws std::invalid_argument fd is negative; no ownership is acquired.
     * @note Does not validate the descriptor, socket type, or flags through system calls.
     */
    explicit Socket(int fd);
    /// @brief Closes the owned descriptor once, preserving errno; does nothing when empty.
    ~Socket() noexcept;
    /// @brief Exclusive ownership cannot be copied.
    Socket(const Socket &) = delete;
    /// @brief Exclusive ownership cannot be copy-assigned.
    Socket &operator=(const Socket &) = delete;
    /**
     * @brief Transfers ownership without moving the kernel resource.
     * @param other The source, left empty after transfer.
     */
    Socket(Socket &&other) noexcept;
    /**
     * @brief Closes the previous descriptor and takes the source descriptor; self-move has no effect.
     * @param other The source, left empty after transfer unless it is this object.
     * @return This object.
     * @pre The destination's old descriptor is not registered in a Poller.
     */
    Socket &operator=(Socket &&other) noexcept;
    /**
     * @brief Returns borrowed access to the descriptor without transferring ownership.
     * @return The owned descriptor, or -1 when empty.
     * @warning Do not close, replace, or independently adopt this descriptor.
     */
    int get_fd() const noexcept;
    /**
     * @brief Reports whether this wrapper holds a descriptor; does not query the kernel.
     * @return True when the stored descriptor is nonnegative.
     */
    bool is_open() const noexcept;
    /**
     * @brief Makes the object empty and attempts to close its descriptor at most once.
     * @pre Any associated Channel has been removed from its Poller.
     * @note Empty calls do nothing. Linux close errors, including EINTR, are not retried or
     * reported; errno is preserved. Closing does not confirm delivery of queued TCP data.
     */
    void close() noexcept;
    /**
     * @brief Relinquishes ownership without closing the descriptor or changing registrations.
     * @return The former descriptor, or -1 when empty; the recipient now owns its cleanup.
     */
    int release() noexcept;

private:
    int fd_ = -1;
};
} // namespace snet

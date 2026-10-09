#pragma once
/** @file Acceptor.hpp
 * @brief Bounded acceptance and ownership of an established TCP listening socket.
 */
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Socket.hpp"
#include <cstddef>
#include <functional>
#include <system_error>

namespace snet
{
/// @brief Immutable per-dispatch acceptance options.
struct AcceptorOptions {
    std::size_t accept_call_budget = 32; ///< Maximum accept4 attempts per dispatch; positive.
};
/** @brief Stable-address owner of a non-blocking TCP listening socket and Channel.
 * @pre Socket is exclusively owned, listening TCP IPv4/IPv6 and non-blocking.
 * @pre All operations use one loop thread; EventLoop outlives this object.
 * @warning Destruction inside this object's dispatch/callback is forbidden.
 */
class Acceptor
{
public:
    /// @brief Receiver of one exclusively owned accepted socket.
    using AcceptCallback = std::function<void(Socket)>;
    /// @brief Resource-error notification after acceptance has been paused.
    using ErrorCallback = std::function<void(Acceptor &, std::error_code)>;
    /** @brief Takes listener ownership without activating I/O.
     * @param loop Non-owning loop reference.
     * @param socket Already configured listening socket.
     * @param options Fixed positive acceptance budget.
     * @throws std::invalid_argument Empty socket or invalid options.
     */
    Acceptor(EventLoop &loop, Socket socket, AcceptorOptions options = {});
    /// @brief Unregisters and closes without application callbacks.
    ~Acceptor() noexcept;
    /// @brief Ownership cannot be copied.
    Acceptor(const Acceptor &) = delete;
    /// @brief Copy assignment is forbidden.
    Acceptor &operator=(const Acceptor &) = delete;
    /// @brief Registered addresses cannot move.
    Acceptor(Acceptor &&) = delete;
    /// @brief Move assignment is forbidden.
    Acceptor &operator=(Acceptor &&) = delete;
    /// @brief Sets/replaces/clears the receiver; clearing an active receiver pauses acceptance.
    /// @param callback New receiver; argument preparation may throw before changing the handler.
    void on_accept(AcceptCallback callback);
    /// @brief Sets/replaces/clears the resource-error handler; self-replacement is allowed.
    /// @param callback New handler; argument preparation may throw.
    void on_error(ErrorCallback callback);
    /// @brief Activates after a receiver is configured; failed registration permits retry.
    /// @throws std::logic_error Missing receiver, already activated, or closed.
    void start();
    /// @brief Removes registration, retaining listener; allowed before start.
    void pause_accepting();
    /// @brief Resumes with a receiver; failed registration leaves the object paused.
    /// @throws std::logic_error Missing receiver or closed object.
    void resume_accepting();
    /// @brief Idempotently unregisters and closes listener; never closes already handed-off sockets.
    /// @note Unexpected deregistration failure diagnoses and terminates.
    void close();

private:
    enum class State { created, active, closed };
    void detach() noexcept;
    void handle_accept();
    void handle_error();
    EventLoop &loop_;
    Socket socket_;
    const AcceptorOptions options_;
    Channel channel_;
    State state_ = State::created;
    bool paused_ = false, registered_ = false, servicing_ = false, callback_active_ = false;
    AcceptCallback accept_callback_;
    ErrorCallback error_callback_;
};
} // namespace snet

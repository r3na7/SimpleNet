#pragma once
/** @file TcpServer.hpp
 * @brief Single-threaded TCP component assembly and connection ownership.
 */
#include "Acceptor.hpp"
#include "TcpConnection.hpp"
#include "detail/LoopCleanup.hpp"
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace snet
{
/// @brief Immutable settings propagated to listener acceptance and each new connection.
struct TcpServerOptions {
    ConnectionOptions connection; ///< Queue limits and per-iteration I/O budgets.
    AcceptorOptions acceptor;     ///< Listening acceptance attempt budget.
};

/** @brief Stable-address owner of an Acceptor and its TcpConnection objects.
 * @pre Socket is exclusively owned, non-blocking listening TCP IPv4/IPv6.
 * @pre All use is in one thread; EventLoop outlives the server.
 * @warning Do not destroy from server/owned-connection dispatch, callbacks or cleanup.
 */
class TcpServer
{
public:
    /// @brief Configuration notification before server-controlled connection activation.
    using ConnectionCallback = std::function<void(TcpConnection &)>;
    /// @brief Resource-error notification after listener acceptance has paused.
    using AcceptErrorCallback = std::function<void(TcpServer &, std::error_code)>;
    /** @brief Takes listener ownership and prepares owner cleanup without activating acceptance.
     * @param loop Non-owning EventLoop reference.
     * @param socket Prepared listening socket.
     * @param options Valid connection and acceptance settings.
     * @throws std::invalid_argument Empty socket or invalid options.
     */
    TcpServer(EventLoop &loop, Socket socket, TcpServerOptions options = {});
    /// @brief Stops and releases owned resources without application callbacks.
    ~TcpServer() noexcept;
    /// @brief Ownership cannot be copied.
    TcpServer(const TcpServer &) = delete;
    /// @brief Copy assignment is forbidden.
    TcpServer &operator=(const TcpServer &) = delete;
    /// @brief Registered context addresses cannot move.
    TcpServer(TcpServer &&) = delete;
    /// @brief Move assignment is forbidden.
    TcpServer &operator=(TcpServer &&) = delete;
    /// @brief Replaces configuration for future clients; clearing an active handler pauses acceptance.
    /// @param callback New handler; preparation may throw before changing the old handler.
    void on_connection(ConnectionCallback callback);
    /// @brief Replaces/clears resource notification; self-replacement is supported.
    /// @param callback New handler; preparation may throw.
    void on_accept_error(AcceptErrorCallback callback);
    /// @brief Activates acceptance with a configuration callback; failed activation permits retry.
    /// @throws std::logic_error Missing configuration, repeated start or permanently stopped listener.
    void start();
    /// @brief Restores temporary acceptance pause; never restarts a closed listener.
    /// @throws std::logic_error Missing configuration or permanently stopped listener.
    void resume_accepting();
    /// @brief Permanently closes listener; existing clients continue.
    void stop_accepting();
    /// @brief Immediately closes current clients; destruction waits for safe owner cleanup.
    void close_connections();
    /// @brief Stops acceptance then closes clients; idempotent, never quits the shared loop.
    void stop();

private:
    template <class Function> struct Slot {
        Function callback;
        bool executing = false, replaced = false;
    };

    template <class Function> void replace(Slot<Function> &slot, Function callback) noexcept
    {
        if (slot.executing)
            slot.replaced = true;

        slot.callback = std::move(callback);
    }

    template <class Function, class... Args> void invoke(Slot<Function> &slot, Args &&...args)
    {
        if (!slot.callback)
            return;

        assert(!callback_active_);
        auto callable = std::move(slot.callback);

        slot.executing = true;
        slot.replaced = false;
        callback_active_ = true;
        auto restore = [&]() noexcept {
            if (!slot.replaced)
                slot.callback = std::move(callable);

            slot.executing = false;
            callback_active_ = false;
        };

        try {
            callable(std::forward<Args>(args)...);
        } catch (...) {
            restore();
            throw;
        }

        restore();
    }

    bool has_configuration() const noexcept;

    struct OwnedConnection {
        TcpServer *server;
        std::uint64_t id;
        std::unique_ptr<TcpConnection> connection;
        OwnedConnection *next_closed = nullptr;
        bool marked = false;

        OwnedConnection(TcpServer &owner, std::uint64_t value, std::unique_ptr<TcpConnection> client) noexcept
            : server(&owner), id(value), connection(std::move(client))
        {
        }
    };

    static void mark_closed(void *, TcpConnection &) noexcept;
    static TcpServerOptions checked_options(TcpServerOptions);

    static void cleanup_entry(void *, detail::CleanupReason) noexcept;
    void cleanup(detail::CleanupReason) noexcept;
    void accept(Socket);
    void accept_error(std::error_code);
    EventLoop &loop_;
    const TcpServerOptions options_;
    Acceptor acceptor_;
    std::unordered_map<std::uint64_t, OwnedConnection> connections_;
    OwnedConnection *closed_ = nullptr;
    std::uint64_t next_id_ = 1;
    bool started_ = false, stopped_ = false, servicing_ = false, callback_active_ = false;

    Slot<ConnectionCallback> connection_callback_;
    Slot<AcceptErrorCallback> error_callback_;
    detail::LoopCleanup cleanup_;
};
} // namespace snet

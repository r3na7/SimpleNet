#pragma once
/** @file TcpConnection.hpp
 * @brief Ownership and non-blocking service of an already-established TCP connection.
 */
#include "Buffer.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Socket.hpp"
#include "detail/LoopWork.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <system_error>
#include <utility>

namespace snet
{
/// @brief Immutable-per-connection useful-byte limits and per-iteration I/O quotas.
struct ConnectionOptions {
    std::size_t input_limit = 64 * 1024;          ///< Maximum unconsumed incoming bytes; positive.
    std::size_t output_limit = 64 * 1024;         ///< Maximum queued outgoing bytes; positive.
    std::size_t output_low_watermark = 32 * 1024; ///< Down-cross threshold; less than output_limit.
    std::size_t read_byte_budget = 64 * 1024;     ///< Maximum received bytes per iteration; positive.
    std::size_t read_call_budget = 16;            ///< Maximum receive attempts per iteration; positive.
    std::size_t write_byte_budget = 64 * 1024;    ///< Maximum sent bytes per iteration; positive.
    std::size_t write_call_budget = 16;           ///< Maximum send/shutdown attempts per iteration; positive.
};
/// @brief Result of accepting bytes into the library queue, not confirmation of peer delivery.
enum class SendStatus {
    accepted,         ///< All supplied bytes accepted.
    would_block,      ///< Only a prefix, possibly empty, accepted due to queue capacity.
    sending_finished, ///< No new bytes allowed after finish_sending.
    closed,           ///< Local socket is already closed.
    io_error          ///< A terminal socket error has been detected before final closure.
};
/// @brief Queue acceptance result; accepted_bytes is never greater than the source length.
struct SendResult {
    std::size_t accepted_bytes; ///< Copied prefix length; zero for terminal statuses.
    SendStatus status;          ///< Acceptance status.
    std::error_code error;      ///< First terminal reason for io_error; otherwise empty.
};

/**
 * @brief Stable-address owner of an established socket, its Channel, and two byte queues.
 * @pre The supplied socket is connected TCP IPv4/IPv6 and non-blocking; all use is in one loop thread.
 * @pre EventLoop outlives this object; the owner retains it through its callbacks and pending work.
 * @warning Destruction from its dispatch/callback/work is forbidden. No ownership is transferred to EventLoop.
 * @note Construction does not activate I/O. Channel handlers are internal; application handlers use on_*.
 */
class TcpConnection
{
public:
    /// @brief An application notification in the loop thread.
    using Callback = std::function<void(TcpConnection &)>;
    /// @brief Closure notification with an empty or preserved terminal socket error.
    using CloseCallback = std::function<void(TcpConnection &, std::error_code)>;
    /** @brief Takes ownership and prepares internal handlers without registering a Channel.
     * @param loop The non-owning loop reference.
     * @param socket An exclusively owned, established non-blocking TCP socket.
     * @param options Valid limits and budgets, fixed for this object's lifetime.
     * @throws std::invalid_argument Socket is empty or options are invalid.
     * @throws std::bad_alloc Preparation failed; acquired resources are released.
     */
    TcpConnection(EventLoop &loop, Socket socket, ConnectionOptions options = {});
    /// @brief Cancels work, unregisters, and closes without calling application handlers.
    /// @pre Not inside this object's callback, dispatch, or work action.
    ~TcpConnection() noexcept;
    /// @brief Connection ownership and registered addresses cannot be copied.
    TcpConnection(const TcpConnection &) = delete;
    /// @brief Connections cannot be copy-assigned.
    TcpConnection &operator=(const TcpConnection &) = delete;
    /// @brief Stable Channel and callback addresses cannot be moved.
    TcpConnection(TcpConnection &&) = delete;
    /// @brief Connections cannot be move-assigned.
    TcpConnection &operator=(TcpConnection &&) = delete;

    /** @brief Activates service after handlers and ownership have been configured.
     * @throws std::logic_error Already activated or closed.
     * @throws std::system_error Registration failed; the connection remains unactivated.
     * @throws std::bad_alloc Registration preparation failed; retry is allowed.
     */
    void start();
    /** @brief Copies an available source prefix and schedules internal sending, without synchronous I/O.
     * @param data Valid source bytes; copied accepted prefix need not survive the call.
     * @return Queue acceptance result; never a delivery acknowledgement.
     * @throws std::logic_error Not activated.
     * @throws std::bad_alloc Storage preparation failed; no new bytes accepted.
     * @throws std::length_error Accepted queue size cannot be represented by Buffer.
     */
    SendResult send(std::span<const char> data);
    /// @brief Provides borrowed access to unconsumed incoming bytes, including after closure.
    /// @return A span valid only until input modification or object destruction.
    std::span<const char> input_data() const noexcept;

    /** @brief Consumes incoming bytes and reconciles read capacity without synchronous receiving.
     * @param count Prefix length; zero does nothing.
     * @throws std::out_of_range Count exceeds available bytes; state remains unchanged.
     * @throws std::system_error Reactor interest reconciliation failed.
     * @throws std::bad_alloc Re-registration preparation failed.
     */
    void consume_input(std::size_t count);
    /// @brief Sets the application's read pause; no synchronous callback or recv.
    /// @throws std::system_error Reactor interest reconciliation failed.
    void pause_reading();
    /// @brief Clears only the application pause; EOF/full input still prevents reading.
    /// @throws std::system_error Reactor interest reconciliation failed.
    /// @throws std::bad_alloc Re-registration preparation failed.
    void resume_reading();
    /// @brief Rejects new output, schedules queue drain and shutdown(SHUT_WR); does not end incoming data.
    /// @throws std::logic_error Not activated.
    void finish_sending();
    /// @brief Immediately unregisters/closes, discards output, retains input, and defers one closure notification.
    /// @note Idempotent; unexpected deregistration failure diagnoses and terminates rather than leaving stale pointers.
    void close();
    /// @brief Sets/replaces/clears the data handler; never calls it synchronously; self-replacement allowed.
    /// @param callback The new handler; preparing its argument may allocate or throw.
    void on_data(Callback callback);
    /// @brief Sets the one-shot EOF handler; replacing it does not replay delivered EOF.
    /// @param callback The new handler; preparing its argument may allocate or throw.
    void on_eof(Callback callback);
    /// @brief Sets the output queue down-cross handler, not a raw EPOLLOUT handler.
    /// @param callback The new handler; preparing its argument may allocate or throw.
    void on_output_available(Callback callback);
    /// @brief Sets the one-shot closure handler; it is never called by a destructor or owner cleanup hook.
    /// @param callback The new handler; preparing its argument may allocate or throw.
    void on_closed(CloseCallback callback);

private:
    friend class TcpServer;
    static ConnectionOptions checked_options(ConnectionOptions options);

    enum class State { created, active, failing, closed };
    template <class Function> struct Slot {
        Function callback;
        bool executing = false;
        bool replaced = false;
    };

    template <class Function> void replace(Slot<Function> &slot, Function callback) noexcept
    {
        if (slot.executing)
            slot.replaced = true;

        slot.callback = std::move(callback);
    }

    template <class Function, class... Args> void invoke(Slot<Function> &slot, Args... args)
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
            callable(*this, args...);
        } catch (...) {
            restore();
            throw;
        }

        restore();
    }

    void sync_interest();
    void detach() noexcept;
    void close_impl(bool notify) noexcept;
    void handle_read(int pending_error = 0);
    void handle_write();
    void handle_error();
    void run_work();
    void drain_output();
    bool can_read() const noexcept;

    void mark_socket_error(int error) noexcept;
    void maybe_auto_close() noexcept;
    void refresh_budgets() noexcept;
    void fail_socket(int error) noexcept;
    bool ready_for_cleanup() const noexcept;

    void cancel_closed_work() noexcept;

    EventLoop &loop_;
    Socket socket_;
    const ConnectionOptions options_;
    Channel channel_;
    Buffer input_;
    Buffer output_;
    State state_ = State::created;
    bool registered_ = false;
    std::uint32_t applied_events_ = 0;
    bool read_paused_ = false;
    bool read_eof_ = false;
    bool eof_pending_ = false;
    bool eof_delivered_ = false;
    bool finish_requested_ = false;
    bool write_shutdown_ = false;
    bool write_blocked_ = false;
    bool output_pending_ = false;
    std::uint64_t budget_iteration_ = 0;
    std::size_t read_bytes_ = 0;
    std::size_t read_calls_ = 0;
    std::size_t write_bytes_ = 0;
    std::size_t write_calls_ = 0;
    bool servicing_ = false;
    bool callback_active_ = false;
    bool closed_pending_ = false;
    bool closed_delivered_ = false;
    std::error_code terminal_error_;

    Slot<Callback> data_callback_, eof_callback_, output_callback_;
    Slot<CloseCallback> closed_callback_;
    void *owner_context_ = nullptr;
    void (*owner_closed_)(void *, TcpConnection &) noexcept = nullptr;
    detail::LoopWork work_;
};
} // namespace snet

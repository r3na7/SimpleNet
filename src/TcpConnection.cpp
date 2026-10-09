#include "TcpConnection.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
snet::ConnectionOptions checked_options(snet::ConnectionOptions options)
{
    if (!options.input_limit || !options.output_limit || options.output_low_watermark >= options.output_limit ||
        !options.read_byte_budget || !options.read_call_budget || !options.write_byte_budget ||
        !options.write_call_budget)
        throw std::invalid_argument("Invalid connection limits or budgets");
    return options;
}
int checked_fd(const snet::Socket &socket)
{
    if (!socket.is_open())
        throw std::invalid_argument("Empty connection socket");
    return socket.get_fd();
}
struct FlagGuard {
    bool &flag;
    explicit FlagGuard(bool &f) : flag(f)
    {
        assert(!flag);
        flag = true;
    }
    ~FlagGuard() noexcept { flag = false; }
};
[[noreturn]] void del_failure(int fd, int error) noexcept
{
    char message[128];
    const char prefix[] = "TcpConnection EPOLL_CTL_DEL fd=";
    auto *at = message;
    std::memcpy(at, prefix, sizeof(prefix) - 1);
    at += sizeof(prefix) - 1;
    at = std::to_chars(at, message + sizeof(message) - 16, fd).ptr;
    const char suffix[] = " errno=";
    std::memcpy(at, suffix, sizeof(suffix) - 1);
    at += sizeof(suffix) - 1;
    at = std::to_chars(at, message + sizeof(message) - 1, error).ptr;
    *at++ = '\n';
    ::write(STDERR_FILENO, message, static_cast<std::size_t>(at - message));
    std::terminate();
}
} // namespace

snet::TcpConnection::TcpConnection(EventLoop &loop, Socket socket, ConnectionOptions options)
    : loop_(loop), socket_(std::move(socket)), options_(checked_options(options)), channel_(checked_fd(socket_)),
      work_(loop, [this] { run_work(); })
{
    channel_.set_read_callback([this] { handle_read(); });
    channel_.set_write_callback([this] { handle_write(); });
    channel_.set_error_callback([this] { handle_error(); });
}

snet::TcpConnection::~TcpConnection() noexcept
{
    assert(!servicing_ && !callback_active_ && !work_.executing());
    work_.cancel();
    close_impl(false);
}

void snet::TcpConnection::start()
{
    if (state_ != State::created)
        throw std::logic_error("Connection cannot be activated twice or after closure");
    const auto desired = read_paused_ ? 0u : static_cast<unsigned>(EPOLLIN | EPOLLRDHUP);
    if (desired) {
        channel_.set_events(desired);
        loop_.update_channel(&channel_);
        registered_ = true;
        applied_events_ = desired;
    }
    state_ = State::active;
}

void snet::TcpConnection::detach() noexcept
{
    if (!registered_)
        return;
    try {
        loop_.remove_channel(&channel_);
    } catch (const std::system_error &error) {
        del_failure(socket_.get_fd(), error.code().value());
    } catch (...) {
        del_failure(socket_.get_fd(), errno);
    }
    registered_ = false;
    applied_events_ = 0;
}

void snet::TcpConnection::sync_interest()
{
    if (state_ != State::active)
        return;
    std::uint32_t desired = read_paused_ ? 0u : static_cast<unsigned>(EPOLLIN | EPOLLRDHUP);
    if (write_blocked_ && !output_.empty())
        desired |= EPOLLOUT;
    if (!desired) {
        detach();
        return;
    }
    if (registered_ && desired == applied_events_)
        return;
    channel_.set_events(desired);
    loop_.update_channel(&channel_);
    registered_ = true;
    applied_events_ = desired;
}

void snet::TcpConnection::close_impl(bool notify) noexcept
{
    if (state_ == State::closed)
        return;
    state_ = State::closed;
    detach();
    socket_.close();
    output_.consume(output_.readable_size());
    output_pending_ = false;
    write_blocked_ = false;
    work_.cancel();
    if (notify) {
        closed_pending_ = true;
        if (owner_closed_)
            owner_closed_(owner_context_, *this);
        work_.schedule();
        loop_.request_cleanup();
    }
}

void snet::TcpConnection::close() { close_impl(true); }

void snet::TcpConnection::run_work()
{
    FlagGuard guard(servicing_);
    try {
        if (state_ == State::active && !write_blocked_)
            drain_output();
        sync_interest();
        if (closed_pending_) {
            work_.cancel();
            closed_pending_ = false;
            closed_delivered_ = true;
            invoke(closed_callback_, terminal_error_);
        } else if (state_ == State::active && output_pending_) {
            output_pending_ = false;
            invoke(output_callback_);
        }
        sync_interest();
        if (closed_pending_ ||
            (state_ == State::active &&
             (output_pending_ || (!write_blocked_ && (!output_.empty() || (finish_requested_ && !write_shutdown_))))))
            work_.schedule();
    } catch (...) {
        if (state_ == State::closed)
            cancel_closed_work();
        throw;
    }
}

bool snet::TcpConnection::ready_for_cleanup() const noexcept
{
    return state_ == State::closed && !servicing_ && !callback_active_ && !work_.pending() && !work_.executing();
}
void snet::TcpConnection::cancel_closed_work() noexcept
{
    if (state_ != State::closed)
        return;
    work_.cancel();
    closed_pending_ = false;
}

snet::SendResult snet::TcpConnection::send(std::span<const char> data)
{
    if (state_ == State::created)
        throw std::logic_error("Sending requires activation");
    if (state_ == State::closed)
        return {0, SendStatus::closed, {}};
    if (state_ == State::failing)
        return {0, SendStatus::io_error, terminal_error_};
    if (finish_requested_)
        return {0, SendStatus::sending_finished, {}};
    if (data.empty())
        return {0, SendStatus::accepted, {}};
    const auto accepted = std::min(data.size(), options_.output_limit - output_.readable_size());
    if (accepted != 0) {
        output_.append(data.first(accepted));
        work_.schedule();
    }
    return {accepted, accepted == data.size() ? SendStatus::accepted : SendStatus::would_block, {}};
}

void snet::TcpConnection::finish_sending()
{
    if (state_ == State::created)
        throw std::logic_error("Finishing requires activation");
    if (state_ != State::active || finish_requested_)
        return;
    finish_requested_ = true;
    work_.schedule();
}

std::span<const char> snet::TcpConnection::input_data() const noexcept { return input_.data(); }
void snet::TcpConnection::consume_input(std::size_t count) { input_.consume(count); }
void snet::TcpConnection::pause_reading()
{
    read_paused_ = true;
    sync_interest();
}
void snet::TcpConnection::resume_reading()
{
    read_paused_ = false;
    sync_interest();
}
void snet::TcpConnection::on_data(Callback callback) { replace(data_callback_, std::move(callback)); }
void snet::TcpConnection::on_eof(Callback callback) { replace(eof_callback_, std::move(callback)); }
void snet::TcpConnection::on_output_available(Callback callback) { replace(output_callback_, std::move(callback)); }
void snet::TcpConnection::on_closed(CloseCallback callback) { replace(closed_callback_, std::move(callback)); }
void snet::TcpConnection::handle_read() {}
void snet::TcpConnection::handle_write()
{
    if (state_ != State::active)
        return;
    FlagGuard guard(servicing_);
    write_blocked_ = false;
    drain_output();
    sync_interest();
}
void snet::TcpConnection::handle_error() {}

void snet::TcpConnection::refresh_budgets() noexcept
{
    const auto iteration = loop_.iteration_id();
    if (iteration == budget_iteration_)
        return;
    budget_iteration_ = iteration;
    write_bytes_ = write_calls_ = 0;
}

void snet::TcpConnection::fail_socket(int error) noexcept
{
    if (!terminal_error_)
        terminal_error_ = std::error_code(error, std::system_category());
    state_ = State::failing;
    close_impl(true);
}

void snet::TcpConnection::drain_output()
{
    if (state_ != State::active)
        return;
    refresh_budgets();
    while (!output_.empty() && write_calls_ < options_.write_call_budget && write_bytes_ < options_.write_byte_budget) {
        const auto queued = output_.readable_size();
        const auto size = std::min(queued, options_.write_byte_budget - write_bytes_);
        ++write_calls_;
        const auto n = ::send(socket_.get_fd(), output_.data().data(), size, MSG_NOSIGNAL);
        if (n > 0) {
            const auto written = static_cast<std::size_t>(n);
            write_bytes_ += written;
            output_.consume(written);
            if (queued > options_.output_low_watermark && output_.readable_size() <= options_.output_low_watermark) {
                output_pending_ = true;
                work_.schedule();
            }
            continue;
        }
        const int error = n == 0 ? EIO : errno;
        if (error == EINTR)
            continue;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            write_blocked_ = true;
            break;
        }
        fail_socket(error);
        return;
    }
    if (output_.empty() && finish_requested_ && !write_shutdown_) {
        while (write_calls_ < options_.write_call_budget) {
            ++write_calls_;
            if (::shutdown(socket_.get_fd(), SHUT_WR) == 0) {
                write_shutdown_ = true;
                break;
            }
            const int error = errno;
            if (error == EINTR)
                continue;
            fail_socket(error);
            return;
        }
    }
    if (!write_blocked_ && (!output_.empty() || (finish_requested_ && !write_shutdown_)))
        work_.schedule();
}

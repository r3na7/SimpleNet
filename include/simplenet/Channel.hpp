#pragma once

#include <cstdint>
#include <functional>

namespace snet
{

class Channel
{
public:
    Channel(int fd);

    Channel(const Channel&) = delete;
    Channel(Channel&&) = delete;

    int get_fd() const noexcept;
    uint32_t get_events() const noexcept;
    uint32_t get_revents() const noexcept;

    void set_events(uint32_t events) noexcept;
    void add_event(uint32_t event) noexcept;
    void remove_event(uint32_t event) noexcept;
    void set_revents(uint32_t events) noexcept;

    void set_read_callback(std::function<void()> callback);
    void set_write_callback(std::function<void()> callback);
    void set_error_callback(std::function<void()> callback);
    void set_close_callback(std::function<void()> callback);

    void handle_read();
    void handle_write();
    void handle_error();
    void handle_event();

    void stop_handling_current_event() noexcept;

private:
    int fd_ = -1;

    uint32_t events_ = 0;
    uint32_t revents_ = 0;

    std::function<void()> read_callback_;
    std::function<void()> write_callback_;
    std::function<void()> error_callback_;

    bool dispatch_cancelled_ = false;
};

} // namespace snet
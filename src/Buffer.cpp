#include "Buffer.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>

snet::Buffer::Buffer(Buffer &&other) noexcept
    : storage_(std::move(other.storage_)), read_pos_(std::exchange(other.read_pos_, 0)),
      write_pos_(std::exchange(other.write_pos_, 0))
{
}

snet::Buffer &snet::Buffer::operator=(Buffer &&other) noexcept
{
    if (this != &other) {
        storage_ = std::move(other.storage_);
        read_pos_ = std::exchange(other.read_pos_, 0);
        write_pos_ = std::exchange(other.write_pos_, 0);
    }

    return *this;
}

std::span<const char> snet::Buffer::data() const noexcept
{
    if (empty())
        return {};

    return {storage_.data() + read_pos_, readable_size()};
}

std::size_t snet::Buffer::readable_size() const noexcept { return write_pos_ - read_pos_; }

std::size_t snet::Buffer::writable_size() const noexcept { return storage_.size() - write_pos_; }

bool snet::Buffer::empty() const noexcept { return read_pos_ == write_pos_; }

void snet::Buffer::append(std::span<const char> bytes)
{
    if (bytes.empty())
        return;

    std::vector<char> staged;
    const auto current = data();
    const std::less<const char *> less;

    if (!current.empty() && !less(bytes.data(), current.data()) &&
        less(bytes.data(), current.data() + current.size())) {
        // Preparation can move or reallocate the very bytes being appended.
        staged.assign(bytes.begin(), bytes.end());
        bytes = staged;
    }

    auto tail = prepare_write(bytes.size());

    std::memcpy(tail.data(), bytes.data(), bytes.size());
    commit_write(bytes.size());
}

void snet::Buffer::consume(std::size_t count)
{
    if (count > readable_size())
        throw std::out_of_range("Cannot consume more bytes than are readable");

    if (count == 0)
        return;

    read_pos_ += count;

    if (empty())
        read_pos_ = write_pos_ = 0;
}

std::span<char> snet::Buffer::prepare_write(std::size_t count)
{
    if (count > writable_size()) {
        const auto readable = readable_size();
        const auto maximum = storage_.max_size();

        if (count > maximum - readable)
            throw std::length_error("Buffer preparation exceeds maximum storage size");

        const auto required = readable + count;

        if (required > storage_.size()) {
            const auto doubled = storage_.size() <= maximum / 2 ? storage_.size() * 2 : maximum;
            // Allocate before compaction so allocation failure preserves the useful sequence.
            storage_.resize(std::max(required, doubled));
        }

        if (readable != 0)
            std::memmove(storage_.data(), storage_.data() + read_pos_, readable);

        read_pos_ = 0;
        write_pos_ = readable;
    }

    if (storage_.empty())
        return {};

    return {storage_.data() + write_pos_, writable_size()};
}

void snet::Buffer::commit_write(std::size_t count)
{
    if (count > writable_size())
        throw std::out_of_range("Cannot commit more bytes than are writable");

    write_pos_ += count;
}

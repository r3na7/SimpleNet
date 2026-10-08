#pragma once

/**
 * @file Buffer.hpp
 * @brief A contiguous queue of bytes independent of sockets and Reactor policy.
 */

#include <cstddef>
#include <span>
#include <vector>

namespace snet
{
/**
 * @brief Stores a contiguous unread byte sequence with read and write positions.
 * @note Views borrow memory and must be obtained again after any modifying operation,
 * including preparation, assignment, and movement. Zero-count operations are no-ops.
 * Copying creates independent storage; moving leaves the source logically empty.
 * @pre No concurrent access without external synchronization.
 */
class Buffer
{
public:
    /// @brief Creates an empty buffer without allocating storage.
    Buffer() noexcept = default;
    /// @brief Copies bytes and positions into independent storage; allocation may throw.
    Buffer(const Buffer &) = default;
    /// @brief Replaces this value with an independent copy; allocation may throw.
    /// @return This buffer.
    Buffer &operator=(const Buffer &) = default;
    /**
     * @brief Transfers storage and positions, leaving the source logically empty.
     * @param other The source buffer.
     */
    Buffer(Buffer &&other) noexcept;
    /**
     * @brief Replaces storage by transfer, leaving the source empty; self-move has no effect.
     * @param other The source buffer.
     * @return This buffer.
     */
    Buffer &operator=(Buffer &&other) noexcept;
    /**
     * @brief Provides read-only borrowed access to the unread sequence.
     * @return A contiguous span of readable_size() bytes; the empty pointer value is unspecified.
     */
    std::span<const char> data() const noexcept;
    /// @brief Returns the number of unconsumed bytes.
    /// @return The unread byte count.
    std::size_t readable_size() const noexcept;
    /// @brief Returns the existing writable tail size, excluding vector capacity beyond its elements.
    /// @return The number of constructed char elements after the write position.
    std::size_t writable_size() const noexcept;
    /// @brief Reports whether the useful sequence is empty.
    /// @return True when readable_size() is zero.
    bool empty() const noexcept;
    /**
     * @brief Copies all source bytes into the queue; an empty source has no effect.
     * @param bytes A valid readable span, external or a subrange of this buffer's current data().
     * @throws std::bad_alloc Storage or self-source staging allocation failed.
     * @throws std::length_error The combined useful sequence cannot fit in storage.
     * @note On failure, the old useful bytes and their order remain unchanged. A self-source
     * is copied before preparation; source views of consumed or writable storage are unsupported.
     */
    void append(std::span<const char> bytes);
    /**
     * @brief Marks a prefix as consumed without allocation or moving the remainder.
     * @param count The prefix length; zero has no effect.
     * @throws std::out_of_range count exceeds readable_size(); the buffer is unchanged.
     * @note Consuming all useful bytes resets both positions and retains storage for reuse.
     */
    void consume(std::size_t count);
    /**
     * @brief Prepares contiguous writable memory without adding useful bytes.
     * @param count The minimum writable size; zero does not modify storage or positions.
     * @return The entire writable tail, whose size is at least count.
     * @throws std::bad_alloc Growth allocation failed; useful bytes are preserved.
     * @throws std::length_error The requested useful-plus-writable size exceeds max_size.
     * @note May compact or grow storage. Memory is prepared before any external I/O.
     */
    std::span<char> prepare_write(std::size_t count);
    /**
     * @brief Marks written tail bytes as useful, without allocating memory.
     * @param count The actual written byte count; zero has no effect.
     * @pre Those bytes were written into prepared memory with no intervening buffer modification.
     * @throws std::out_of_range count exceeds writable_size(); the buffer is unchanged.
     */
    void commit_write(std::size_t count);

private:
    std::vector<char> storage_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
};
} // namespace snet

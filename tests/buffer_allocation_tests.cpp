#include <algorithm>
#include <array>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <new>
#include <simplenet/Buffer.hpp>
#include <span>
#include <string>
#include <string_view>

namespace
{
bool measuring = false;
std::size_t allocations = 0;
std::size_t permitted = std::numeric_limits<std::size_t>::max();
void *allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t))
{
    if (measuring && ++allocations > permitted)
        throw std::bad_alloc();
    void *ptr = nullptr;
    if (alignment <= alignof(std::max_align_t))
        ptr = std::malloc(size ? size : 1);
    else if (posix_memalign(&ptr, alignment, size ? size : 1) != 0)
        ptr = nullptr;
    if (!ptr)
        throw std::bad_alloc();
    return ptr;
}
} // namespace
void *operator new(std::size_t n) { return allocate(n); }
void *operator new[](std::size_t n) { return allocate(n); }
void *operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void *operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n, static_cast<std::size_t>(a));
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    try {
        return allocate(n, static_cast<std::size_t>(a));
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }

namespace
{
class AllocationGate
{
public:
    explicit AllocationGate(std::size_t allow = std::numeric_limits<std::size_t>::max())
    {
        allocations = 0;
        permitted = allow;
        measuring = true;
    }
    ~AllocationGate() { measuring = false; }
    AllocationGate(const AllocationGate &) = delete;
    AllocationGate &operator=(const AllocationGate &) = delete;
};
std::span<const char> bytes(std::string_view text) { return {text.data(), text.size()}; }
std::string contents(const snet::Buffer &buffer)
{
    auto view = buffer.data();
    return view.empty() ? std::string{} : std::string(view.data(), view.size());
}
std::string fill_storage(snet::Buffer &buffer)
{
    auto tail = buffer.prepare_write(4);
    std::string fixture(tail.size(), 'Q');
    fixture.front() = 'A';
    fixture.back() = 'Z';
    std::copy(fixture.begin(), fixture.end(), tail.begin());
    buffer.commit_write(tail.size());
    return fixture;
}

TEST(BufferAllocationTest, DefaultAndConsumeCommitDoNotAllocate)
{
    snet::Buffer buffer;
    auto tail = buffer.prepare_write(4);
    ASSERT_GE(tail.size(), 4u);
    tail[0] = 'A';
    tail[1] = 'B';
    {
        AllocationGate gate(0);
        snet::Buffer empty;
        empty.append({});
        empty.consume(0);
        empty.commit_write(0);
        (void)empty.prepare_write(0);
        buffer.commit_write(2);
        buffer.consume(1);
        buffer.consume(0);
        buffer.commit_write(0);
        buffer.append({});
        (void)buffer.prepare_write(0);
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(contents(buffer), "B");
}

TEST(BufferAllocationTest, CompactionDoesNotAllocate)
{
    snet::Buffer buffer;
    auto tail = buffer.prepare_write(4);
    ASSERT_GE(tail.size(), 4u);
    std::fill(tail.begin(), tail.end(), 'x');
    tail[tail.size() - 2] = 'C';
    tail[tail.size() - 1] = 'D';
    buffer.commit_write(tail.size());
    buffer.consume(tail.size() - 2);
    const std::array<char, 2> extra{'E', 'F'};
    {
        AllocationGate gate(0);
        buffer.append(extra);
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(contents(buffer), "CDEF");
}

TEST(BufferAllocationTest, GeometricGrowthBoundsAllocations)
{
    snet::Buffer buffer;
    const std::array<char, 1> byte{'X'};
    {
        AllocationGate gate;
        for (int i = 0; i < 1024; ++i)
            buffer.append(byte);
    }
    EXPECT_LE(allocations, 32u);
    EXPECT_EQ(buffer.readable_size(), 1024u);
    EXPECT_EQ(contents(buffer), std::string(1024, 'X'));
}

TEST(BufferAllocationTest, PrepareFailurePreservesData)
{
    snet::Buffer buffer;
    const auto fixture = fill_storage(buffer);
    bool failed = false;
    try {
        AllocationGate gate(0);
        (void)buffer.prepare_write(1);
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(contents(buffer), fixture);
    buffer.append(bytes("!"));
    EXPECT_EQ(contents(buffer), fixture + "!");
}

TEST(BufferAllocationTest, AppendFailurePreservesData)
{
    snet::Buffer buffer;
    const auto fixture = fill_storage(buffer);
    bool failed = false;
    try {
        AllocationGate gate(0);
        buffer.append(bytes("!"));
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(contents(buffer), fixture);
    buffer.append(bytes("!"));
    EXPECT_EQ(contents(buffer), fixture + "!");
}

TEST(BufferAllocationTest, SelfAppendStagingFailure)
{
    snet::Buffer buffer;
    const auto fixture = fill_storage(buffer);
    bool failed = false;
    try {
        AllocationGate gate(0);
        buffer.append(buffer.data());
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(contents(buffer), fixture);
    buffer.append(bytes("!"));
    EXPECT_EQ(contents(buffer), fixture + "!");
}

TEST(BufferAllocationTest, AppendFailureAfterStaging)
{
    snet::Buffer buffer;
    const auto fixture = fill_storage(buffer);
    bool failed = false;
    try {
        AllocationGate gate(1);
        buffer.append(buffer.data());
    } catch (const std::bad_alloc &) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(contents(buffer), fixture);
    buffer.append(buffer.data());
    EXPECT_EQ(contents(buffer), fixture + fixture);
}
} // namespace

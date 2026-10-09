#include "simplenet/Simplenet.hpp"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(std::is_nothrow_default_constructible_v<snet::Buffer>);
static_assert(std::is_nothrow_move_constructible_v<snet::Buffer>);
static_assert(std::is_nothrow_move_assignable_v<snet::Buffer>);
static_assert(std::is_nothrow_destructible_v<snet::Buffer>);
static_assert(std::is_copy_constructible_v<snet::Buffer>);
static_assert(std::is_copy_assignable_v<snet::Buffer>);

namespace
{
std::span<const char> bytes(std::string_view text) { return {text.data(), text.size()}; }

std::string contents(const snet::Buffer &buffer)
{
    auto view = buffer.data();

    return view.empty() ? std::string{} : std::string(view.data(), view.size());
}

void full_tail_ending_cd(snet::Buffer &buffer)
{
    auto tail = buffer.prepare_write(4);

    ASSERT_GE(tail.size(), 4u);
    std::fill(tail.begin(), tail.end(), 'x');
    tail[tail.size() - 2] = 'C';
    tail[tail.size() - 1] = 'D';
    buffer.commit_write(tail.size());
    buffer.consume(tail.size() - 2);
}

TEST(BufferTest, EmptyBuffer)
{
    snet::Buffer buffer;

    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(buffer.data().empty());
    EXPECT_EQ(buffer.readable_size(), 0u);
    EXPECT_EQ(buffer.writable_size(), 0u);
    buffer.consume(0);
    buffer.append({});

    EXPECT_TRUE(buffer.prepare_write(0).empty());
    buffer.commit_write(0);

    EXPECT_TRUE(buffer.empty());
}

TEST(BufferTest, BinaryAppendAndConsume)
{
    snet::Buffer buffer;
    const std::array<char, 4> input{'A', '\0', 'B', 'C'};

    buffer.append(input);

    EXPECT_FALSE(buffer.empty());
    ASSERT_EQ(buffer.readable_size(), 4u);
    EXPECT_EQ(contents(buffer), std::string("A\0BC", 4));
    buffer.consume(2);

    EXPECT_EQ(contents(buffer), "BC");
    buffer.consume(2);

    EXPECT_TRUE(buffer.empty());
    buffer.append(bytes("XY"));

    EXPECT_EQ(contents(buffer), "XY");
}

TEST(BufferTest, PartialCommit)
{
    snet::Buffer buffer;
    auto tail = buffer.prepare_write(8);

    ASSERT_GE(tail.size(), 8u);
    std::copy_n("abc", 3, tail.data());

    EXPECT_EQ(buffer.readable_size(), 0u);
    buffer.commit_write(3);

    EXPECT_EQ(contents(buffer), "abc");
    auto next = buffer.prepare_write(2);

    std::copy_n("de", 2, next.data());

    EXPECT_EQ(contents(buffer), "abc");
    buffer.commit_write(2);

    EXPECT_EQ(contents(buffer), "abcde");
}

TEST(BufferTest, InvalidCountsPreserveData)
{
    snet::Buffer buffer;

    buffer.append(bytes("abc"));

    EXPECT_THROW(buffer.consume(4), std::out_of_range);
    EXPECT_EQ(contents(buffer), "abc");
    EXPECT_THROW(buffer.commit_write(buffer.writable_size() + 1), std::out_of_range);
    EXPECT_EQ(contents(buffer), "abc");
    EXPECT_THROW(buffer.consume(std::numeric_limits<std::size_t>::max()), std::out_of_range);
    EXPECT_THROW(buffer.commit_write(std::numeric_limits<std::size_t>::max()), std::out_of_range);
    EXPECT_EQ(contents(buffer), "abc");
}

TEST(BufferTest, OversizedPreparationPreservesData)
{
    snet::Buffer buffer;

    buffer.append(bytes("abc"));

    EXPECT_THROW(buffer.prepare_write(std::numeric_limits<std::size_t>::max()), std::length_error);
    EXPECT_EQ(contents(buffer), "abc");
    buffer.append(bytes("d"));

    EXPECT_EQ(contents(buffer), "abcd");
}

TEST(BufferTest, ZeroOperationsPreserveView)
{
    snet::Buffer buffer;

    buffer.append(bytes("abc"));
    buffer.consume(1);
    auto view = buffer.data();

    buffer.append({});
    buffer.consume(0);
    buffer.commit_write(0);
    (void)buffer.prepare_write(0);

    EXPECT_EQ(buffer.data().data(), view.data());
    EXPECT_EQ(contents(buffer), "bc");
}

TEST(BufferTest, CompactionPreservesOrder)
{
    snet::Buffer buffer;

    ASSERT_NO_FATAL_FAILURE(full_tail_ending_cd(buffer));
    buffer.append(bytes("EF"));

    EXPECT_EQ(contents(buffer), "CDEF");
}

TEST(BufferTest, GrowthAfterConsumePreservesOrder)
{
    snet::Buffer buffer;

    ASSERT_NO_FATAL_FAILURE(full_tail_ending_cd(buffer));
    auto tail = buffer.prepare_write(1024);

    ASSERT_GE(tail.size(), 1024u);
    EXPECT_EQ(contents(buffer), "CD");
    tail[0] = 'E';
    buffer.commit_write(1);

    EXPECT_EQ(contents(buffer), "CDE");
}

TEST(BufferTest, SelfAppendCompacts)
{
    snet::Buffer buffer;

    ASSERT_NO_FATAL_FAILURE(full_tail_ending_cd(buffer));
    buffer.append(buffer.data());

    EXPECT_EQ(contents(buffer), "CDCD");
}

TEST(BufferTest, SelfAppendGrows)
{
    snet::Buffer buffer;
    auto tail = buffer.prepare_write(4);

    ASSERT_GE(tail.size(), 4u);
    std::string fixture(tail.size(), 'Q');

    fixture.front() = 'A';
    fixture.back() = 'Z';
    std::copy(fixture.begin(), fixture.end(), tail.begin());
    buffer.commit_write(tail.size());
    buffer.append(buffer.data());

    EXPECT_EQ(contents(buffer), fixture + fixture);
}

TEST(BufferTest, SelfSubrangeAppend)
{
    snet::Buffer buffer;

    buffer.append(bytes("ABCDE"));

    ASSERT_EQ(buffer.readable_size(), 5u);
    buffer.append(buffer.data().subspan(1, 3));

    EXPECT_EQ(contents(buffer), "ABCDEBCD");
}

TEST(BufferTest, IndependentCopy)
{
    snet::Buffer original;

    original.append(bytes("ABCDE"));
    original.consume(2);
    snet::Buffer constructed(original);
    snet::Buffer assigned;

    assigned.append(bytes("old"));
    assigned = original;
    original.consume(3);
    original.append(bytes("changed"));
    constructed.append(bytes("F"));

    EXPECT_EQ(contents(original), "changed");
    EXPECT_EQ(contents(constructed), "CDEF");
    EXPECT_EQ(contents(assigned), "CDE");
}

TEST(BufferTest, MoveLeavesReusableSource)
{
    snet::Buffer original;

    original.append(bytes("ABCDE"));
    original.consume(2);
    snet::Buffer moved(std::move(original));

    EXPECT_TRUE(original.empty());
    EXPECT_TRUE(original.data().empty());
    EXPECT_EQ(contents(moved), "CDE");
    original.append(bytes("new"));

    EXPECT_EQ(contents(original), "new");
    EXPECT_EQ(contents(moved), "CDE");
    snet::Buffer destination;

    destination.append(bytes("old"));
    destination = std::move(moved);

    EXPECT_EQ(contents(destination), "CDE");
    EXPECT_TRUE(moved.empty());
    moved.append(bytes("again"));

    EXPECT_EQ(contents(moved), "again");
    EXPECT_EQ(contents(destination), "CDE");
}

TEST(BufferTest, SelfMovePreservesData)
{
    snet::Buffer buffer;

    buffer.append(bytes("ABCDE"));
    buffer.consume(2);
    auto *alias = &buffer;

    buffer = std::move(*alias);

    EXPECT_EQ(contents(buffer), "CDE");
}

TEST(BufferTest, EmptyMoves)
{
    snet::Buffer empty;
    snet::Buffer constructed(std::move(empty));

    EXPECT_TRUE(constructed.empty());
    snet::Buffer destination;

    destination.append(bytes("old"));
    destination = std::move(constructed);

    EXPECT_TRUE(destination.empty());
    EXPECT_TRUE(constructed.empty());
    destination.append(bytes("new"));

    EXPECT_EQ(contents(destination), "new");
}
} // namespace

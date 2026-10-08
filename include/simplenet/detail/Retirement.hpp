#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace snet
{
class EventLoop;
namespace detail
{

/// @cond INTERNAL
class RetirementEntry
{
public:
    RetirementEntry(const RetirementEntry &) = delete;
    RetirementEntry &operator=(const RetirementEntry &) = delete;
    RetirementEntry(RetirementEntry &&) = delete;
    RetirementEntry &operator=(RetirementEntry &&) = delete;
    virtual ~RetirementEntry() noexcept = default;
    virtual bool can_destroy() const noexcept = 0;
    virtual void cancel_pending() noexcept = 0;

protected:
    RetirementEntry() = default;

private:
    friend class snet::EventLoop;
    RetirementEntry *next_ = nullptr;
};

/// @endcond

/**
 * @brief Separate prepared storage for deferred object ownership.
 * @tparam T A stable-address type with a noexcept destructor.
 * @note Created by EventLoop::prepare_retirement and transferred by EventLoop::retire.
 *       Empty slots can be destroyed before transfer without affecting the object.
 */
template <class T> class RetirementSlot final : public RetirementEntry
{
private:
    bool can_destroy() const noexcept override { return ready_(*object_); }
    void cancel_pending() noexcept override { cancel_(*object_); }

private:
    friend class snet::EventLoop;
    RetirementSlot(EventLoop &loop, bool (*ready)(const T &) noexcept, void (*cancel)(T &) noexcept)
        : loop_(&loop), ready_(ready), cancel_(cancel)
    {
        static_assert(std::is_nothrow_destructible_v<T>);
    }
    EventLoop *loop_;
    bool (*ready_)(const T &) noexcept;
    void (*cancel_)(T &) noexcept;
    std::unique_ptr<T> object_;
};
} // namespace detail
} // namespace snet

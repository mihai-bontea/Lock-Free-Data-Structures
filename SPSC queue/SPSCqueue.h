#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

// Cache line size used for padding. 64 fits most x86/ARM and Apple M-series
#ifndef SPSC_CACHE_LINE
#define SPSC_CACHE_LINE 64
#endif

/*
 * Bounded lock-free (wait-free) single-producer / single-consumer queue
 * - Exactly one thread may call the producer API (push / emplace)
 * - Exactly one thread may call the consumer API (pop / front / try_pop)
 * - Indices are monotonic counters (no wasted slot)
 * - slot = index & mask
 */
template <typename T, std::size_t Capacity>
class spsc_queue
{
    static_assert(Capacity >= 1 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_nothrow_destructible_v<T>);

    static constexpr std::size_t mask  = Capacity - 1;
    static constexpr std::size_t cache_line  = SPSC_CACHE_LINE;

    /*
     * Raw storage with the right size and alignment for a T, without creating a T in it.
     * (A union's members are never constructed or destroyed automatically
     * => doesn't require T to be default-constructible)
     */
    union Slot
    {
        Slot() noexcept {}
        ~Slot() {}
        T value;
    };

    /*
     * - Reading the other thread's index is the expensive part of every operation => caching is used
     * - A stale value is good enough; head only ever increases, at worst the producer thinks
     *   the queue is full when it isn't
     * - alignas(cache_line) is used to avoid false-sharing between producer and consumer
     */

    // Producer line: own index + cached view of consumer's index.
    alignas(cache_line) std::atomic<std::size_t> m_tail{0};
    std::size_t m_head_cache{0};

    // Consumer line: own index + cached view of producer's index.
    alignas(cache_line) std::atomic<std::size_t> m_head{0};
    std::size_t m_tail_cache{0};

    /*
     * alignas(cache_line) is used again, otherwise the first few slots would share the consumer's index line
     * (every time the producer writes into the slots, it invalidates the line holding 'head' in the consumer's cache)
     */
    alignas(cache_line) Slot m_slots[Capacity];

public:
    using value_type = T;

    spsc_queue() = default;
    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;

    ~spsc_queue()
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            const auto tail = m_tail.load(std::memory_order_relaxed);
            for (auto h = m_head.load(std::memory_order_relaxed); h != tail; ++h)
                m_slots[h & mask].value.~T();
        }
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    // ---------------- Producer ----------------

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args)
    noexcept(std::is_nothrow_constructible_v<T, Args&&...>)
    {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        if (tail - m_head_cache == Capacity)
        {
            m_head_cache = m_head.load(std::memory_order_acquire);
            if (tail - m_head_cache == Capacity) return false;
        }
        // If this throws, m_tail is untouched: queue stays consistent.
        ::new (static_cast<void*>(&m_slots[tail & mask].value)) T(std::forward<Args>(args)...);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        return try_emplace(v);
    }
    [[nodiscard]] bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        return try_emplace(std::move(v));
    }

    // ---------------- Consumer ----------------

    // Out-parameter form: move-assigns into an existing object, so callers can reuse its resources.
    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        T* p = front();
        if (!p) return false;
        out = std::move(*p);
        pop();
        return true;
    }

    // Zero-copy access: returns the oldest element or nullptr if empty. Valid until pop() is called.
    [[nodiscard]] T* front() noexcept
    {
        const auto head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail_cache)
        {
            m_tail_cache = m_tail.load(std::memory_order_acquire);
            if (head == m_tail_cache) return nullptr;
        }
        return &m_slots[head & mask].value;
    }

    // Destroys the front element. Precondition: front() != nullptr.
    void pop() noexcept
    {
        const auto head = m_head.load(std::memory_order_relaxed);
        m_slots[head & mask].value.~T();
        m_head.store(head + 1, std::memory_order_release);
    }

    // ---------------- observers ----------------

    [[nodiscard]] std::size_t size() const noexcept
    {
        // Read head first: head <= tail always holds, so the difference is never negative.
        const auto head = m_head.load(std::memory_order_acquire);
        const auto tail = m_tail.load(std::memory_order_acquire);
        const auto n = tail - head;
        return n > Capacity ? Capacity : n;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] bool full() const noexcept { return size() == Capacity; }
};
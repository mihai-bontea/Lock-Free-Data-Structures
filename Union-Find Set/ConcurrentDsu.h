#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

/*
 * Lock-free concurrent disjoint-set union (union-find)
 * - Any number of threads may call find / unite / same_set concurrently
 * - find and same_set are lock-free; unite is lock-free (a CAS only fails if another thread linked a root)
 * - No un-union / erase: the forest only ever grows, which is what makes this work
 *
 * Invariant: m_parent[x] always points to an ancestor of x, and ancestors stay ancestors forever
 * (unions only link a root under another node; non-roots never become roots again)
 * => path compression is just a monotone hint: racing or failed compression writes are harmless
 */
template <typename Index = std::uint32_t>
class concurrent_dsu
{
    static_assert(std::is_unsigned_v<Index>, "Index must be an unsigned integer type");
    static_assert(sizeof(Index) == 4 || sizeof(Index) == 8, "Index must be 32 or 64 bits");

    // On some 32-bit targets, std::atomic<std::uint64_t> isn't lock-free
    // The code would still be correct, but it would no longer be lock-free
    static_assert(std::atomic<Index>::is_always_lock_free);

    /*
     * Linking order: the root with the lower priority is linked under the higher one
     * - Any strict total order prevents cycles (two threads can't link r1 -> r2 and r2 -> r1)
     * - Plain index order degrades badly on sequential inputs (e.g. unite(i, i + 1) builds a chain)
     *   (structured inputs, like consecutive ids lead to long chains; hashing breaks the correlation)
     * - A bijective mixer gives a pseudo-random but fixed total order
     * - There are no collisions, every possible output value is produced by some input
     */
    [[nodiscard]] static constexpr Index priority(Index x) noexcept
    {
        if constexpr (sizeof(Index) == 4)
        {
            // murmur3 fmix32
            x ^= x >> 16; x *= 0x85ebca6bu;
            x ^= x >> 13; x *= 0xc2b2ae35u;
            x ^= x >> 16;
        }
        else
        {
            // splitmix64 finalizer
            x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27; x *= 0x94d049bb133111ebull;
            x ^= x >> 31;
        }
        return x;
    }

    // One atomic parent link per element, densely packed
    // Contention is concentrated on roots (the only CAS that matters for correctness)
    std::size_t m_size{0};
    std::unique_ptr<std::atomic<Index>[]> m_parent;

public:
    using index_type = Index;

    // Precondition: n - 1 is representable in Index
    explicit concurrent_dsu(std::size_t n)
            : m_size(n), m_parent(std::make_unique<std::atomic<Index>[]>(n))
    {
        for (std::size_t i = 0; i < n; ++i)
            m_parent[i].store(static_cast<Index>(i), std::memory_order_relaxed);
    }

    concurrent_dsu(const concurrent_dsu&) = delete;
    concurrent_dsu& operator=(const concurrent_dsu&) = delete;

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }


    // Returns a node that was the root of x's set at some point during the call
    // Uses path halving: x.parent <- x.grandparent, then jump to the grandparent
    [[nodiscard]] Index find(Index x) noexcept
    {
        while (true)
        {
            Index p = m_parent[x].load(std::memory_order_acquire);
            if (p == x) return x;

            const Index gp = m_parent[p].load(std::memory_order_acquire);
            if (p != gp)
            {
                // Failure means someone already moved x higher (or compressed it differently): ignore it
                // "if parent[x] is still p, set it to gp; otherwise do nothing"
                m_parent[x].compare_exchange_weak(p, gp, std::memory_order_release, std::memory_order_relaxed);
            }
            x = gp;
        }
    }


    // Merges the sets of a and b. Returns true if this call performed the merge,
    // false if they were already in the same set
    bool unite(Index a, Index b) noexcept
    {
        while (true)
        {
            a = find(a);
            b = find(b);
            if (a == b) return false;

            // Link the lower-priority root under the higher-priority one
            if (priority(a) > priority(b)) std::swap(a, b);

            // The only correctness-critical write: succeeds only if a is still a root
            Index expected = a;
            if (m_parent[a].compare_exchange_strong(expected, b, std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
                return true;
            // a got linked by another thread in the meantime: retry from the new roots
        }
    }

    /*
     * Linearizable membership query
     * - Naive find(a) == find(b) can return a false negative: a's root may be linked under
     *   b's root between the two finds
     * - If a is still a root after both finds, then at that moment a and b were in different sets
     */
    [[nodiscard]] bool same_set(Index a, Index b) noexcept
    {
        while (true)
        {
            a = find(a);
            b = find(b);
            if (a == b) return true;
            if (m_parent[a].load(std::memory_order_acquire) == a) return false;
        }
    }
};
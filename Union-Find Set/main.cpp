#include "ConcurrentDsu.h"

#include <cstdio>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

// Sequential reference
struct seq_dsu
{
    std::vector<std::uint32_t> p;
    explicit seq_dsu(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    std::uint32_t find(std::uint32_t x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }
    void unite(std::uint32_t a, std::uint32_t b) { p[find(a)] = find(b); }
};

int main()
{
    constexpr std::size_t n = 1 << 20;
    constexpr std::size_t edges = 1 << 21;
    constexpr unsigned threads = 8;

    std::mt19937_64 rng(42);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> e(edges);
    for (auto& [a, b] : e) { a = rng() % n; b = rng() % n; }
    // Also some sequential chains (adversarial for index linking)
    for (std::size_t i = 0; i + 1 < 100000; ++i) e[i] = {std::uint32_t(i), std::uint32_t(i + 1)};

    concurrent_dsu<> dsu(n);
    std::vector<std::thread> pool;
    std::atomic<std::size_t> merges{0};
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            std::size_t local = 0;
            for (std::size_t i = t; i < edges; i += threads)
            {
                local += dsu.unite(e[i].first, e[i].second);
                (void)dsu.same_set(e[i].first, e[(i * 7) % edges].second);
            }
            merges += local;
        });
    for (auto& th : pool) th.join();

    seq_dsu ref(n);
    for (auto [a, b] : e) ref.unite(a, b);

    std::size_t ref_sets = 0, sets = 0, mismatches = 0;
    for (std::uint32_t i = 0; i < n; ++i)
    {
        ref_sets += ref.find(i) == i;
        sets += dsu.find(i) == i;
    }
    for (std::size_t i = 0; i < edges; ++i)
    {
        auto a = e[i].first, b = e[(i * 13) % edges].second;
        mismatches += (ref.find(a) == ref.find(b)) != dsu.same_set(a, b);
    }
    std::printf("sets=%zu ref_sets=%zu merges=%zu (expect %zu) mismatches=%zu\n",
                sets, ref_sets, merges.load(), n - ref_sets, mismatches);
    return (sets == ref_sets && merges == n - ref_sets && mismatches == 0) ? 0 : 1;
}
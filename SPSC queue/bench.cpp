// Benchmark: lock-free spsc_queue vs. mutex-guarded ring buffer.
#include "SPSCqueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// Same bounded ring, guarded by one std::mutex
template <typename T, std::size_t Capacity>
class mutex_queue
{
    static_assert((Capacity & (Capacity - 1)) == 0);
    static constexpr std::size_t mask = Capacity - 1;

    std::mutex m_mutex;
    std::size_t m_head{0};
    std::size_t m_tail{0};
    T m_slots[Capacity]{};

public:
    [[nodiscard]] bool try_push(const T& v)
    {
        std::lock_guard lock(m_mutex);
        if (m_tail - m_head == Capacity) return false;
        m_slots[m_tail++ & mask] = v;
        return true;
    }

    [[nodiscard]] bool try_pop(T& out)
    {
        std::lock_guard lock(m_mutex);
        if (m_head == m_tail) return false;
        out = m_slots[m_head++ & mask];
        return true;
    }
};

using clock_type = std::chrono::steady_clock;
constexpr std::size_t queue_capacity = 1024;

static int g_cpu_producer = -1;
static int g_cpu_consumer = -1;

static void pin_to_cpu(int cpu)
{
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: failed to pin to cpu %d\n", cpu);
#else
    (void)cpu;
#endif
}

static inline void cpu_relax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// Throughput: producer pushes `count` ints as fast as possible, consumer pops and verifies order.
template <typename Queue>
double run_throughput(std::size_t count)
{
    auto q = std::make_unique<Queue>();
    std::atomic<bool> start{false};

    std::thread consumer([&]
    {
        pin_to_cpu(g_cpu_consumer);
        while (!start.load(std::memory_order_acquire)) cpu_relax();
        std::uint64_t v;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            while (!q->try_pop(v)) cpu_relax();
            if (v != i) { std::fprintf(stderr, "order violation\n"); std::abort(); }
        }
    });

    pin_to_cpu(g_cpu_producer);
    const auto t0 = clock_type::now();
    start.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < count; ++i)
        while (!q->try_push(i)) cpu_relax();
    consumer.join();
    const auto t1 = clock_type::now();

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(count) / sec / 1e6;
}

// Latency: ping-pong over two queues; one message in flight at a time.
// Returns mean round-trip time in nanoseconds.
template <typename Queue>
double run_round_trip(std::size_t iterations)
{
    auto ping = std::make_unique<Queue>();
    auto pong = std::make_unique<Queue>();

    std::thread echo([&]
    {
        pin_to_cpu(g_cpu_consumer);
        std::uint64_t v;
        for (std::size_t i = 0; i < iterations; ++i)
        {
            while (!ping->try_pop(v)) cpu_relax();
            while (!pong->try_push(v)) cpu_relax();
        }
    });

    pin_to_cpu(g_cpu_producer);
    std::uint64_t v;
    const auto t0 = clock_type::now();
    for (std::uint64_t i = 0; i < iterations; ++i)
    {
        while (!ping->try_push(i)) cpu_relax();
        while (!pong->try_pop(v)) cpu_relax();
    }
    const auto t1 = clock_type::now();
    echo.join();

    return std::chrono::duration<double, std::nano>(t1 - t0).count()
           / static_cast<double>(iterations);
}

template <typename F>
double median_of(int runs, F&& f)
{
    std::vector<double> r;
    for (int i = 0; i < runs; ++i) r.push_back(f());
    std::sort(r.begin(), r.end());
    return r[r.size() / 2];
}

int main(int argc, char** argv)
{
    if (argc == 3)
    {
        g_cpu_producer = std::atoi(argv[1]);
        g_cpu_consumer = std::atoi(argv[2]);
    }

    constexpr int runs = 7;
    constexpr std::size_t tp_count = 20'000'000;
    constexpr std::size_t rt_count = 2'000'000;

    using lf = spsc_queue<std::uint64_t, queue_capacity>;
    using mx = mutex_queue<std::uint64_t, queue_capacity>;

    // Warm-up
    run_throughput<lf>(1'000'000);
    run_throughput<mx>(1'000'000);

    const double tp_lf = median_of(runs, [&] { return run_throughput<lf>(tp_count); });
    const double tp_mx = median_of(runs, [&] { return run_throughput<mx>(tp_count); });
    const double rt_lf = median_of(runs, [&] { return run_round_trip<lf>(rt_count); });
    const double rt_mx = median_of(runs, [&] { return run_round_trip<mx>(rt_count); });

    std::printf("capacity %zu, median of %d runs, cpus %d/%d\n\n",
                queue_capacity, runs, g_cpu_producer, g_cpu_consumer);
    std::printf("%-14s %18s %18s\n", "", "throughput Mops/s", "round-trip ns");
    std::printf("%-14s %18.1f %18.1f\n", "spsc_queue", tp_lf, rt_lf);
    std::printf("%-14s %18.1f %18.1f\n", "mutex_queue", tp_mx, rt_mx);
    std::printf("\nspeedup: %.1fx throughput, %.1fx latency\n", tp_lf / tp_mx, rt_mx / rt_lf);
}
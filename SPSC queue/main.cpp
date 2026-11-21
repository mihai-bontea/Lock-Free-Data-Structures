#include "SPSCqueue.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

struct NoDefault
{
    explicit NoDefault(int v) : v(v) {}
    int v;
};

int main()
{
    // Basic semantics, full capacity usable.
    {
        spsc_queue<int, 4> q;
        assert(q.empty());
        for (int i = 0; i < 4; ++i) assert(q.try_push(i));
        assert(q.full() && !q.try_push(99));
        int x;
        for (int i = 0; i < 4; ++i) { assert(q.try_pop(x) && x == i); }
        assert(!q.try_pop(x) && q.empty());
    }
    // Non-default-constructible T, front/pop.
    {
        spsc_queue<NoDefault, 2> q;
        assert(q.try_emplace(7));
        assert(q.front() && q.front()->v == 7);
        q.pop();
        assert(!q.front());
    }
    // Leftover elements destroyed.
    {
        auto p = std::make_shared<int>(1);
        { spsc_queue<std::shared_ptr<int>, 8> q; assert(q.try_push(p)); assert(q.try_push(p)); }
        assert(p.use_count() == 1);
    }
    // Concurrent stress: ordering + no data races (run under TSan).
    {
        constexpr std::size_t N = 2'000'000;
        auto q = std::make_unique<spsc_queue<std::string, 1024>>();
        std::thread prod([&] {
            for (std::size_t i = 0; i < N; ++i)
                while (!q->try_emplace(std::to_string(i))) {}
        });
        std::string s;
        for (std::size_t i = 0; i < N; ++i)
        {
            while (!q->try_pop(s)) {}
            if (s != std::to_string(i)) { std::printf("FAIL at %zu\n", i); return 1; }
        }
        prod.join();
        assert(q->empty());
    }
    std::puts("OK");
}
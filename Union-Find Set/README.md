# Lock-free concurrent union-find

A header-only (`ConcurrentDsu.h`) disjoint-set union where any number of threads can call `find`, `unite` and `same_set` concurrently, without locks.

```cpp
concurrent_dsu<> dsu(n);          // n singleton sets, Index = std::uint32_t
dsu.unite(a, b);                  // true if this call merged two sets
dsu.same_set(a, b);               // linearizable membership query
dsu.find(x);                      // current representative of x's set
```

## The basic idea

Each node stores the index of its parent; a root points to itself. `unite(a, b)` finds both roots and points the *losing root* at the winning root. `find` climbs parent links until it reaches a node that is its own parent.

The concurrent version keeps exactly this structure. It works without locks because the forest **only ever grows**: sets merge but never split, and a node that stops being a root never becomes one again.

## Key invariant: ancestry is permanent

There are only two kinds of writes:

- **Link** (`unite`): `parent[a] = b`, only while `a` is a root.
- **Compression** (`find`): `parent[x] = grandparent(x)`, when `x` is not a root.

Neither removes an ancestor from a node's path to its root. So `parent[x]` always points to *some* ancestor of `x`, and once two nodes share a set they always will.

## `find` and path halving

```cpp
p  = parent[x];  if (p == x) return x;
gp = parent[p];
if (p != gp) CAS(parent[x], p, gp);   // shortcut x to its grandparent
x = gp;                               // and continue two levels up
```

- Path halving makes paths roughly half as long on each walk
- If the CAS fails, another thread already moved `x` higher; the failure is ignored. A CAS is used instead of a plain store so compression never moves a pointer *down* (undoing someone else's work).
- `compare_exchange_weak` is fine because spurious failures are ignored anyway

## `unite` and the linking order

```cpp
a = find(a); b = find(b);
if (a == b) return false;
if (priority(a) > priority(b)) swap(a, b);
if (CAS(parent[a], a, b)) return true;   // succeeds only if a is still a root
// otherwise a was linked meanwhile: retry from the new roots
```

- The CAS is the **only** correctness-critical write. If it fails, someone linked `a` in the meantime, so we find the new roots and retry.
- `b` does not need to still be a root: if it was linked under `c` meanwhile, `a` simply ends up one level deeper in the same merged set.
- **Why an order is needed:** without one, two threads could link `r1 -> r2` and `r2 -> r1` simultaneously and create a cycle. Linking the lower-priority root under the higher one makes priority strictly increase along every edge, so cycles are impossible.

### Why hash the indices

`priority(x)` is an integer mixer (murmur3 `fmix32`, or the splitmix64 finalizer for 64-bit indices).

- Comparing raw indices would turn common structured inputs (`unite(i, i+1)`, `unite(0, i)`, sorted edge lists, BFS-numbered graphs) into long chains. Pseudo-random priorities give expected O(log n) depth for any input order.
- **It is bijective** (a permutation of all 32/64-bit values): xor-shift and multiplication by an odd constant are both invertible mod 2^n. So distinct nodes never tie, and every thread agrees on the winner of every comparison.
- **each set's root is its maximum-priority element**, regardless of thread timing.

## `same_set`

`find(a) == find(b)` alone can give a false negative: `a`'s root may be linked under `b`'s root between the two finds. So when the roots differ, `same_set` re-checks that `a`'s root is still a root; if it is, the sets really were different at that moment, otherwise it retries.

## Design notes

- **Index type:** `std::uint32_t` by default, not `size_t`. `find` is pointer-chasing and memory-bound; 4-byte indices halve the array and double cache density.
- `is_always_lock_free` rejects platforms where a 64-bit atomic would silently use a hidden lock.
- **Contention:** linking writes the *losing* root's slot, so contention is threads racing to CAS the same root; a giant component's root is also read by every `find`.
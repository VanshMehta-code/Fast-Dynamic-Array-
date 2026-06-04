# fastvec_asm.hpp — Usage Guide

Ultra-low-latency vector for Linux x86-64. Zero stdlib. Raw mmap/mremap syscalls.
Drop-in replacement for `std::vector` with a different type name and namespace.

---

## Requirements

| Requirement | Detail |
|---|---|
| OS | Linux x86-64 **only** |
| Compiler | GCC or Clang (requires `__builtin_*` intrinsics) |
| Standard | C++17 or later |
| Header | `#include "fastvec_asm.hpp"` |

**Mandatory build flags — all four, every time:**

```bash
g++ -std=c++17 -O3 -march=native -fno-exceptions -fno-rtti your_file.cpp
```

Missing any of these leaves significant performance on the table. `-O3 -march=native` is what makes `__builtin_memcpy` emit AVX2/SSE4 instructions instead of scalar loops.

---

## The Two Types

Everything lives in the `fv` namespace.

### `fv::vec<T>` — Heap-only (general purpose)

Equivalent to `std::vector<T>`. All storage on the heap via mmap.

```cpp
fv::vec<int>    prices;
fv::vec<double> returns;
fv::vec<Order>  orders;
```

### `fv::svec<T, N>` — Small-buffer vector

Stores up to `N` elements inline (no heap allocation). Falls back to heap when exceeded.
Use when most instances stay small. Default `N = 8`.

```cpp
fv::svec<int>      tags;        // N=8 default — holds up to 8 ints inline
fv::svec<double, 4> quad;       // holds up to 4 doubles inline
fv::svec<Order, 16> hot_orders; // holds up to 16 Orders inline
```

Choose `svec` when the typical case is small and heap pressure matters.
Choose `vec` when size is unbounded or consistently large.

---

## API Reference

The public API matches `std::vector` exactly. If you know `std::vector`, you know this.

### Construction

```cpp
fv::vec<int> a;                         // default — empty
fv::vec<int> b(100);                    // 100 default-constructed elements
fv::vec<int> c(100, 0);                 // 100 zeros
fv::vec<int> d = {1, 2, 3, 4};         // initializer list
fv::vec<int> e(other.begin(), other.end()); // iterator range
fv::vec<int> f(other);                  // copy
fv::vec<int> g(std::move(other));       // move
```

### Modification

```cpp
v.push_back(x);                // copy-push
v.push_back(std::move(x));    // move-push
v.emplace_back(args...);      // construct in-place — returns T& (unlike std::vector pre-C++17)
v.pop_back();
v.insert(it, x);
v.emplace(it, args...);
v.erase(it);
v.erase(first, last);
v.clear();
v.resize(n);
v.resize(n, value);
v.assign(first, last);
```

### Access

```cpp
v[i]          // unchecked — no bounds check overhead
v.at(i)       // bounds-checked — traps (not throws) on violation
v.front()
v.back()
v.data()      // raw pointer
```

### Capacity

```cpp
v.size()
v.capacity()
v.empty()
v.reserve(n)        // pre-allocates; MAP_POPULATE pre-faults all pages
v.shrink_to_fit()   // actually releases memory via mremap (not just a hint)
```

### Iterators

```cpp
v.begin() / v.end()
v.cbegin() / v.cend()
v.rbegin() / v.rend()
```

### HFT-specific

```cpp
v.hint_sequential();   // call before a full forward scan — triggers MADV_SEQUENTIAL
```

---

## Rules for Best Output

Follow these in order of impact.

### 1. Always call `reserve(n)` before a hot loop

This is the single highest-impact rule. `reserve` uses mmap + MAP_POPULATE, which pre-faults every page before you enter your loop. Without it, the first write to each new page triggers a page fault — a kernel round-trip that can cost thousands of nanoseconds.

```cpp
// BAD — page faults on first write to each new page
fv::vec<Order> orders;
for (auto& msg : feed) orders.push_back(msg.to_order());

// GOOD — all pages pre-faulted, zero fault latency in the loop
fv::vec<Order> orders;
orders.reserve(expected_count);
for (auto& msg : feed) orders.push_back(msg.to_order());
```

### 2. Use `emplace_back` instead of `push_back` for non-trivial types

`emplace_back` constructs in-place, eliminating a copy or move. It also returns a `T&` directly, which `std::vector::emplace_back` didn't do until C++17. Use the return value when you need to act on the inserted element immediately.

```cpp
// OK
orders.push_back(Order{id, price, qty});

// Better — constructs directly in the vector's memory
Order& o = orders.emplace_back(id, price, qty);
o.timestamp = now();
```

### 3. Use `svec` for short-lived, small collections

If a vector rarely exceeds a handful of elements (e.g. a list of legs in a spread, or a small set of tags), `svec` avoids a heap allocation entirely. The inline buffer is 64-byte cache-line aligned, so it won't cause false sharing.

```cpp
// Legs of a 4-leg spread — never more than 4; stays entirely in the object
fv::svec<Leg, 4> legs;
legs.emplace_back(inst_a, qty_a);
legs.emplace_back(inst_b, qty_b);
```

### 4. Use `hint_sequential()` before full forward scans

When you're about to iterate the entire vector front-to-back, call this first. It issues `MADV_SEQUENTIAL` to the kernel, which aggressively read-ahead's pages into memory before you need them.

```cpp
book.hint_sequential();
for (const auto& level : book) process(level);
```

Do not call this if you're doing random access — it will hurt, not help.

### 5. Use `vec` (not `svec`) for large or unpredictable sizes

`svec`'s inline buffer is fixed at compile time. If your data frequently exceeds `N`, every instance wastes `N * sizeof(T)` bytes of stack/object space, plus you pay a relocation when it overflows. Use `vec` when size is unbounded or highly variable.

### 6. Prefer `operator[]` over `at()` in hot paths

`at()` calls `FV_ASSERT` which expands to `__builtin_trap()` on failure — no exception overhead, but the bounds check itself still costs a branch. In a tight inner loop where correctness is already guaranteed, use `operator[]`.

```cpp
// In a hot inner loop with known-valid index:
double pnl = positions[i].qty * prices[i];  // no branch overhead
```

### 7. Never use `insert` or `erase` in a hot loop

Both shift elements, which is O(n). If you need frequent mid-container mutation, this is not the right data structure. Use push_back/pop_back (O(1)) or redesign the access pattern.

### 8. Let `shrink_to_fit()` actually shrink

Unlike `std::vector::shrink_to_fit()` (which is non-binding), this implementation uses mremap to genuinely release tail pages back to the OS. If you have a vector that bulged during a burst and is now idle, shrink it.

```cpp
orders.clear();
orders.shrink_to_fit();  // actually returns memory to the OS
```

### 9. Build with all four mandatory flags — always

```bash
-O3 -march=native -fno-exceptions -fno-rtti
```

Without `-march=native`, `__builtin_memcpy` and `__builtin_memmove` emit scalar code. The entire vectorized fast-path disappears. Without `-fno-exceptions`, the compiler generates unwinding tables even for code that never throws, bloating the binary and polluting the i-cache.

---

## What Changes vs `std::vector`

| Behavior | `std::vector` | `fastvec` |
|---|---|---|
| Namespace | `std::` | `fv::` |
| OOM | throws `std::bad_alloc` | `__builtin_trap()` — process dies |
| `at()` violation | throws `std::out_of_range` | `__builtin_trap()` — process dies |
| Allocator | glibc malloc | raw mmap syscall |
| Memory alignment | unspecified | page-aligned (4096 B) |
| Growth factor | typically 2× | 1.5× |
| `shrink_to_fit` | non-binding hint | actually shrinks via mremap |
| `emplace_back` return | `void` (pre-C++17) / `T&` (C++17+) | always `T&` |
| Platform | portable | Linux x86-64 only |
| Exception support | yes | **no** — `-fno-exceptions` required |
| RTTI | yes | **no** — `-fno-rtti` required |

---

## Common Patterns

### Fill then read (most common HFT pattern)

```cpp
fv::vec<Quote> quotes;
quotes.reserve(max_depth);

while (has_update()) {
    quotes.emplace_back(parse_quote());
}

quotes.hint_sequential();
for (const auto& q : quotes) publish(q);
```

### Short-lived accumulator

```cpp
// svec avoids heap entirely for small counts
fv::svec<Fill, 8> fills;
for (auto& leg : order.legs()) fills.emplace_back(execute(leg));
reconcile(fills);
// fills destroyed — no heap to free
```

### Swap two vectors (O(1) for heap buffers)

```cpp
fv::vec<Level> a, b;
// ... populate ...
a.swap(b);  // O(1) pointer swap when neither is in SBO mode
```

---

## Limitations

- Linux x86-64 only. Will not compile on any other platform.
- No exceptions. `at()` and OOM are fatal traps.
- No RTTI. Do not use with code that requires `dynamic_cast` or `typeid`.
- No standard allocator support. Cannot swap allocators or use pmr.
- `swap()` involving `svec` instances in SBO mode is O(n), not O(1).
# Fast-Dynamic-Array-

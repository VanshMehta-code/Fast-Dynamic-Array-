#pragma once

/*
 * fastvec_asm.hpp ─── Ultra-low-latency vector  (Linux x86-64)
 * =============================================================
 *  ZERO stdlib.  Raw mmap / mremap inline syscalls.  MAP_POPULATE.
 *
 *  What changed vs. fastvec.hpp
 *  ─────────────────────────────
 *  REMOVED (8 heavy headers):
 *    <cstdlib> <cstring> <cassert> <stdexcept>
 *    <algorithm> <iterator> <type_traits> <utility>
 *  KEPT (compiler-provided, near-zero cost):
 *    <new>              — placement new
 *    <initializer_list> — {a,b,c} syntax
 *
 *  NEW OPTIMIZATIONS:
 *  1. Raw mmap/munmap/mremap inline asm  — bypasses glibc entirely
 *  2. MAP_POPULATE                       — pre-faults ALL pages at alloc time
 *                                          → zero first-write page-fault latency
 *  3. mremap-FIRST grow strategy:
 *       • In-place extend: kernel updates page tables only → ZERO data copy
 *       • Forced move:     kernel remaps physical pages to new VA → ZERO copy
 *       Falls back to mmap+relocate only when mremap fails (extremely rare on 64-bit)
 *  4. __builtin_mem* intrinsics  — AVX2/SSE4 with -O3 -march=native (no libc calls)
 *  5. __builtin_prefetch         — explicit cache warm-up on large relocations
 *  6. Compiler type-trait builtins (__is_trivially_copyable etc.) — no <type_traits>
 *  7. fv_move / fv_fwd           — pure static_casts, zero overhead, no <utility>
 *  8. FV_HOT on hot path         — denser i-cache; FV_COLD on grow/slow paths
 *  9. Custom ReverseIter<T>      — drops std::reverse_iterator + <iterator>
 * 10. FV_ASSERT → __builtin_trap — no exceptions, no RTTI, no unwinding tables
 * 11. Page-aligned capacity       — mmap granularity gives "free" extra capacity
 * 12. hint_sequential()          — madvise(MADV_SEQUENTIAL) for scan-heavy loops
 *
 *  RETAINED from fastvec.hpp:
 *    SBO, 1.5× growth, FV_LIKELY/UNLIKELY, 64-byte SBO alignment,
 *    emplace_back returns T&, separate slow/fast push_back paths.
 *
 *  BUILD (mandatory for full benefit):
 *    g++/clang++ -std=c++17 -O3 -march=native -fno-exceptions -fno-rtti
 *
 *  HFT TIP: Always call reserve(n) before a hot loop.
 *    reserve() uses mmap + MAP_POPULATE → all n elements' pages are pre-faulted.
 *    With mremap, subsequent grows stay zero-copy as long as VA space is adjacent.
 *
 *  PLATFORM: Linux x86-64 only (GCC or Clang)
 */

#include <new>              // placement new  — compiler-provided, ~zero overhead
#include <initializer_list> // {a,b,c} syntax — compiler magic type

#if !defined(__linux__) || !defined(__x86_64__)
  #error "fastvec_asm.hpp: Linux x86-64 only (uses mmap/munmap/mremap syscalls)"
#endif
#if !defined(__GNUC__) && !defined(__clang__)
  #error "fastvec_asm.hpp: GCC or Clang required (uses __builtin_* and type-trait builtins)"
#endif

// ─── Annotations ─────────────────────────────────────────────────────────────
#define FV_FORCEINLINE   __attribute__((always_inline)) inline
#define FV_NOINLINE      __attribute__((noinline))
#define FV_HOT           __attribute__((hot))
#define FV_COLD          __attribute__((cold))
#define FV_LIKELY(x)     __builtin_expect(!!(x), 1)
#define FV_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#define FV_RESTRICT      __restrict__
// Replaces <cassert>/<stdexcept>: fatal trap, no exception overhead, no unwinding
#define FV_ASSERT(cond)  do { if (FV_UNLIKELY(!(cond))) __builtin_trap(); } while(0)
// L2 prefetch (locality=1 → L2, not L1; reduces pollution of L1 on speculative loads)
#define FV_PREFETCH_R(p) __builtin_prefetch((p), 0, 1)
#define FV_PREFETCH_W(p) __builtin_prefetch((p), 1, 1)

// Compiler built-in types — no <cstddef> required
using fv_size_t    = __SIZE_TYPE__;     // identical to size_t on all GCC/Clang targets
using fv_ptrdiff_t = __PTRDIFF_TYPE__; // identical to ptrdiff_t

namespace fv {

// ─── Meta-utils: replaces <utility> + the <type_traits> pieces we used ───────

template<typename T> struct fv_rm_ref       { using t = T; };
template<typename T> struct fv_rm_ref<T&>   { using t = T; };
template<typename T> struct fv_rm_ref<T&&>  { using t = T; };

template<bool B, typename T = void> struct fv_enable_if         {};
template<typename T>                struct fv_enable_if<true, T> { using type = T; };

// fv_is_integral<T>: replaces std::is_integral (used for SFINAE on iterator ctor).
// Only needs to reject integral types to prevent (n, val) ambiguity with (Iter, Iter).
template<typename T> struct fv_is_integral { static constexpr bool value = false; };
#define FV_INTSPEC_(T) \
    template<> struct fv_is_integral<T>                { static constexpr bool value = true; }; \
    template<> struct fv_is_integral<const T>          { static constexpr bool value = true; }; \
    template<> struct fv_is_integral<volatile T>       { static constexpr bool value = true; }; \
    template<> struct fv_is_integral<const volatile T> { static constexpr bool value = true; };
FV_INTSPEC_(bool)
FV_INTSPEC_(char)
FV_INTSPEC_(signed char)
FV_INTSPEC_(unsigned char)
FV_INTSPEC_(short)
FV_INTSPEC_(unsigned short)
FV_INTSPEC_(int)
FV_INTSPEC_(unsigned int)
FV_INTSPEC_(long)
FV_INTSPEC_(unsigned long)
FV_INTSPEC_(long long)
FV_INTSPEC_(unsigned long long)
FV_INTSPEC_(wchar_t)
FV_INTSPEC_(char16_t)
FV_INTSPEC_(char32_t)
#undef FV_INTSPEC_

// fv_is_ptr<T>: replaces std::is_pointer (used for raw-pointer fast path in assign).
template<typename T> struct fv_is_ptr                { static constexpr bool value = false; };
template<typename T> struct fv_is_ptr<T*>            { static constexpr bool value = true;  };
template<typename T> struct fv_is_ptr<const T*>      { static constexpr bool value = true;  };
template<typename T> struct fv_is_ptr<volatile T*>   { static constexpr bool value = true;  };
template<typename T> struct fv_is_ptr<const volatile T*> { static constexpr bool value = true; };

// std::move  — literally just a cast; no overhead whatsoever
template<typename T>
[[nodiscard]] FV_FORCEINLINE constexpr
typename fv_rm_ref<T>::t&& fv_move(T&& t) noexcept {
    return static_cast<typename fv_rm_ref<T>::t&&>(t);
}

// std::forward — lvalue and rvalue overloads
template<typename T>
[[nodiscard]] FV_FORCEINLINE constexpr
T&& fv_fwd(typename fv_rm_ref<T>::t& t)  noexcept { return static_cast<T&&>(t); }
template<typename T>
[[nodiscard]] FV_FORCEINLINE constexpr
T&& fv_fwd(typename fv_rm_ref<T>::t&& t) noexcept { return static_cast<T&&>(t); }

// std::swap
template<typename T>
FV_FORCEINLINE void fv_swap(T& a, T& b) noexcept {
    T tmp = fv_move(a);
    a     = fv_move(b);
    b     = fv_move(tmp);
}

// ─── Minimal reverse iterator — replaces std::reverse_iterator + <iterator> ──
template<typename T>
struct ReverseIter {
    T* p_;
    FV_FORCEINLINE explicit ReverseIter(T* p) noexcept : p_(p) {}
    FV_FORCEINLINE T&           operator*()      const noexcept { return *(p_ - 1); }
    FV_FORCEINLINE T*           operator->()     const noexcept { return   p_ - 1;  }
    FV_FORCEINLINE ReverseIter& operator++()           noexcept { --p_; return *this; }
    FV_FORCEINLINE ReverseIter  operator++(int)        noexcept { ReverseIter t=*this; --p_; return t; }
    FV_FORCEINLINE ReverseIter& operator--()           noexcept { ++p_; return *this; }
    FV_FORCEINLINE bool operator==(ReverseIter o) const noexcept { return p_ == o.p_; }
    FV_FORCEINLINE bool operator!=(ReverseIter o) const noexcept { return p_ != o.p_; }
};

// ─── Internal implementation details ─────────────────────────────────────────
namespace detail {

// Linux x86-64 syscall numbers and constants
static constexpr long     SYS_MMAP    = 9L;
static constexpr long     SYS_MUNMAP  = 11L;
static constexpr long     SYS_MREMAP  = 25L;
static constexpr long     SYS_MADVISE = 28L;
static constexpr long     PROT_RW     = 0x03L;    // PROT_READ | PROT_WRITE
static constexpr long     MAP_PA      = 0x22L;    // MAP_PRIVATE | MAP_ANONYMOUS
static constexpr long     MAP_POP     = 0x8000L;  // MAP_POPULATE
static constexpr long     MREMAP_MM   = 0x01L;    // MREMAP_MAYMOVE
static constexpr long     MADV_SEQ    = 0x02L;    // MADV_SEQUENTIAL
static constexpr fv_size_t PAGE       = 4096UL;
static constexpr fv_size_t CLINE      = 64UL;     // cache line size

// Round bytes up to the next page boundary
FV_FORCEINLINE constexpr fv_size_t page_round(fv_size_t n) noexcept {
    return (n + PAGE - 1UL) & ~(PAGE - 1UL);
}

// ── mmap ─────────────────────────────────────────────────────────────────────
// MAP_POPULATE: kernel pre-faults every page before returning.
// First write to any element has zero page-fault latency.
// Memory is page-aligned (4096 B) → free 64-byte cache-line alignment included.
[[nodiscard]] FV_NOINLINE FV_COLD
void* fv_mmap(fv_size_t bytes) noexcept {
    const fv_size_t sz    = page_round(bytes);
    register long   r10   asm("r10") = MAP_PA | MAP_POP;
    register long   r8    asm("r8")  = -1L;  // fd (anonymous)
    register long   r9    asm("r9")  =  0L;  // offset
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_MMAP), "D"(0L), "S"(sz), "d"(PROT_RW), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    // Linux syscall error: rax in [-4095, -1]
    return FV_UNLIKELY(ret < 0L) ? nullptr : reinterpret_cast<void*>(ret);
}

// ── munmap ────────────────────────────────────────────────────────────────────
// Always called with page_round(cap_ * sizeof(T)) — symmetric with fv_mmap.
FV_FORCEINLINE
void fv_munmap(void* ptr, fv_size_t bytes) noexcept {
    const fv_size_t sz = page_round(bytes);
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_MUNMAP), "D"(ptr), "S"(sz)
        : "rcx", "r11", "memory"
    );
    (void)ret;
}

// ── mremap ────────────────────────────────────────────────────────────────────
// MREMAP_MAYMOVE: kernel tries to extend in-place first.
//   ● Adjacent VA free  → page-table update only. ZERO bytes copied.
//   ● No adjacent space → moves entire mapping to new VA. ZERO bytes copied.
// On 64-bit Linux (128 TB VA space) mremap almost never fails in practice.
[[nodiscard]] FV_NOINLINE FV_COLD
void* fv_mremap(void* ptr, fv_size_t old_bytes, fv_size_t new_bytes) noexcept {
    const fv_size_t old_sz  = page_round(old_bytes);
    const fv_size_t new_sz  = page_round(new_bytes);
    register long   r10     asm("r10") = MREMAP_MM;
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(SYS_MREMAP), "D"(ptr), "S"(old_sz), "d"(new_sz), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return FV_UNLIKELY(ret < 0L) ? nullptr : reinterpret_cast<void*>(ret);
}

// Round element count so that count * elem_size is page-aligned.
// Since mmap always allocates whole pages anyway, this gives free extra capacity.
FV_FORCEINLINE constexpr
fv_size_t round_cap(fv_size_t count, fv_size_t elem_size) noexcept {
    return page_round(count * elem_size) / elem_size;
}

// ── Relocate ──────────────────────────────────────────────────────────────────
// For trivial types: __builtin_memmove → compiler emits AVX2/SSE4 with -O3 -march=native.
//   Prefetches source cache lines for large moves (> 8 cache lines = 512 bytes).
// For non-trivial: move-construct at dst, destroy src.
//   Direction is chosen automatically to be safe for in-place shifts (insert/erase).
template<typename T>
FV_FORCEINLINE void relocate(T* dst, T* src, fv_size_t n) noexcept {
    if constexpr (__is_trivially_copyable(T)) {
        const fv_size_t bytes = n * sizeof(T);
        // Prefetch source for large moves — amortizes cache-miss latency
        if (FV_UNLIKELY(bytes > CLINE * 8u)) {
            for (fv_size_t off = 0; off < bytes; off += CLINE)
                FV_PREFETCH_R(reinterpret_cast<const char*>(src) + off);
        }
        // memmove: handles overlapping ranges (insert/erase) and is vectorized by compiler
        __builtin_memmove(dst, src, bytes);
    } else {
        // Safe iteration direction for in-place left/right shifts
        if (dst <= src || dst >= src + n) {
            // Forward: safe for left-shift (erase) and non-overlapping (grow)
            for (fv_size_t i = 0; i < n; ++i) {
                ::new (static_cast<void*>(dst + i)) T(fv_move(src[i]));
                src[i].~T();
            }
        } else {
            // Backward: required for right-shift (insert) to avoid clobbering src
            for (fv_size_t i = n; i-- > 0u;) {
                ::new (static_cast<void*>(dst + i)) T(fv_move(src[i]));
                src[i].~T();
            }
        }
    }
}

} // namespace detail


// ╔═════════════════════════════════════════════════════════════════════════════╗
// ║                fastvec<T, SBO_N>                                           ║
// ║   SBO_N > 0: uses inline buffer before any heap allocation                 ║
// ╚═════════════════════════════════════════════════════════════════════════════╝
template<typename T, fv_size_t SBO_N = 0>
class fastvec {
public:
    using value_type             = T;
    using size_type              = fv_size_t;
    using difference_type        = fv_ptrdiff_t;
    using reference              = T&;
    using const_reference        = const T&;
    using pointer                = T*;
    using const_pointer          = const T*;
    using iterator               = T*;
    using const_iterator         = const T*;
    using reverse_iterator       = ReverseIter<T>;
    using const_reverse_iterator = ReverseIter<const T>;

private:
    // ── Compile-time type properties (GCC/Clang compiler builtins) ───────────
    static constexpr bool kSBO   = (SBO_N > 0);
    static constexpr bool kTriv  = __is_trivially_copyable(T);
    static constexpr bool kTrivD = __has_trivial_destructor(T);     // GCC + Clang
    static constexpr bool kTrivC = __is_trivially_constructible(T); // default ctor

    // ── Hot fields: 3 consecutive 64-bit words → reside in one cache line ────
    T*        data_ = nullptr;
    size_type size_ = 0;
    size_type cap_  = 0;

    // ── SBO inline storage ────────────────────────────────────────────────────
    // Cache-line aligned for SBO (avoids false sharing on nearby objects).
    // For non-SBO, uses alignof(T) to avoid the 40-byte padding that alignas(64) would add.
    static constexpr fv_size_t kSBOAlign = kSBO ? detail::CLINE : alignof(T);
    alignas(kSBOAlign) unsigned char sbo_[kSBO ? sizeof(T) * SBO_N : 1u];

    FV_FORCEINLINE       T* sbo_ptr()       noexcept { return reinterpret_cast<T*>(sbo_); }
    FV_FORCEINLINE const T* sbo_ptr() const noexcept { return reinterpret_cast<const T*>(sbo_); }
    FV_FORCEINLINE bool using_sbo()   const noexcept {
        return kSBO && (data_ == const_cast<fastvec*>(this)->sbo_ptr());
    }

    // ── Growth policy: 1.5× (better cache reuse than 2×) ─────────────────────
    FV_FORCEINLINE size_type next_cap(size_type required) const noexcept {
        size_type nc = cap_ + (cap_ >> 1u); // 1.5×
        if (nc < required) nc = required;
        return detail::round_cap(nc, sizeof(T)); // snap up to page boundary for free
    }

    // ── grow_to: COLD path — mremap first, mmap fallback ─────────────────────
    //
    //  mremap path (primary):  O(1) regardless of size. Zero data copy.
    //    The kernel either extends the VA mapping in-place, or moves the physical
    //    page table entries to a new VA. No byte of user data is touched.
    //
    //  mmap fallback (rare):   O(n) — fresh allocation + relocate + munmap old.
    //    MAP_POPULATE ensures the new pages are pre-faulted before return.
    //
    FV_NOINLINE FV_COLD void grow_to(size_type new_cap) {
        if (new_cap == 0) return;
        if constexpr (kSBO) {
            if (new_cap <= SBO_N) return;
        }

        const fv_size_t new_bytes = detail::page_round(new_cap * sizeof(T));

        // ── PRIMARY: try mremap (zero-copy) ───────────────────────────────────
        if (!using_sbo() && data_) {
            const fv_size_t old_bytes = detail::page_round(cap_ * sizeof(T));
            void* remapped = detail::fv_mremap(data_, old_bytes, new_bytes);
            if (FV_LIKELY(remapped != nullptr)) {
                data_ = static_cast<T*>(remapped);
                cap_  = new_bytes / sizeof(T);
                return; // done — no copy, no destroy, no alloc
            }
        }

        // ── FALLBACK: mmap + relocate + munmap old ────────────────────────────
        T* nd = static_cast<T*>(detail::fv_mmap(new_bytes));
        if (FV_UNLIKELY(!nd)) __builtin_trap(); // OOM is fatal — no exception cost

        if (size_ > 0) detail::relocate(nd, data_, size_);

        if (!using_sbo() && data_)
            detail::fv_munmap(data_, detail::page_round(cap_ * sizeof(T)));

        data_ = nd;
        cap_  = new_bytes / sizeof(T); // page rounding may give slightly more capacity
    }

    FV_FORCEINLINE FV_HOT void ensure_cap(size_type required) {
        if (FV_UNLIKELY(required > cap_)) grow_to(next_cap(required));
    }

    FV_FORCEINLINE void destroy_all() noexcept {
        if constexpr (!kTrivD)
            for (size_type i = 0; i < size_; ++i) data_[i].~T();
    }

    FV_FORCEINLINE void free_heap() noexcept {
        if (!using_sbo() && data_)
            detail::fv_munmap(data_, detail::page_round(cap_ * sizeof(T)));
    }

    void move_from(fastvec&& o) noexcept {
        if (o.using_sbo()) {
            if constexpr (kSBO) {
                detail::relocate(sbo_ptr(), o.sbo_ptr(), o.size_);
                data_ = sbo_ptr();
                cap_  = SBO_N;
            }
        } else {
            data_ = o.data_;
            cap_  = o.cap_;
            if constexpr (kSBO) { o.data_ = o.sbo_ptr(); o.cap_ = SBO_N; }
            else                { o.data_ = nullptr;       o.cap_ = 0;    }
        }
        size_   = o.size_;
        o.size_ = 0;
    }

    // ── Cold slow-paths (pushed out of hot i-cache region) ───────────────────
    FV_NOINLINE FV_COLD void pb_slow(const T& v) {
        grow_to(next_cap(size_ + 1));
        ::new (static_cast<void*>(data_ + size_)) T(v);
        ++size_;
    }
    FV_NOINLINE FV_COLD void pb_slow(T&& v) {
        grow_to(next_cap(size_ + 1));
        ::new (static_cast<void*>(data_ + size_)) T(fv_move(v));
        ++size_;
    }
    template<typename... Args>
    FV_NOINLINE FV_COLD T& eb_slow(Args&&... args) {
        grow_to(next_cap(size_ + 1));
        T* p = ::new (static_cast<void*>(data_ + size_)) T(fv_fwd<Args>(args)...);
        ++size_;
        return *p;
    }

public:
    // ─── Constructors ─────────────────────────────────────────────────────────
    FV_FORCEINLINE fastvec() noexcept {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
    }

    explicit fastvec(size_type n) {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
        resize(n);
    }

    fastvec(size_type n, const T& v) {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
        resize(n, v);
    }

    fastvec(std::initializer_list<T> il) {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
        assign(il.begin(), il.end());
    }

    // Iterator-range constructor — SFINAE'd out for integral types
    // (prevents ambiguity with (size_type, value_type) constructor)
    template<typename Iter,
             typename = typename fv_enable_if<!fv_is_integral<Iter>::value>::type>
    fastvec(Iter first, Iter last) {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
        assign(first, last);
    }

    fastvec(const fastvec& o) {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
        assign(o.begin(), o.end());
    }

    fastvec(fastvec&& o) noexcept {
        if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
        move_from(fv_move(o));
    }

    ~fastvec() { destroy_all(); free_heap(); }

    // ─── Assignment ───────────────────────────────────────────────────────────
    fastvec& operator=(const fastvec& o) {
        if (this != &o) { clear(); assign(o.begin(), o.end()); }
        return *this;
    }

    fastvec& operator=(fastvec&& o) noexcept {
        if (this != &o) {
            destroy_all(); free_heap();
            if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; size_ = 0; }
            else                { data_ = nullptr;    cap_ = 0;      size_ = 0; }
            move_from(fv_move(o));
        }
        return *this;
    }

    fastvec& operator=(std::initializer_list<T> il) {
        clear(); assign(il.begin(), il.end()); return *this;
    }

    // ─── Element access ───────────────────────────────────────────────────────
    FV_FORCEINLINE reference       operator[](size_type i) noexcept       { return data_[i]; }
    FV_FORCEINLINE const_reference operator[](size_type i) const noexcept { return data_[i]; }

    // at() uses FV_ASSERT — no exception overhead, traps on violation
    reference       at(size_type i)       { FV_ASSERT(i < size_); return data_[i]; }
    const_reference at(size_type i) const { FV_ASSERT(i < size_); return data_[i]; }

    FV_FORCEINLINE reference       front() noexcept       { return data_[0]; }
    FV_FORCEINLINE const_reference front() const noexcept { return data_[0]; }
    FV_FORCEINLINE reference       back()  noexcept       { return data_[size_ - 1]; }
    FV_FORCEINLINE const_reference back()  const noexcept { return data_[size_ - 1]; }
    FV_FORCEINLINE T*              data()  noexcept       { return data_; }
    FV_FORCEINLINE const T*        data()  const noexcept { return data_; }

    // ─── Iterators ────────────────────────────────────────────────────────────
    FV_FORCEINLINE iterator       begin()  noexcept       { return data_; }
    FV_FORCEINLINE const_iterator begin()  const noexcept { return data_; }
    FV_FORCEINLINE const_iterator cbegin() const noexcept { return data_; }
    FV_FORCEINLINE iterator       end()    noexcept       { return data_ + size_; }
    FV_FORCEINLINE const_iterator end()    const noexcept { return data_ + size_; }
    FV_FORCEINLINE const_iterator cend()   const noexcept { return data_ + size_; }

    reverse_iterator       rbegin()  noexcept       { return reverse_iterator(end());   }
    const_reverse_iterator rbegin()  const noexcept { return const_reverse_iterator(end());   }
    reverse_iterator       rend()    noexcept       { return reverse_iterator(begin()); }
    const_reverse_iterator rend()    const noexcept { return const_reverse_iterator(begin()); }

    // ─── Capacity ─────────────────────────────────────────────────────────────
    FV_FORCEINLINE bool      empty()    const noexcept { return size_ == 0; }
    FV_FORCEINLINE size_type size()     const noexcept { return size_; }
    FV_FORCEINLINE size_type capacity() const noexcept { return cap_; }

    void reserve(size_type n) {
        if (n > 0 && n > cap_) grow_to(detail::round_cap(n, sizeof(T)));
    }

    // mremap allows O(1) shrink too — releases tail pages to the OS
    void shrink_to_fit() {
        if (size_ == 0) {
            free_heap();
            if constexpr (kSBO) { data_ = sbo_ptr(); cap_ = SBO_N; }
            else                { data_ = nullptr;   cap_ = 0;     }
            return;
        }
        const size_type nc = detail::round_cap(size_, sizeof(T));
        if (nc >= cap_) return;
        // Try mremap to shrink (O(1) — releases tail pages, zero copy)
        if (!using_sbo()) {
            void* r = detail::fv_mremap(data_,
                          detail::page_round(cap_ * sizeof(T)),
                          detail::page_round(nc   * sizeof(T)));
            if (r) {
                data_ = static_cast<T*>(r);
                cap_  = detail::page_round(nc * sizeof(T)) / sizeof(T);
                return;
            }
        }
        // Fallback: mmap new smaller region + copy
        T* nd = static_cast<T*>(detail::fv_mmap(nc * sizeof(T)));
        if (!nd) return; // silent fail (best-effort)
        detail::relocate(nd, data_, size_);
        free_heap();
        data_ = nd;
        cap_  = detail::page_round(nc * sizeof(T)) / sizeof(T);
    }

    // ─── HOT PATH ─────────────────────────────────────────────────────────────
    // These 3 functions are the inner loop of almost every HFT container fill.
    // FV_LIKELY: branch predictor trained to expect the fast path.
    // FV_HOT:    tells the compiler to optimize aggressively and place in hot section.
    // pb_slow / eb_slow: FV_COLD, placed in a separate cold code region.

    FV_FORCEINLINE FV_HOT void push_back(const T& v) {
        if (FV_LIKELY(size_ < cap_)) {
            ::new (static_cast<void*>(data_ + size_)) T(v);
            ++size_;
        } else {
            pb_slow(v);
        }
    }

    FV_FORCEINLINE FV_HOT void push_back(T&& v) {
        if (FV_LIKELY(size_ < cap_)) {
            ::new (static_cast<void*>(data_ + size_)) T(fv_move(v));
            ++size_;
        } else {
            pb_slow(fv_move(v));
        }
    }

    template<typename... Args>
    FV_FORCEINLINE FV_HOT T& emplace_back(Args&&... args) {
        if (FV_LIKELY(size_ < cap_)) {
            T* p = ::new (static_cast<void*>(data_ + size_)) T(fv_fwd<Args>(args)...);
            ++size_;
            return *p;
        }
        return eb_slow(fv_fwd<Args>(args)...);
    }

    FV_FORCEINLINE FV_HOT void pop_back() noexcept {
        FV_ASSERT(size_ > 0);
        --size_;
        if constexpr (!kTrivD) data_[size_].~T();
    }

    FV_FORCEINLINE void clear() noexcept { destroy_all(); size_ = 0; }

    // ─── resize ───────────────────────────────────────────────────────────────
    void resize(size_type n) {
        if (n > size_) {
            ensure_cap(n);
            if constexpr (kTrivC)
                __builtin_memset(data_ + size_, 0, (n - size_) * sizeof(T));
            else
                for (size_type i = size_; i < n; ++i)
                    ::new (static_cast<void*>(data_ + i)) T();
        } else {
            if constexpr (!kTrivD)
                for (size_type i = n; i < size_; ++i) data_[i].~T();
        }
        size_ = n;
    }

    void resize(size_type n, const T& v) {
        if (n > size_) {
            ensure_cap(n);
            for (size_type i = size_; i < n; ++i)
                ::new (static_cast<void*>(data_ + i)) T(v);
        } else {
            if constexpr (!kTrivD)
                for (size_type i = n; i < size_; ++i) data_[i].~T();
        }
        size_ = n;
    }

    // ─── insert / emplace ─────────────────────────────────────────────────────
    iterator insert(const_iterator p, const T& v) { return emplace(p, v); }
    iterator insert(const_iterator p, T&&  v)     { return emplace(p, fv_move(v)); }

    template<typename... Args>
    iterator emplace(const_iterator cpos, Args&&... args) {
        // Capture index BEFORE ensure_cap (data_ may change after grow_to)
        const size_type idx = static_cast<size_type>(cpos - data_);
        ensure_cap(size_ + 1);
        T* p = data_ + idx;
        if (idx < size_) detail::relocate(p + 1, p, size_ - idx); // shift right
        ::new (static_cast<void*>(p)) T(fv_fwd<Args>(args)...);
        ++size_;
        return p;
    }

    // ─── erase ────────────────────────────────────────────────────────────────
    iterator erase(const_iterator cp) noexcept {
        const size_type i = static_cast<size_type>(cp - data_);
        FV_ASSERT(i < size_);
        if constexpr (!kTrivD) data_[i].~T();
        detail::relocate(data_ + i, data_ + i + 1, size_ - i - 1); // shift left
        --size_;
        return data_ + i;
    }

    iterator erase(const_iterator cfirst, const_iterator clast) noexcept {
        const size_type first = static_cast<size_type>(cfirst - data_);
        const size_type last  = static_cast<size_type>(clast  - data_);
        const size_type cnt   = last - first;
        if (cnt == 0) return data_ + first;
        if constexpr (!kTrivD)
            for (size_type i = first; i < last; ++i) data_[i].~T();
        detail::relocate(data_ + first, data_ + last, size_ - last);
        size_ -= cnt;
        return data_ + first;
    }

    // ─── assign ───────────────────────────────────────────────────────────────
    template<typename Iter>
    void assign(Iter first, Iter last) {
        clear();
        if constexpr (fv_is_ptr<Iter>::value) {
            // Raw-pointer range: O(1) distance, memcpy fast-path for trivial types
            const size_type n = static_cast<size_type>(last - first);
            ensure_cap(n);
            if constexpr (kTriv) {
                __builtin_memcpy(data_, first, n * sizeof(T));
                size_ = n;
            } else {
                for (; first != last; ++first) {
                    ::new (static_cast<void*>(data_ + size_)) T(*first);
                    ++size_;
                }
            }
        } else {
            // Generic iterator: push_back loop (handles input iterators too)
            for (; first != last; ++first) push_back(*first);
        }
    }

    // ─── swap ─────────────────────────────────────────────────────────────────
    void swap(fastvec& o) noexcept {
        if (using_sbo() || o.using_sbo()) {
            // Can't steal SBO pointer — must move elements
            fastvec tmp(fv_move(o));
            o = fv_move(*this);
            *this = fv_move(tmp);
        } else {
            fv_swap(data_, o.data_);
            fv_swap(size_, o.size_);
            fv_swap(cap_,  o.cap_);
        }
    }

    // ─── Comparison ───────────────────────────────────────────────────────────
    bool operator==(const fastvec& o) const noexcept {
        if (size_ != o.size_) return false;
        if constexpr (kTriv)
            return __builtin_memcmp(data_, o.data_, size_ * sizeof(T)) == 0;
        for (size_type i = 0; i < size_; ++i)
            if (!(data_[i] == o.data_[i])) return false;
        return true;
    }
    bool operator!=(const fastvec& o) const noexcept { return !(*this == o); }
    bool operator< (const fastvec& o) const noexcept {
        // Manual lexicographic compare — replaces std::lexicographical_compare
        const size_type n = size_ < o.size_ ? size_ : o.size_;
        for (size_type i = 0; i < n; ++i) {
            if (data_[i] < o.data_[i]) return true;
            if (o.data_[i] < data_[i]) return false;
        }
        return size_ < o.size_;
    }

    // ─── HFT utilities ────────────────────────────────────────────────────────

    // hint_sequential: advises the kernel that elements will be read front-to-back.
    // The kernel responds by aggressively read-ahead'ing pages.
    // Call before iterating the full vector in a tight loop.
    void hint_sequential() noexcept {
        if (!data_ || !size_) return;
        long ret;
        asm volatile(
            "syscall"
            : "=a"(ret)
            : "a"(detail::SYS_MADVISE),
              "D"(data_),
              "S"(size_ * sizeof(T)),
              "d"(detail::MADV_SEQ)
            : "rcx", "r11", "memory"
        );
        (void)ret;
    }
};

// ─── Non-member helpers ───────────────────────────────────────────────────────
template<typename T, fv_size_t N>
void swap(fastvec<T,N>& a, fastvec<T,N>& b) noexcept { a.swap(b); }

// ─── Convenience aliases ──────────────────────────────────────────────────────
template<typename T>
using vec = fastvec<T, 0>;       // heap-only (no SBO)

template<typename T, fv_size_t N = 8>
using svec = fastvec<T, N>;      // small-buffer vector (avoids heap for ≤ N elements)

} // namespace fv

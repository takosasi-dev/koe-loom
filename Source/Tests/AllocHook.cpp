// Global operator new replacement that counts allocations per thread (AC-39: "確保検知つきで実行").
// Counting is a thread_local increment; behaviour otherwise forwards to malloc/free.

#include "Tests/TestUtil.h"

#include <cstdlib>
#include <new>

namespace
{
thread_local long long tlAllocations = 0;

void* allocOrThrow (std::size_t n)
{
    ++tlAllocations;
    if (n == 0) n = 1;
    if (void* p = std::malloc (n)) return p;
    throw std::bad_alloc();
}

void* allocAlignedOrThrow (std::size_t n, std::align_val_t al)
{
    ++tlAllocations;
    if (n == 0) n = 1;
    if (void* p = _aligned_malloc (n, static_cast<std::size_t> (al))) return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new (std::size_t n) { return allocOrThrow (n); }
void* operator new[] (std::size_t n) { return allocOrThrow (n); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept { ++tlAllocations; return std::malloc (n ? n : 1); }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { ++tlAllocations; return std::malloc (n ? n : 1); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }

void* operator new (std::size_t n, std::align_val_t al) { return allocAlignedOrThrow (n, al); }
void* operator new[] (std::size_t n, std::align_val_t al) { return allocAlignedOrThrow (n, al); }
void operator delete (void* p, std::align_val_t) noexcept { _aligned_free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { _aligned_free (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { _aligned_free (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { _aligned_free (p); }

namespace koe::test
{
AllocationCounter::AllocationCounter() : startCount (tlAllocations) {}
AllocationCounter::~AllocationCounter() = default;
long long AllocationCounter::count() const { return tlAllocations - startCount; }
} // namespace koe::test

#pragma once

#include <cstddef>
#include <cstdlib>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace Licasa::CodecBuffer {
// A 4K frame can raise glibc's adaptive mmap threshold to almost 32 MiB.
// Subsequent native scratch/frame allocations then grow a worker arena whose
// free top chunk malloc_trim() does not always return. Keep large codec-owned
// buffers individually releasable, without changing the process allocator or
// the ordinary JPEG/PNG reader. ASan uses malloc's instrumented redzones.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LICASA_CODEC_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define LICASA_CODEC_ASAN 1
#endif
inline constexpr bool usesMapping(std::size_t bytes) noexcept
{
#if defined(__linux__) && !defined(LICASA_CODEC_ASAN)
    return bytes >= 1024 * 1024;
#else
    (void)bytes;
    return false;
#endif
}

inline void* allocate(std::size_t bytes) noexcept
{
#if defined(__linux__)
    if (usesMapping(bytes)) {
        void* result =
            mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return result == MAP_FAILED ? nullptr : result;
    }
#endif
    return std::malloc(bytes);
}

inline void release(void* memory, std::size_t bytes) noexcept
{
#if defined(__linux__)
    if (memory && usesMapping(bytes)) {
        munmap(memory, bytes);
        return;
    }
#else
    (void)bytes;
#endif
    std::free(memory);
}
} // namespace Licasa::CodecBuffer

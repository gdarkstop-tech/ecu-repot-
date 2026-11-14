// Minimal UTF-8 helpers: copy with truncation that never splits a code point.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace robot {

// Copies src into dst (capacity dstSize, including the terminator).
// Truncates at a code-point boundary. Returns true if truncation happened.
bool utf8CopyTruncate(char* dst, size_t dstSize, const char* src);

// True if every byte sequence in s is well-formed UTF-8 (no overlongs/surrogates checked
// beyond structure — enough to protect the renderer from garbage).
bool utf8IsValid(const char* s);

// True if s contains any byte >= 0x80 (i.e. needs a Unicode font).
bool utf8HasNonAscii(const char* s);

}  // namespace robot

#pragma once

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace llvm {

/// Well-known constants used as ObfCon split noise (hashes, crypto, magic IDs).
ArrayRef<uint64_t> yansoKnownConstants(unsigned BitWidth);

} // namespace llvm

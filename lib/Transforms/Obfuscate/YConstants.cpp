#include "YConstants.h"

#include "llvm/ADT/STLExtras.h"

#include <vector>

using namespace llvm;

namespace {

constexpr uint32_t MD5[] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
};

constexpr uint32_t SHA256[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
};

constexpr uint32_t BLOWFISH[] = {
    0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344, 0xa4093822, 0x299f31d0,
    0x082efa98, 0xec4e6c89, 0x452821e6, 0x38d01377, 0xbe5466cf, 0x34e90c6c,
    0xc0ac29b7, 0xc97c50dd, 0x3f84d5b5, 0xb5470917, 0x9216d5d9, 0x8979fb1b,
};

constexpr uint32_t AES_TE0[] = {
    0xc66363a5, 0xf87c7c84, 0xee777799, 0xf67b7b8d, 0xfff2f20d, 0xd66b6bbd,
    0xde6f6fb1, 0x91c5c554, 0x60303050, 0x02010103, 0xce6767a9, 0x562b2b7d,
    0xe7fefe19, 0xb5d7d762, 0x4dababe6, 0xec76769a, 0x8fcaca45, 0x1f82829d,
    0x89c9c940, 0xfa7d7d87, 0xeffafa15, 0xb25959eb, 0x8e4747c9, 0xfbf0f00b,
    0x41adadec, 0xb3d4d467, 0x5fa2a2fd, 0x45afafea, 0x239c9cbf, 0x53a4a4f7,
    0xe4727296, 0x9bc0c05b,
};

constexpr uint16_t JPEG[] = {
    2446,  3196,  4433,  6270,  7373,  9633,
    12299, 15137, 16069, 16819, 20995, 25172,
};

constexpr uint16_t ICONIC16[] = {
    0x9e37,         // TEA delta
    0xb7e1,         // RC5 P16
    0x1021, 0x8408, // CRC-16/CCITT
    0x8005, 0xa001, // CRC-16/IBM
    0x55aa,         // boot signature
    0xace1, 0xb400, // LFSR
};

constexpr uint64_t SHA512[] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
};

constexpr uint64_t SHA512_IV[] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

constexpr uint64_t XOSHIRO256_JUMPS[] = {
    0x180ec6d33cfd0abaULL, 0xd5a61266f0c9392cULL, 0xa9582618e03fc9aaULL,
    0x39abdc4529b1661cULL, 0x76e15d3efefdcbbfULL, 0xc5004e441c522fb3ULL,
    0x77710069854ee241ULL, 0x39109bb02acbe635ULL,
};

constexpr uint64_t WHIRLPOOL[] = {
    0x18186018c07830d8ULL, 0x23238c2305af4626ULL, 0xc6c63fc67ef991b8ULL,
    0xe8e887e8136fcdfbULL, 0x878726874ca113cbULL, 0xb8b8dab8a9626d11ULL,
    0x0101040108050209ULL, 0x4f4f214f426e9e0dULL,
};

constexpr uint64_t ASCII64[] = {
    0x4170706c65436f6dULL, // "AppleCom"
    0x4546492050415254ULL, // "EFI PART"
    0x5253442050545220ULL, // "RSD PTR "
    0x7668647866696c65ULL, // "vhdxfile"
    0x636f6e6563746978ULL, // "conectix"
    0x4d53444f53352e30ULL, // "MSDOS5.0"
    0x4d5357494e342e31ULL, // "MSWIN4.1"
    0x46726565444f5320ULL, // "FreeDOS "
    0x4e54465320202020ULL, // "NTFS    "
    0x4641543332202020ULL, // "FAT32   "
    0x4558464154202020ULL, // "EXFAT   "
    0x5f42485266535f4dULL, // "_BHRfS_M"
    0x4b564d4b564d4b56ULL, // "KVMKVMKV"
    0x564d77617265564dULL, // "VMwareVM"
    0x47656e75696e6549ULL, // "GenuineI"
    0x213c617263683e0aULL, // "!<arch>\n"
    0x485454502f312e31ULL, // "HTTP/1.1"
    0x5353482d322e302dULL, // "SSH-2.0-"
    0x255044462d312e37ULL, // "%PDF-1.7"
    0x53514c6974652066ULL, // "SQLite f"
    0x414e44524f494421ULL, // "ANDROID!"
    0x564e4452424f4f54ULL, // "VNDRBOOT"
};

static void appendUnique(std::vector<uint64_t> &Result, uint64_t V) {
  if (V > 1 && !llvm::is_contained(Result, V))
    Result.push_back(V);
}

static const std::vector<uint64_t> &known32() {
  static const std::vector<uint64_t> Values = [] {
    std::vector<uint64_t> Result = {
        0x5f3759df, // Quake rsqrt
        0xcbf43926, // CRC-32("123456789")
    };
    for (ArrayRef<uint32_t> Family : {ArrayRef<uint32_t>(MD5),
                                      ArrayRef<uint32_t>(SHA256),
                                      ArrayRef<uint32_t>(BLOWFISH),
                                      ArrayRef<uint32_t>(AES_TE0)})
      for (uint32_t V : Family)
        appendUnique(Result, V);
    for (uint32_t I = 1; I <= 32; ++I) {
      uint32_t CRC = I;
      for (unsigned Bit = 0; Bit != 8; ++Bit)
        CRC = (CRC >> 1) ^ ((CRC & 1) ? 0xedb88320U : 0);
      appendUnique(Result, CRC);
    }
    return Result;
  }();
  return Values;
}

static const std::vector<uint64_t> &known16() {
  static const std::vector<uint64_t> Values = [] {
    std::vector<uint64_t> Result;
    for (uint16_t V : JPEG)
      appendUnique(Result, V);
    for (uint16_t V : ICONIC16)
      appendUnique(Result, V);
    return Result;
  }();
  return Values;
}

static const std::vector<uint64_t> &known64() {
  static const std::vector<uint64_t> Values = [] {
    std::vector<uint64_t> Result = {
        0x5fe6eb50c7b537a9ULL, // double rsqrt
        0x736f6d6570736575ULL, 0x646f72616e646f6dULL, // SipHash IV
        0x6c7967656e657261ULL, 0x7465646279746573ULL, 0x9e3779b185ebca87ULL,
        0xc2b2ae3d27d4eb4fULL, // xxHash64
        0x165667b19e3779f9ULL, 0x85ebca77c2b2ae63ULL, 0x27d4eb2f165667c5ULL,
    };
    for (uint64_t V : SHA512)
      appendUnique(Result, V);
    for (uint64_t V : SHA512_IV)
      appendUnique(Result, V);
    for (uint64_t V : XOSHIRO256_JUMPS)
      appendUnique(Result, V);
    for (uint64_t V : WHIRLPOOL)
      appendUnique(Result, V);
    for (uint64_t V : ASCII64)
      appendUnique(Result, V);
    return Result;
  }();
  return Values;
}

} // namespace

ArrayRef<uint64_t> llvm::yansoKnownConstants(unsigned BitWidth) {
  switch (BitWidth) {
  case 16:
    return known16();
  case 32:
    return known32();
  case 64:
    return known64();
  default:
    return {};
  }
}

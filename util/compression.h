//
// Modernized for C++23.
//
// Compression utilities extracted from the old port/port_stdcxx.h.
// Each compressor is conditionally compiled via HAVE_SNAPPY / HAVE_ZSTD.
// When the library is not available, the functions return std::nullopt /
// false, allowing callers to fall back gracefully.
//
// C++23 changes vs. old port layer:
//   - Standalone header (no port/port.h umbrella)
//   - std::string_view input parameters
//   - std::optional<std::string> return for compress (no output param)
//   - std::optional<size_t> for uncompressed-length query

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Conditionally include compression libraries.
#if HAVE_SNAPPY
#include <snappy.h>
#endif

#if HAVE_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

namespace leveldb {

// ===========================================================================
// Snappy compression
// ===========================================================================

[[nodiscard]] inline std::optional<std::string>
Snappy_Compress(std::string_view input) {
#if HAVE_SNAPPY
  std::string output;
  output.resize(snappy::MaxCompressedLength(input.size()));
  size_t outlen;
  snappy::RawCompress(input.data(), input.size(), output.data(), &outlen);
  output.resize(outlen);
  return output;
#else
  (void)input;
  return std::nullopt;
#endif
}

[[nodiscard]] inline std::optional<size_t>
Snappy_GetUncompressedLength(std::string_view input) {
#if HAVE_SNAPPY
  size_t result;
  if (snappy::GetUncompressedLength(input.data(), input.size(), &result)) {
    return result;
  }
  return std::nullopt;
#else
  (void)input;
  return std::nullopt;
#endif
}

[[nodiscard]] inline bool
Snappy_Uncompress(std::string_view input, char* output) {
#if HAVE_SNAPPY
  return snappy::RawUncompress(input.data(), input.size(), output);
#else
  (void)input;
  (void)output;
  return false;
#endif
}

// ===========================================================================
// Zstd compression
// ===========================================================================

[[nodiscard]] inline std::optional<std::string>
Zstd_Compress(int level, std::string_view input) {
#if HAVE_ZSTD
  size_t outlen = ZSTD_compressBound(input.size());
  if (ZSTD_isError(outlen)) {
    return std::nullopt;
  }
  std::string output;
  output.resize(outlen);
  ZSTD_CCtx* ctx = ZSTD_createCCtx();
  ZSTD_compressionParameters parameters =
      ZSTD_getCParams(level, std::max(input.size(), size_t{1}), /*dictSize=*/0);
  ZSTD_CCtx_setCParams(ctx, parameters);
  outlen = ZSTD_compress2(ctx, output.data(), output.size(),
                          input.data(), input.size());
  ZSTD_freeCCtx(ctx);
  if (ZSTD_isError(outlen)) {
    return std::nullopt;
  }
  output.resize(outlen);
  return output;
#else
  (void)level;
  (void)input;
  return std::nullopt;
#endif
}

[[nodiscard]] inline std::optional<size_t>
Zstd_GetUncompressedLength(std::string_view input) {
#if HAVE_ZSTD
  size_t size = ZSTD_getFrameContentSize(input.data(), input.size());
  if (size == 0 || size == ZSTD_CONTENTSIZE_ERROR ||
      size == ZSTD_CONTENTSIZE_UNKNOWN) {
    return std::nullopt;
  }
  return size;
#else
  (void)input;
  return std::nullopt;
#endif
}

[[nodiscard]] inline bool
Zstd_Uncompress(std::string_view input, char* output) {
#if HAVE_ZSTD
  auto len = Zstd_GetUncompressedLength(input);
  if (!len) return false;
  ZSTD_DCtx* ctx = ZSTD_createDCtx();
  size_t outlen = ZSTD_decompressDCtx(ctx, output, *len,
                                       input.data(), input.size());
  ZSTD_freeDCtx(ctx);
  return !ZSTD_isError(outlen);
#else
  (void)input;
  (void)output;
  return false;
#endif
}

}  // namespace leveldb

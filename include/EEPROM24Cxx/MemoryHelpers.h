// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include "EEPROM24Cxx/Types.h"

namespace EEPROM24Cxx { namespace memory {
/// Pure helpers: no bus, allocation, native struct layout or persistence policy.
/// Byte arrays supplied to codecs must have the indicated number of bytes.
constexpr bool fitsRange(uint32_t capacity, uint32_t address, size_t length) {
  return address <= capacity && length <= static_cast<size_t>(capacity - address);
}
constexpr uint32_t pageStart(const Geometry& geometry, uint32_t address) {
  return geometry.pageSizeBytes ? address - address % geometry.pageSizeBytes : address;
}
constexpr size_t pageRemaining(const Geometry& geometry, uint32_t address) {
  return !geometry.pageSizeBytes || address >= geometry.capacityBytes ? 0 :
      (geometry.pageSizeBytes - address % geometry.pageSizeBytes < geometry.capacityBytes - address ?
       geometry.pageSizeBytes - address % geometry.pageSizeBytes : geometry.capacityBytes - address);
}
constexpr size_t bankRemaining(const Geometry& geometry, uint32_t address) {
  if ((geometry.wordAddressBytes != 1 && geometry.wordAddressBytes != 2) ||
      address >= geometry.capacityBytes) return 0;
  const uint32_t size = uint32_t{1} << (8U * geometry.wordAddressBytes);
  return size - address % size < geometry.capacityBytes - address ?
      size - address % size : geometry.capacityBytes - address;
}
inline void encodeUint16Le(uint16_t value, uint8_t out[2]) {
  out[0] = static_cast<uint8_t>(value); out[1] = static_cast<uint8_t>(value >> 8U);
}
inline void encodeUint32Le(uint32_t value, uint8_t out[4]) {
  for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8U));
}
inline void encodeUint64Le(uint64_t value, uint8_t out[8]) {
  for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8U));
}
inline uint16_t decodeUint16Le(const uint8_t in[2]) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8U));
}
inline uint32_t decodeUint32Le(const uint8_t in[4]) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (i * 8U);
  return value;
}
inline uint64_t decodeUint64Le(const uint8_t in[8]) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(in[i]) << (i * 8U);
  return value;
}
inline void encodeUint16Be(uint16_t value, uint8_t out[2]) {
  out[0] = static_cast<uint8_t>(value >> 8U); out[1] = static_cast<uint8_t>(value);
}
inline void encodeUint32Be(uint32_t value, uint8_t out[4]) {
  for (unsigned i = 0; i < 4; ++i) out[3U - i] = static_cast<uint8_t>(value >> (i * 8U));
}
inline void encodeUint64Be(uint64_t value, uint8_t out[8]) {
  for (unsigned i = 0; i < 8; ++i) out[7U - i] = static_cast<uint8_t>(value >> (i * 8U));
}
inline uint16_t decodeUint16Be(const uint8_t in[2]) {
  return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8U) | in[1]);
}
inline uint32_t decodeUint32Be(const uint8_t in[4]) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value = (value << 8U) | in[i];
  return value;
}
inline uint64_t decodeUint64Be(const uint8_t in[8]) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value = (value << 8U) | in[i];
  return value;
}
inline void encodeInt32Le(int32_t value, uint8_t out[4]) { encodeUint32Le(static_cast<uint32_t>(value), out); }
inline int32_t decodeInt32Le(const uint8_t in[4]) {
  const uint32_t value = decodeUint32Le(in);
  return value <= static_cast<uint32_t>(INT32_MAX) ? static_cast<int32_t>(value) :
      -1 - static_cast<int32_t>(UINT32_MAX - value);
}
inline void encodeInt64Le(int64_t value, uint8_t out[8]) { encodeUint64Le(static_cast<uint64_t>(value), out); }
inline int64_t decodeInt64Le(const uint8_t in[8]) {
  const uint64_t value = decodeUint64Le(in);
  return value <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(value) :
      -1 - static_cast<int64_t>(UINT64_MAX - value);
}
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE-754 binary32 required");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "IEEE-754 binary64 required");
inline void encodeFloatLe(float value, uint8_t out[4]) {
  uint32_t raw = 0; std::memcpy(&raw, &value, sizeof(raw)); encodeUint32Le(raw, out);
}
inline float decodeFloatLe(const uint8_t in[4]) {
  const uint32_t raw = decodeUint32Le(in); float value = 0; std::memcpy(&value, &raw, sizeof(value)); return value;
}
inline void encodeDoubleLe(double value, uint8_t out[8]) {
  uint64_t raw = 0; std::memcpy(&raw, &value, sizeof(raw)); encodeUint64Le(raw, out);
}
inline double decodeDoubleLe(const uint8_t in[8]) {
  const uint64_t raw = decodeUint64Le(in); double value = 0; std::memcpy(&value, &raw, sizeof(value)); return value;
}
/// CRC32/ISO-HDLC: start with 0xFFFFFFFF, update chunks, then XOR 0xFFFFFFFF.
/// data may be null only when length is zero. This is error detection, not security.
inline uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    state ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      state = (state >> 1U) ^ ((state & 1U) ? 0xEDB88320U : 0U);
  }
  return state;
}
inline uint32_t crc32(const uint8_t* data, size_t length) {
  return crc32Update(0xFFFFFFFFU, data, length) ^ 0xFFFFFFFFU;
}
}} // namespace EEPROM24Cxx::memory

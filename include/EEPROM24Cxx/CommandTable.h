// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

namespace EEPROM24Cxx { namespace cmd {
/// 24Cxx uses a control/slave address and memory pointer, not command registers.
constexpr uint8_t DEFAULT_ADDRESS = 0x50;
constexpr uint8_t MIN_ADDRESS = 0x50;
constexpr uint8_t MAX_ADDRESS = 0x57;
constexpr size_t MAX_WRITE_DATA_BYTES = 128;
constexpr size_t MAX_READ_CHUNK = 128;
constexpr uint32_t DEFAULT_WRITE_CYCLE_MS = 5;
constexpr uint32_t TIMER_QUANTIZATION_MARGIN_MS = 1;
}} // namespace EEPROM24Cxx::cmd

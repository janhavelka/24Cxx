// Copyright (c) 2026 Thymos Solution s.r.o. SPDX-License-Identifier: MIT
#pragma once
#include "EEPROM24Cxx/Types.h"
#include "EEPROM24Cxx/CommandTable.h"

namespace EEPROM24Cxx {
/// Callbacks are synchronous, enforce timeout, perform one transaction, do not
/// re-enter the driver, retain buffers, retry, initialize or recover the bus.
/// For nonzero TX and RX, writeRead must use repeated START without a STOP.
/// With supportsCurrentAddressRead, tx=nullptr/txLen=0 requests a pure SLA+R
/// read followed by master NACK and STOP, without an address-write phase.
/// Successful completion requires exact TX/RX counts. Failed RX is discarded.
using I2cWriteFn = TransportResult (*)(uint8_t, const uint8_t*, size_t, uint32_t, void*);
using I2cWriteReadFn = TransportResult (*)(uint8_t, const uint8_t*, size_t, uint8_t*, size_t, uint32_t, void*);
/// Optional address-only SLA+W/ACK/STOP; no data or memory pointer byte.
/// Only this callback is used for write-cycle ACK polling. NACK_ADDRESS is
/// expected write-busy until tWR expires; undifferentiated NACK is a failure.
using I2cProbeFn = TransportResult (*)(uint8_t, uint32_t, void*);
/// Optional monotonic clock in the same domain as poll()/tick(). Required only
/// for logical transfer deadlines; otherwise allows tighter post-STOP waits.
/// Must be bounded, non-reentrant, and wrap naturally as uint32_t milliseconds.
using NowMsFn = uint32_t (*)(void*);
constexpr uint32_t MIN_I2C_TIMEOUT_MS = 1;
constexpr uint32_t DEFAULT_I2C_TIMEOUT_MS = 50;
constexpr uint32_t MAX_I2C_TIMEOUT_MS = 1000;
constexpr size_t MAX_TRANSPORT_TX_BYTES = 130;
constexpr size_t MAX_TRANSPORT_RX_BYTES = 128;
/// Configuration is copied. Callback contexts remain application-owned and
/// valid until end() or successful replacement bind(). Failed bind validation
/// preserves the existing configuration. Callbacks never outlive their caller.
struct Config {
  I2cWriteFn i2cWrite = nullptr;
  I2cWriteReadFn i2cWriteRead = nullptr;
  I2cProbeFn i2cProbe = nullptr;
  void* i2cUser = nullptr;
  NowMsFn nowMs = nullptr;
  void* timeUser = nullptr;
  uint8_t i2cAddress = cmd::DEFAULT_ADDRESS; // Strap/base address: bank bits must be zero.
  uint32_t i2cTimeoutMs = DEFAULT_I2C_TIMEOUT_MS;
  size_t maxTxBytes = 128; // Total transaction size, including pointer prefix.
  size_t maxRxBytes = 128;
  DeviceVariant variant = DeviceVariant::ZETTA_ZD24C02B;
  Geometry customGeometry = {};
  uint32_t writeCycleMs = 0; // Zero uses geometry; override cannot shorten its tWR.
  uint8_t offlineThreshold = 0; // Diagnostic only; zero disables OFFLINE.
  bool supportsCurrentAddressRead = false; // Opt in only if zero-TX reads are supported.
};
} // namespace EEPROM24Cxx

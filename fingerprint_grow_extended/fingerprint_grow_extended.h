// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ============================================================
// fingerprint_grow_extended — standalone add-on component
//
// Does NOT replace the original fingerprint_grow component and does not
// modify its code. It merely shares the same UART bus (physically the
// same R503 sensor) and implements commands that the original ESPHome
// component does not support (READ_INDEX_TABLE, template transfer via
// UpChar/DownChar).
//
// How it works:
//   - Own minimal UART transaction (password verification + command),
//     completely independent of the state of the fingerprint_grow
//     component.
//   - ESPHome is single-threaded/cooperative: as long as our transaction
//     runs synchronously (without yielding to other components), there is
//     no collision with the periodic update() of fingerprint_grow.
//
// Result handling: get_slot_map() and read_template() return their
// results as a return value (struct), NOT via sensor entities. Reason:
// Home Assistant entity states are limited to 255 characters — raw
// template data (Base64) and sometimes the slot occupancy list exceed
// that. Instead, the structs are used directly in an `api.respond`
// lambda in the YAML config (see example.yaml).
// ============================================================

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/uart/uart.h"

#include <vector>
#include <string>

namespace esphome {
namespace fingerprint_grow_extended {

static const uint16_t START_CODE = 0xEF01;
static const uint8_t  PID_COMMAND  = 0x01;
static const uint8_t  PID_DATA     = 0x02;
static const uint8_t  PID_ACK      = 0x07;
static const uint8_t  PID_END_DATA = 0x08;

static const uint8_t  CMD_VERIFY_PASSWORD  = 0x13;
static const uint8_t  CMD_READ_INDEX_TABLE = 0x1F;
static const uint8_t  CMD_LOAD_CHAR        = 0x07;  // Template from slot -> buffer
static const uint8_t  CMD_STORE_CHAR       = 0x06;  // Buffer -> slot
static const uint8_t  CMD_DELETE           = 0x0C;  // Delete slot (for a "real" move)
static const uint8_t  CMD_READ_SYS_PARAM   = 0x0F;  // Read system parameters (incl. packet size)
static const uint8_t  CMD_UP_CHAR          = 0x08;  // Template from buffer -> host (multi-packet)
static const uint8_t  CMD_DOWN_CHAR        = 0x09;  // Template from host -> buffer (multi-packet)

static const uint8_t  RESP_OK            = 0x00;
static const uint8_t  RESP_BAD_LOCATION  = 0x0B;  // invalid slot
static const uint8_t  RESP_FLASH_ERR     = 0x18;
static const uint8_t  RESP_DB_RANGE_FAIL = 0x0C;  // on load: slot is empty

static const uint16_t MAX_SLOTS = 200;
static const uint8_t  INDEX_TABLE_BYTES = 32;

// Safety limit for template size (R503 templates are usually well below
// this; prevents unbounded growth on protocol errors).
static const size_t   MAX_TEMPLATE_BYTES = 4096;

// ── Result structs (instead of sensor publish) ───────────────────
struct SlotMapResult {
  bool success{false};
  std::string slots_json{"[]"};  // JSON array of occupied slots, e.g. "[0,1,5]"
  int32_t next_free_slot{-1};    // -1 = unknown/error, otherwise 0..MAX_SLOTS
};

struct TemplateResult {
  bool success{false};
  std::string checksum_hex;   // e.g. "0x4A3F"
  uint32_t length{0};         // Length in bytes
  std::string data_base64;    // Raw data, Base64-encoded
};

struct MoveResult {
  bool success{false};
  std::string error_message;  // empty on success
};

struct WriteTemplateResult {
  bool success{false};
  std::string error_message;  // empty on success
  uint32_t length{0};         // Number of bytes written (after Base64 decoding)
};

class FingerprintGrowExtendedComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override {}
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_password(uint32_t password) { this->password_ = password; }

  // Reads the slot occupancy and returns the result directly
  // (for use in an api.respond lambda, see YAML example).
  SlotMapResult get_slot_map();

  // Moves a fingerprint from source_slot to dest_slot and returns the
  // result directly (for use in an api.respond lambda).
  // Internally: LoadChar(source_slot) -> buffer 1 -> StoreChar(buffer 1, dest_slot).
  // The source slot remains unchanged by this (a copy, not a "real" move at
  // protocol level) — on success it is then explicitly deleted, so the
  // outcome is equivalent to a move.
  MoveResult move_fingerprint(uint16_t source_slot, uint16_t dest_slot);

  // Reads the template from the given slot completely (LoadChar + UpChar,
  // multi-packet transfer), computes a CRC16 checksum and returns checksum
  // (hex), length (bytes) and the raw data Base64-encoded directly (for use
  // in an api.respond lambda).
  TemplateResult read_template(uint16_t slot);

  // Writes a template previously saved via read_template (Base64-encoded)
  // back into the given slot and returns the result directly (for use in an
  // api.respond lambda).
  // (DownChar + StoreChar, multi-packet transfer). Counterpart to
  // read_template — basis for backup/restore of individual slots.
  WriteTemplateResult write_template(uint16_t slot, const std::string &base64_data);

 protected:
  // Sends a command packet and returns the confirm code.
  // Received payload data (without the confirm byte) ends up in response_data.
  uint8_t send_and_receive_(uint8_t command, const std::vector<uint8_t> &data,
                            std::vector<uint8_t> &response_data);

  // Reads exactly one raw response packet from the UART (any PID).
  // Returns false on timeout/protocol error. pid_out and payload_out are
  // filled on success (payload_out WITHOUT checksum bytes).
  bool read_raw_packet_(uint8_t &pid_out, std::vector<uint8_t> &payload_out);

  // Writes a raw packet with the given PID and payload (incl. checksum).
  void write_raw_packet_(uint8_t pid, const std::vector<uint8_t> &payload);

  bool verify_password_();

  // Reads the system parameters (READ_SYS_PARAM) and derives the currently
  // configured data packet size (32/64/128/256 bytes) from them. The result
  // is cached in data_package_size_. Returns false on error.
  bool read_data_package_length_();

  // Receives a complete multi-packet data stream (after UpChar) and appends
  // the payload data to out until an END_DATA packet (PID 0x08) has been
  // received. Returns false on timeout/protocol error.
  bool receive_data_stream_(std::vector<uint8_t> &out);

  // Sends data in packets of size data_package_size_ (after DownChar),
  // the last packet being marked with PID END_DATA.
  void send_data_stream_(const std::vector<uint8_t> &data);

  static uint16_t crc16_(const std::vector<uint8_t> &data);
  static std::string base64_encode_(const std::vector<uint8_t> &data);
  static std::vector<uint8_t> base64_decode_(const std::string &encoded);

  uint32_t password_{0};
  uint16_t data_package_size_{128};  // Fallback, updated via ReadSysParam
  bool     data_package_size_known_{false};
};

// ════════════════════════════════════════════════════════════════
// Action classes — for "fire and forget" calls outside of
// api.respond (e.g. on_boot, on_enrollment_done). Return values are
// discarded here; for response data, call the C++ methods directly
// from an api.respond lambda (see YAML example).
// ════════════════════════════════════════════════════════════════

template<typename... Ts>
class GetSlotMapAction : public Action<Ts...>, public Parented<FingerprintGrowExtendedComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->get_slot_map(); }
};

template<typename... Ts>
class MoveFingerprintAction : public Action<Ts...>, public Parented<FingerprintGrowExtendedComponent> {
 public:
  TEMPLATABLE_VALUE(uint16_t, source_slot)
  TEMPLATABLE_VALUE(uint16_t, dest_slot)
  void play(const Ts &...x) override {
    auto source_slot = this->source_slot_.value(x...);
    auto dest_slot = this->dest_slot_.value(x...);
    this->parent_->move_fingerprint(source_slot, dest_slot);
  }
};

template<typename... Ts>
class ReadTemplateAction : public Action<Ts...>, public Parented<FingerprintGrowExtendedComponent> {
 public:
  TEMPLATABLE_VALUE(uint16_t, slot)
  void play(const Ts &...x) override {
    auto slot = this->slot_.value(x...);
    this->parent_->read_template(slot);
  }
};

template<typename... Ts>
class WriteTemplateAction : public Action<Ts...>, public Parented<FingerprintGrowExtendedComponent> {
 public:
  TEMPLATABLE_VALUE(uint16_t, slot)
  TEMPLATABLE_VALUE(std::string, template_data)
  void play(const Ts &...x) override {
    auto slot = this->slot_.value(x...);
    auto template_data = this->template_data_.value(x...);
    this->parent_->write_template(slot, template_data);
  }
};

}  // namespace fingerprint_grow_extended
}  // namespace esphome

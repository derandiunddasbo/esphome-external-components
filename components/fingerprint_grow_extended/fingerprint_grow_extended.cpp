// SPDX-License-Identifier: GPL-3.0-only
#include "fingerprint_grow_extended.h"
#include "esphome/core/log.h"
#include <algorithm>

namespace esphome {
namespace fingerprint_grow_extended {

static const char *const TAG = "fingerprint_grow_extended";

// ════════════════════════════════════════════════════════════════
// Generic packet primitives
// ════════════════════════════════════════════════════════════════

void FingerprintGrowExtendedComponent::write_raw_packet_(uint8_t pid,
                                                       const std::vector<uint8_t> &payload) {
  uint16_t wire_length = payload.size() + 2;

  this->write_byte((uint8_t) (START_CODE >> 8));
  this->write_byte((uint8_t) (START_CODE & 0xFF));
  this->write_byte(0xFF);
  this->write_byte(0xFF);
  this->write_byte(0xFF);
  this->write_byte(0xFF);
  this->write_byte(pid);
  this->write_byte((uint8_t) (wire_length >> 8));
  this->write_byte((uint8_t) (wire_length & 0xFF));

  uint16_t checksum = pid + (wire_length >> 8) + (wire_length & 0xFF);
  for (uint8_t b : payload) {
    this->write_byte(b);
    checksum += b;
  }
  this->write_byte((uint8_t) (checksum >> 8));
  this->write_byte((uint8_t) (checksum & 0xFF));
}

bool FingerprintGrowExtendedComponent::read_raw_packet_(uint8_t &pid_out,
                                                      std::vector<uint8_t> &payload_out) {
  payload_out.clear();

  uint8_t byte;
  uint16_t idx = 0;
  uint16_t length = 0;
  std::vector<uint8_t> raw;

  uint32_t start = millis();
  const uint32_t timeout_ms = 3000;

  while (true) {
    if (millis() - start > timeout_ms) {
      ESP_LOGW(TAG, "Timeout while waiting for packet");
      return false;
    }
    if (this->available() == 0) {
      delay(1);
      continue;
    }
    byte = this->read();

    switch (idx) {
      case 0:
        if (byte != (uint8_t) (START_CODE >> 8)) continue;
        break;
      case 1:
        if (byte != (uint8_t) (START_CODE & 0xFF)) { idx = 0; continue; }
        break;
      case 2: case 3: case 4: case 5:
        break;
      case 6:
        pid_out = byte;
        break;
      case 7:
        length = (uint16_t) byte << 8;
        break;
      case 8:
        length |= byte;
        break;
      default:
        raw.push_back(byte);
        if ((idx - 8) == length) {
          if (raw.size() < 2) {
            ESP_LOGW(TAG, "Packet too short");
            return false;
          }
          payload_out.assign(raw.begin(), raw.end() - 2);
          return true;
        }
        break;
    }
    idx++;
  }
}

uint8_t FingerprintGrowExtendedComponent::send_and_receive_(uint8_t command,
                                                          const std::vector<uint8_t> &data,
                                                          std::vector<uint8_t> &response_data) {
  std::vector<uint8_t> payload;
  payload.reserve(1 + data.size());
  payload.push_back(command);
  payload.insert(payload.end(), data.begin(), data.end());
  this->write_raw_packet_(PID_COMMAND, payload);

  uint8_t pid = 0;
  std::vector<uint8_t> resp_payload;
  if (!this->read_raw_packet_(pid, resp_payload)) {
    return 0xFF;
  }
  if (pid != PID_ACK) {
    ESP_LOGW(TAG, "Unexpected PID in response: 0x%02X (command 0x%02X)", pid, command);
    return 0xFE;
  }
  if (resp_payload.empty()) {
    ESP_LOGW(TAG, "Empty response (command 0x%02X)", command);
    return 0xFE;
  }
  uint8_t confirm = resp_payload[0];
  response_data.assign(resp_payload.begin() + 1, resp_payload.end());
  return confirm;
}

bool FingerprintGrowExtendedComponent::receive_data_stream_(std::vector<uint8_t> &out) {
  while (true) {
    uint8_t pid = 0;
    std::vector<uint8_t> chunk;
    if (!this->read_raw_packet_(pid, chunk)) {
      ESP_LOGW(TAG, "Timeout/error while receiving data stream");
      return false;
    }
    if (pid != PID_DATA && pid != PID_END_DATA) {
      ESP_LOGW(TAG, "Unexpected PID in data stream: 0x%02X", pid);
      return false;
    }
    out.insert(out.end(), chunk.begin(), chunk.end());

    if (out.size() > MAX_TEMPLATE_BYTES) {
      ESP_LOGE(TAG, "Data stream exceeds safety limit (%u bytes) - aborted",
               (unsigned) MAX_TEMPLATE_BYTES);
      return false;
    }
    if (pid == PID_END_DATA) {
      return true;
    }
  }
}

void FingerprintGrowExtendedComponent::send_data_stream_(const std::vector<uint8_t> &data) {
  size_t chunk_size = this->data_package_size_ > 0 ? this->data_package_size_ : 128;
  const size_t total = data.size();

  ESP_LOGD(TAG, "Sending data stream: %u bytes in packets of %u bytes",
           (unsigned) total, (unsigned) chunk_size);

  if (total == 0) {
    this->write_raw_packet_(PID_END_DATA, {});
    return;
  }

  // Unambiguous framing: regular chunks are ALWAYS sent as DATA. Only
  // the last, SHORTER remainder chunk is marked as END_DATA. If the total
  // length is an exact multiple of the packet size, there is no shorter
  // remainder — an additional EMPTY END_DATA packet follows instead.
  // Reason: if the last full chunk itself is marked as END_DATA, some
  // sensors do not unambiguously interpret that as the end of the
  // transfer and keep waiting for data — the subsequent StoreChar command
  // is then ignored (timeout, no response).
  size_t offset = 0;
  while (offset + chunk_size <= total) {
    std::vector<uint8_t> chunk(data.begin() + offset, data.begin() + offset + chunk_size);
    this->write_raw_packet_(PID_DATA, chunk);
    offset += chunk_size;
  }

  size_t remaining = total - offset;
  if (remaining > 0) {
    std::vector<uint8_t> chunk(data.begin() + offset, data.end());
    this->write_raw_packet_(PID_END_DATA, chunk);
  } else {
    // Exact multiple of the packet size — explicit empty terminating packet
    this->write_raw_packet_(PID_END_DATA, {});
  }
}

// ════════════════════════════════════════════════════════════════
// Password / system parameters
// ════════════════════════════════════════════════════════════════

bool FingerprintGrowExtendedComponent::verify_password_() {
  std::vector<uint8_t> data = {
      (uint8_t) (this->password_ >> 24), (uint8_t) (this->password_ >> 16),
      (uint8_t) (this->password_ >> 8),  (uint8_t) (this->password_ & 0xFF)};
  std::vector<uint8_t> response;
  uint8_t result = this->send_and_receive_(CMD_VERIFY_PASSWORD, data, response);
  if (result != RESP_OK) {
    ESP_LOGW(TAG, "Password verification failed: 0x%02X", result);
    return false;
  }
  return true;
}

bool FingerprintGrowExtendedComponent::read_data_package_length_() {
  std::vector<uint8_t> response;
  uint8_t result = this->send_and_receive_(CMD_READ_SYS_PARAM, {}, response);
  if (result != RESP_OK) {
    ESP_LOGW(TAG, "ReadSysParam failed: 0x%02X", result);
    return false;
  }
  if (response.size() < 16) {
    ESP_LOGW(TAG, "ReadSysParam: response too short (%u bytes)", (unsigned) response.size());
    return false;
  }
  uint16_t code = ((uint16_t) response[12] << 8) | response[13];
  uint16_t size;
  switch (code) {
    case 0: size = 32; break;
    case 1: size = 64; break;
    case 2: size = 128; break;
    case 3: size = 256; break;
    default:
      ESP_LOGW(TAG, "Unknown data package size code: %u - using fallback %u bytes",
               code, this->data_package_size_);
      return false;
  }
  this->data_package_size_ = size;
  this->data_package_size_known_ = true;
  ESP_LOGI(TAG, "Sensor data packet size detected: %u bytes", size);
  return true;
}

// ════════════════════════════════════════════════════════════════
// CRC16 (CCITT/XModem, poly 0x1021, init 0x0000)
// ════════════════════════════════════════════════════════════════

uint16_t FingerprintGrowExtendedComponent::crc16_(const std::vector<uint8_t> &data) {
  uint16_t crc = 0x0000;
  for (uint8_t b : data) {
    crc ^= (uint16_t) b << 8;
    for (uint8_t i = 0; i < 8; i++) {
      if (crc & 0x8000) {
        crc = (uint16_t) (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

// ════════════════════════════════════════════════════════════════
// Base64 (standard alphabet, with padding)
// ════════════════════════════════════════════════════════════════

static const char *const BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string FingerprintGrowExtendedComponent::base64_encode_(const std::vector<uint8_t> &data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 3 <= data.size()) {
    uint32_t n = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8) | data[i + 2];
    out += BASE64_CHARS[(n >> 18) & 0x3F];
    out += BASE64_CHARS[(n >> 12) & 0x3F];
    out += BASE64_CHARS[(n >> 6) & 0x3F];
    out += BASE64_CHARS[n & 0x3F];
    i += 3;
  }

  size_t remaining = data.size() - i;
  if (remaining == 1) {
    uint32_t n = (uint32_t) data[i] << 16;
    out += BASE64_CHARS[(n >> 18) & 0x3F];
    out += BASE64_CHARS[(n >> 12) & 0x3F];
    out += "==";
  } else if (remaining == 2) {
    uint32_t n = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8);
    out += BASE64_CHARS[(n >> 18) & 0x3F];
    out += BASE64_CHARS[(n >> 12) & 0x3F];
    out += BASE64_CHARS[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

std::vector<uint8_t> FingerprintGrowExtendedComponent::base64_decode_(const std::string &encoded) {
  static int8_t lookup[256];
  static bool lookup_ready = false;
  if (!lookup_ready) {
    for (int i = 0; i < 256; i++) lookup[i] = -1;
    for (int i = 0; i < 64; i++) lookup[(uint8_t) BASE64_CHARS[i]] = (int8_t) i;
    lookup_ready = true;
  }

  std::vector<uint8_t> out;
  out.reserve((encoded.size() / 4) * 3);

  uint32_t buffer = 0;
  int bits = 0;

  for (char c : encoded) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
    int8_t val = lookup[(uint8_t) c];
    if (val < 0) continue;

    buffer = (buffer << 6) | (uint32_t) val;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t) ((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

// ════════════════════════════════════════════════════════════════
// Slot occupancy (READ_INDEX_TABLE)
// ════════════════════════════════════════════════════════════════

SlotMapResult FingerprintGrowExtendedComponent::get_slot_map() {
  ESP_LOGI(TAG, "Reading index table (slot occupancy)...");

  SlotMapResult result;

  if (!this->verify_password_()) {
    return result;  // success=false
  }

  std::vector<uint8_t> response;
  uint8_t status = this->send_and_receive_(CMD_READ_INDEX_TABLE, {0x00}, response);
  if (status != RESP_OK) {
    ESP_LOGW(TAG, "READ_INDEX_TABLE failed: 0x%02X", status);
    return result;
  }
  if (response.size() < INDEX_TABLE_BYTES) {
    ESP_LOGW(TAG, "READ_INDEX_TABLE: response too short (%u bytes)", (unsigned) response.size());
    return result;
  }

  std::string json = "[";
  int32_t first_free = -1;
  bool first = true;

  for (uint16_t slot = 0; slot < MAX_SLOTS; slot++) {
    uint8_t byte_idx = slot / 8;
    uint8_t bit_idx  = slot % 8;
    bool occupied = (response[byte_idx] >> bit_idx) & 0x01;

    if (occupied) {
      if (!first) json += ",";
      json += std::to_string(slot);
      first = false;
    } else if (first_free < 0) {
      first_free = (int32_t) slot;
    }
  }
  json += "]";

  ESP_LOGI(TAG, "Occupied slots: %s", json.c_str());
  ESP_LOGI(TAG, "Next free slot: %d", (int) first_free);

  result.success = true;
  result.slots_json = json;
  result.next_free_slot = first_free >= 0 ? first_free : (int32_t) MAX_SLOTS;
  return result;
}

// ════════════════════════════════════════════════════════════════
// Move (LoadChar + StoreChar + Delete)
// ════════════════════════════════════════════════════════════════

MoveResult FingerprintGrowExtendedComponent::move_fingerprint(uint16_t source_slot, uint16_t dest_slot) {
  ESP_LOGI(TAG, "Moving fingerprint: slot %u -> slot %u", source_slot, dest_slot);

  MoveResult result;

  if (source_slot == dest_slot) {
    ESP_LOGW(TAG, "Source and destination slot are identical - nothing to do");
    result.error_message = "Source and destination slot are identical";
    return result;
  }

  if (!this->verify_password_()) {
    result.error_message = "Password verification failed";
    return result;
  }

  std::vector<uint8_t> load_data = {
      0x01, (uint8_t) (source_slot >> 8), (uint8_t) (source_slot & 0xFF)};
  std::vector<uint8_t> response;
  uint8_t status = this->send_and_receive_(CMD_LOAD_CHAR, load_data, response);

  if (status != RESP_OK) {
    char buf[80];
    switch (status) {
      case RESP_BAD_LOCATION:
      case RESP_DB_RANGE_FAIL:
        ESP_LOGE(TAG, "Source slot %u is empty or invalid (0x%02X)", source_slot, status);
        snprintf(buf, sizeof(buf), "Source slot %u is empty or invalid (0x%02X)",
                 source_slot, status);
        break;
      default:
        ESP_LOGE(TAG, "LoadChar failed: 0x%02X", status);
        snprintf(buf, sizeof(buf), "LoadChar failed (0x%02X)", status);
        break;
    }
    result.error_message = std::string(buf);
    return result;
  }
  ESP_LOGD(TAG, "Template loaded from slot %u", source_slot);

  std::vector<uint8_t> store_data = {
      0x01, (uint8_t) (dest_slot >> 8), (uint8_t) (dest_slot & 0xFF)};
  status = this->send_and_receive_(CMD_STORE_CHAR, store_data, response);

  if (status != RESP_OK) {
    char buf[80];
    switch (status) {
      case RESP_BAD_LOCATION:
        ESP_LOGE(TAG, "Destination slot %u invalid (0x%02X)", dest_slot, status);
        snprintf(buf, sizeof(buf), "Destination slot %u invalid (0x%02X)", dest_slot, status);
        break;
      case RESP_FLASH_ERR:
        ESP_LOGE(TAG, "Flash error while writing to slot %u", dest_slot);
        snprintf(buf, sizeof(buf), "Flash error while writing to slot %u", dest_slot);
        break;
      default:
        ESP_LOGE(TAG, "StoreChar failed: 0x%02X", status);
        snprintf(buf, sizeof(buf), "StoreChar failed (0x%02X)", status);
        break;
    }
    result.error_message = std::string(buf);
    return result;
  }
  ESP_LOGI(TAG, "Template successfully stored in slot %u", dest_slot);

  std::vector<uint8_t> delete_data = {
      (uint8_t) (source_slot >> 8), (uint8_t) (source_slot & 0xFF), 0x00, 0x01};
  status = this->send_and_receive_(CMD_DELETE, delete_data, response);

  if (status != RESP_OK) {
    ESP_LOGW(TAG,
             "Copied, but deleting source slot %u failed (0x%02X) - "
             "fingerprint now exists in BOTH slots %u and %u",
             source_slot, status, source_slot, dest_slot);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "Copied, but deleting slot %u failed (0x%02X) - "
             "fingerprint now exists in slots %u AND %u",
             source_slot, status, source_slot, dest_slot);
    result.error_message = std::string(buf);
    return result;
  }

  ESP_LOGI(TAG, "Move completed: slot %u -> slot %u", source_slot, dest_slot);
  result.success = true;
  return result;
}

// ════════════════════════════════════════════════════════════════
// Read template (LoadChar + UpChar) — checksum, length, Base64 backup
// ════════════════════════════════════════════════════════════════

TemplateResult FingerprintGrowExtendedComponent::read_template(uint16_t slot) {
  ESP_LOGI(TAG, "Reading template from slot %u...", slot);

  TemplateResult result;

  if (!this->verify_password_()) {
    return result;  // success=false
  }

  if (!this->data_package_size_known_) {
    if (!this->read_data_package_length_()) {
      ESP_LOGW(TAG, "Could not determine data packet size - using fallback %u bytes",
               this->data_package_size_);
    }
  }

  std::vector<uint8_t> load_data = {0x01, (uint8_t) (slot >> 8), (uint8_t) (slot & 0xFF)};
  std::vector<uint8_t> response;
  uint8_t status = this->send_and_receive_(CMD_LOAD_CHAR, load_data, response);
  if (status != RESP_OK) {
    switch (status) {
      case RESP_BAD_LOCATION:
      case RESP_DB_RANGE_FAIL:
        ESP_LOGE(TAG, "Slot %u is empty or invalid (0x%02X)", slot, status);
        break;
      default:
        ESP_LOGE(TAG, "LoadChar failed: 0x%02X", status);
        break;
    }
    return result;
  }

  status = this->send_and_receive_(CMD_UP_CHAR, {0x01}, response);
  if (status != RESP_OK) {
    ESP_LOGE(TAG, "UpChar failed: 0x%02X", status);
    return result;
  }

  std::vector<uint8_t> template_bytes;
  if (!this->receive_data_stream_(template_bytes)) {
    ESP_LOGE(TAG, "Error while receiving template data");
    return result;
  }

  uint16_t checksum = crc16_(template_bytes);
  char checksum_hex[8];
  snprintf(checksum_hex, sizeof(checksum_hex), "0x%04X", checksum);

  ESP_LOGI(TAG, "Template read: slot %u, %u bytes, CRC16=%s",
           slot, (unsigned) template_bytes.size(), checksum_hex);

  result.success = true;
  result.checksum_hex = std::string(checksum_hex);
  result.length = (uint32_t) template_bytes.size();
  result.data_base64 = base64_encode_(template_bytes);
  return result;
}

// ════════════════════════════════════════════════════════════════
// Write template (DownChar + StoreChar) — restore from Base64
// ════════════════════════════════════════════════════════════════

WriteTemplateResult FingerprintGrowExtendedComponent::write_template(uint16_t slot,
                                                                   const std::string &base64_data) {
  ESP_LOGI(TAG, "Writing template to slot %u...", slot);

  WriteTemplateResult result;

  std::vector<uint8_t> template_bytes = base64_decode_(base64_data);
  if (template_bytes.empty()) {
    ESP_LOGE(TAG, "Template data empty or Base64 decoding failed - aborted");
    result.error_message = "Template data empty or Base64 decoding failed";
    return result;
  }
  if (template_bytes.size() > MAX_TEMPLATE_BYTES) {
    ESP_LOGE(TAG, "Template too large (%u bytes) - aborted", (unsigned) template_bytes.size());
    char buf[64];
    snprintf(buf, sizeof(buf), "Template too large (%u bytes)", (unsigned) template_bytes.size());
    result.error_message = std::string(buf);
    return result;
  }

  if (!this->verify_password_()) {
    result.error_message = "Password verification failed";
    return result;
  }

  if (!this->data_package_size_known_) {
    if (!this->read_data_package_length_()) {
      ESP_LOGW(TAG, "Could not determine data packet size - using fallback %u bytes",
               this->data_package_size_);
    }
  }

  std::vector<uint8_t> response;
  uint8_t status = this->send_and_receive_(CMD_DOWN_CHAR, {0x01}, response);
  if (status != RESP_OK) {
    ESP_LOGE(TAG, "DownChar failed: 0x%02X", status);
    char buf[48];
    snprintf(buf, sizeof(buf), "DownChar failed (0x%02X)", status);
    result.error_message = std::string(buf);
    return result;
  }

  this->send_data_stream_(template_bytes);

  // Short defensive pause: some sensors need a moment after the last data
  // packet to process the data internally before they correctly accept
  // the next command.
  delay(50);

  std::vector<uint8_t> store_data = {0x01, (uint8_t) (slot >> 8), (uint8_t) (slot & 0xFF)};
  status = this->send_and_receive_(CMD_STORE_CHAR, store_data, response);
  if (status != RESP_OK) {
    char buf[64];
    switch (status) {
      case RESP_BAD_LOCATION:
        ESP_LOGE(TAG, "Destination slot %u invalid (0x%02X)", slot, status);
        snprintf(buf, sizeof(buf), "Destination slot %u invalid (0x%02X)", slot, status);
        break;
      case RESP_FLASH_ERR:
        ESP_LOGE(TAG, "Flash error while writing to slot %u", slot);
        snprintf(buf, sizeof(buf), "Flash error while writing to slot %u", slot);
        break;
      default:
        ESP_LOGE(TAG, "StoreChar failed: 0x%02X", status);
        snprintf(buf, sizeof(buf), "StoreChar failed (0x%02X)", status);
        break;
    }
    result.error_message = std::string(buf);
    return result;
  }

  ESP_LOGI(TAG, "Template successfully written to slot %u (%u bytes)",
           slot, (unsigned) template_bytes.size());

  result.success = true;
  result.length = (uint32_t) template_bytes.size();
  return result;
}

// ════════════════════════════════════════════════════════════════
// Configuration dump
// ════════════════════════════════════════════════════════════════

void FingerprintGrowExtendedComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Fingerprint Grow Extended (add-on component):");
  ESP_LOGCONFIG(TAG, "  Results are delivered as action response (no sensor entities)");
  if (this->data_package_size_known_) {
    ESP_LOGCONFIG(TAG, "  Data packet size already detected: %u bytes", this->data_package_size_);
  }
}

}  // namespace fingerprint_grow_extended
}  // namespace esphome

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/hal.h"

#include <string>
#include <cstdio>
#include <vector>

namespace esphome {
namespace intex_spa {

// ── Model IDs ────────────────────────────────────────────────────────────────
static const uint16_t MODEL_28458 = 28458;
static const uint16_t MODEL_28442 = 28442;

// ── Protocol constants (from original Spa.ino) ───────────────────────────────
static const uint8_t SIZE_PUMP_DATA       = 17;
static const uint8_t SIZE_CONTROLLER_DATA = 8;

// Commands sent TO pump
static const uint16_t CMD_ON_OFF             = 0x0001;
static const uint16_t CMD_CELSIUS_FAHRENHEIT = 0x0002;
static const uint16_t CMD_INCREASE           = 0x0004;
static const uint16_t CMD_DECREASE           = 0x0008;
static const uint16_t CMD_WATER_FILTER       = 0x0010;
static const uint16_t CMD_BUBBLE             = 0x0020;
static const uint16_t CMD_HEATER             = 0x0040;
static const uint16_t CMD_SANITIZER          = 0x0080;  // model 28458 only
static const uint16_t CMD_WATER_JET          = 0x0100;  // model 28458 only

// Byte indices inside the received pump frame
static const uint8_t BYTE_STATUS_FLAGS        = 2;   // Fahrenheit, command-received
static const uint8_t BYTE_STATUS_COMMAND      = 4;   // on/off bits for all functions
static const uint8_t BYTE_ACTUAL_TEMP         = 5;
static const uint8_t BYTE_SETPOINT_TEMP       = 7;
static const uint8_t BYTE_SANITIZER_TIME      = 8;   // remaining hours (001H-008H)
static const uint8_t BYTE_FILTER_TIME         = 12;  // remaining hours
static const uint8_t BYTE_ERROR               = 14;
static const uint8_t BYTE_CONTROLLER_LOADING  = 3;

// Bitmasks inside BYTE_STATUS_COMMAND
static const uint8_t BIT_POWER          = 0x01;
static const uint8_t BIT_HEATER_STANDBY = 0x02;
static const uint8_t BIT_HEATER_ON      = 0x04;
static const uint8_t BIT_WATER_FILTER   = 0x08;
static const uint8_t BIT_BUBBLE         = 0x10;
static const uint8_t BIT_SANITIZER      = 0x20;
static const uint8_t BIT_WATER_JET      = 0x80;

// Bitmasks inside BYTE_STATUS_FLAGS
static const uint8_t BIT_FAHRENHEIT      = 0x01;
static const uint8_t BIT_CMD_RECEIVED    = 0x80;

// Error codes reported in BYTE_ERROR
static const uint8_t ERR_NONE           = 0;
static const uint8_t ERR_NO_FLOW        = 90;
static const uint8_t ERR_LOW_SALT       = 91;
static const uint8_t ERR_HIGH_SALT      = 92;
static const uint8_t ERR_TEMP_TOO_LOW   = 94;
static const uint8_t ERR_TEMP_TOO_HIGH  = 95;
static const uint8_t ERR_SYSTEM         = 96;
static const uint8_t ERR_DRY_FIRE       = 97;
static const uint8_t ERR_TEMP_SENSOR    = 99;

// LC12S hardware configuration
static const uint8_t LC12S_DEVICE_ID_HIGH = 0xB9;
static const uint8_t LC12S_DEVICE_ID_LOW  = 0x46;

// ── Forward declaration ───────────────────────────────────────────────────────
class IntexSpa;

// ─────────────────────────────────────────────────────────────────────────────
// SpaSwitch – a simple on/off switch that queues a toggle command
// ─────────────────────────────────────────────────────────────────────────────
class SpaSwitch : public switch_::Switch, public Parented<IntexSpa> {
 public:
  void set_command(uint16_t cmd) { command_ = cmd; }
  void set_status_bit(uint8_t bit) { status_bit_ = bit; }  // bit in BYTE_STATUS_COMMAND to verify, 0=none
 protected:
  void write_state(bool state) override;
  uint16_t command_{0};
  uint8_t  status_bit_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// SpaTimerSelect – dropdown: "Off" / "2h" / "3h" / "4h" / "5h" / "6h" / "8h"
// (valid values depend on function; constructor sets the valid set)
// ─────────────────────────────────────────────────────────────────────────────
class SpaTimerSelect : public select::Select, public Parented<IntexSpa> {
 public:
  enum class Function { FILTER, SANITIZER };
  void set_function(Function f) { function_ = f; }
 protected:
  void control(const std::string &value) override;
  Function function_{Function::FILTER};
};

// ─────────────────────────────────────────────────────────────────────────────
// SpaClimate – represents the heater as a HA climate entity
//   AUTO mode = heater requested (pump decides when to heat)
//   OFF  mode = heater off
// ─────────────────────────────────────────────────────────────────────────────
class SpaClimate : public climate::Climate, public Component, public Parented<IntexSpa> {
 public:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
};

// ─────────────────────────────────────────────────────────────────────────────
// IntexSpa – main hub component
// ─────────────────────────────────────────────────────────────────────────────
class IntexSpa : public Component, public uart::UARTDevice {
 public:
  // ── ESPHome lifecycle ──────────────────────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── Configuration setters ─────────────────────────────────────────────────
  void set_network_id(uint16_t id) { network_id_ = id; }
  void set_channel(uint8_t ch)     { channel_ = ch; }
  void set_model(uint16_t m)       { model_ = m; }
  void set_cs_pin(uint8_t p)       { cs_pin_ = p; }
  void set_set_pin(uint8_t p)      { set_pin_ = p; }
  void set_active_scan(bool b)     { active_scan_ = b; }
  bool is_power_on() const         { return power_on_; }

  // ── Sensor registration ───────────────────────────────────────────────────
  void set_actual_temperature_sensor(sensor::Sensor *s)  { actual_temp_sensor_ = s; }
  void set_error_code_sensor(sensor::Sensor *s)          { error_code_sensor_ = s; }
  void set_filter_remaining_sensor(sensor::Sensor *s)    { filter_remaining_sensor_ = s; }
  void set_sanitizer_remaining_sensor(sensor::Sensor *s) { sanitizer_remaining_sensor_ = s; }
  // Optional: a real-time clock (homeassistant/sntp/etc). Without one, the
  // sub-hour estimate still works within a single boot session but resets
  // to a fresh anchor on every restart instead of surviving it.
  void set_time_id(time::RealTimeClock *t) { time_id_ = t; }
  void set_filter_remaining_hm_text_sensor(text_sensor::TextSensor *s)    { filter_remaining_hm_ts_ = s; }
  void set_sanitizer_remaining_hm_text_sensor(text_sensor::TextSensor *s) { sanitizer_remaining_hm_ts_ = s; }

  // ── Binary sensor registration ────────────────────────────────────────────
  void set_heater_active_binary_sensor(binary_sensor::BinarySensor *s)     { heater_active_bs_ = s; }
  void set_water_filter_binary_sensor(binary_sensor::BinarySensor *s)   { water_filter_bs_ = s; }
  void set_bubble_binary_sensor(binary_sensor::BinarySensor *s)         { bubble_bs_ = s; }
  void set_water_jet_binary_sensor(binary_sensor::BinarySensor *s)      { water_jet_bs_ = s; }
  void set_sanitizer_binary_sensor(binary_sensor::BinarySensor *s)      { sanitizer_bs_ = s; }
  void set_comm_error_binary_sensor(binary_sensor::BinarySensor *s)     { comm_error_bs_ = s; }
  void set_spa_active_binary_sensor(binary_sensor::BinarySensor *s)     { spa_active_bs_ = s; }
  void set_scanning_binary_sensor(binary_sensor::BinarySensor *s)       { scanning_bs_ = s; }
  void set_is_sending_binary_sensor(binary_sensor::BinarySensor *s)     { is_sending_bs_ = s; }

  // Queue a setpoint change (latest call always overwrites previous)
  void queue_temperature(uint8_t target)  { want_temp_ = target; want_temp_change_ = true; }
  void queue_filter(uint8_t hours, bool off) { want_filter_ = hours; want_filter_off_ = off; want_filter_change_ = true; }
  void queue_sanitizer(uint8_t hours, bool off) { want_sanitizer_ = hours; want_sanitizer_off_ = off; want_sanitizer_change_ = true; }
  // bit_mask: bit in BYTE_STATUS_COMMAND to verify (0 = no verification, e.g. CMD_HEATER)
  void queue_toggle(uint16_t cmd, uint8_t bit_mask = 0, bool desired_state = false) {
    pending_toggle_cmd_           = cmd;
    pending_toggle_start_         = millis();
    pending_toggle_bit_mask_      = bit_mask;
    pending_toggle_desired_state_ = desired_state;
    is_busy_                      = true;
    if (is_sending_bs_) is_sending_bs_->publish_state(true);
  }

  // ── Switch registration ────────────────────────────────────────────────────
  void set_power_switch(SpaSwitch *s)        { power_sw_ = s; }
  void set_bubble_switch(SpaSwitch *s)       { bubble_sw_ = s; }
  void set_water_jet_switch(SpaSwitch *s)    { water_jet_sw_ = s; }
  void set_fahrenheit_switch(SpaSwitch *s)   { fahrenheit_sw_ = s; }

  // ── Select registration ────────────────────────────────────────────────────
  void set_filter_select(SpaTimerSelect *s)    { filter_select_ = s; }
  void set_sanitizer_select(SpaTimerSelect *s) { sanitizer_select_ = s; }

  // ── Climate registration ───────────────────────────────────────────────────
  void set_climate(SpaClimate *c) { climate_ = c; }

  // ── Public API (called by sub-entities) ────────────────────────────────────
  void queue_command(uint16_t cmd)           { command_to_send_ |= cmd; last_time_send_data_ = millis(); }
  void set_target_temperature(float temp);
  void set_filter_time(uint8_t hours);    // 0=off, 2, 4, 6
  void set_sanitizer_time(uint8_t hours); // 0=off, 3, 5, 8
  bool is_online() const { return !comm_error_; }
  bool is_fahrenheit() const { return fahrenheit_; }

  // Called by SpaTimerSelect – must be public
  std::string hours_to_option_(uint8_t hours);
  uint8_t     option_to_hours_(const std::string &opt);

 protected:
  // ── Configuration ──────────────────────────────────────────────────────────
  uint16_t network_id_{0xFFFF};
  uint8_t  channel_{0x48};
  uint16_t model_{MODEL_28458};
  uint8_t  cs_pin_{18};
  uint8_t  set_pin_{19};
  bool     active_scan_{true};

  // ── Protocol state ─────────────────────────────────────────────────────────
  uint8_t  first_command_char_{0};
  uint8_t  controller_loading_state_{0};
  bool     first_send_{true};
  bool     comm_error_{false};

  // RX state machine
  uint16_t rx_state_{0};
  uint8_t  data_[SIZE_PUMP_DATA + 2]{};
  uint8_t  data_controller_[SIZE_CONTROLLER_DATA + 2]{};
  uint16_t data_counter_{0};
  bool     finish_pump_message_{false};
  bool     finish_controller_message_{false};
  bool     command_received_{false};

  // TX
  uint16_t command_to_send_{0};
  uint32_t last_time_send_data_{0};

  // Temperature setpoint tracking
  uint8_t  actual_setpoint_temp_{0};
  uint8_t  target_setpoint_temp_{0};
  bool     change_target_temp_{false};
  bool     fahrenheit_{false};

  // Filter time tracking
  uint8_t  actual_filter_time_{0};
  uint8_t  target_filter_time_{0};
  bool     change_filter_time_{false};
  bool     switch_off_filter_{false};
  bool     state_filter_{false};

  // Sanitizer time tracking (model 28458 only)
  uint8_t  actual_sanitizer_time_{0};
  uint8_t  target_sanitizer_time_{0};
  bool     change_sanitizer_time_{false};
  bool     switch_off_sanitizer_{false};
  bool     state_sanitizer_{false};

  // LC12S non-blocking config state machine (runs in first loop() calls)
  bool     lc12s_configured_{false};
  uint8_t  lc12s_config_step_{0};
  uint32_t lc12s_config_time_{0};

  // LC12S channel switch state machine
  enum class LC12SState { IDLE, SET_LOW_WAIT, SEND_CONFIG, POST_WAIT, SET_HIGH, FLUSH };
  LC12SState lc12s_sw_state_{LC12SState::IDLE};
  uint8_t    lc12s_sw_channel_{0};
  uint32_t   lc12s_sw_time_{0};

  // ── Scan & connection timing constants ────────────────────────────────────
  static const uint32_t COMM_TIMEOUT_MS        = 10000; // → comm lost
  static const uint32_t STARTUP_CONFIG_MS      = 120000; // 2 min startup window // startup: try configured channel
  static const uint32_t STARTUP_NVS_MS         = 30000;  // 30s NVS channel window // startup: try NVS channel
  static const uint32_t RUNTIME_SCAN_DELAY_MS  = 90000; // runtime: delay before scan
  static const uint32_t SCAN_ACTIVE_DWELL_MS   = 1000;  // active scan: 1s listen per channel
  static const uint32_t SCAN_PASSIVE_DWELL_MS  = 5000;  // passive scan: 5s listen per channel
  static const uint32_t SCAN_CONFIRM_MS        = 10000; // confirm candidate for 10s
  static const uint32_t SCAN_RECHECK_MS        = 10000; // recheck last good channel every 10s

  // ── Scan state machine ─────────────────────────────────────────────────────
  enum class ScanState {
    IDLE,
    STARTUP_CONFIG,     // trying configured channel on startup (30s)
    STARTUP_NVS,        // trying NVS channel on startup (15s)
    SWEEP,              // iterating channels (post-config flush via pending mechanism)
    CONFIRM,            // confirming a candidate (10s)
    RECHECK,            // rechecking last good channel mid-scan (3s)
  };


  ScanState scan_state_{ScanState::STARTUP_CONFIG};
  uint8_t   scan_channel_{0};          // current sweep channel
  uint8_t   scan_candidate_{0xFF};     // channel with byte activity
  uint8_t   nvs_channel_{0xFF};        // channel loaded from NVS (0xFF = none)
  uint8_t   last_good_channel_{0xFF};  // last channel that had a valid frame
  uint32_t  scan_phase_start_{0};      // when current phase started
  uint32_t  scan_last_recheck_{0};     // last time we rechecked last_good_channel
  uint32_t  scan_bytes_seen_{0};       // bytes received in current dwell window
  bool      startup_done_{false};      // true once first valid frame received

  binary_sensor::BinarySensor *scanning_bs_{nullptr};

  uint32_t last_time_receive_data_{0};
  uint32_t last_life_signal_{0};
  uint8_t  tx_echo_remaining_{0};
  uint8_t  last_tx_frame_[8]{};

  // Post-TX blackout: suppress state publishing for 1000ms after sending a command
  // to prevent bounce-back where the pump echoes the old state before processing.
  uint32_t last_tx_time_{0};
  static const uint32_t TX_BLACKOUT_MS = 1000;

  // Hierarchical command queue: only one "step" command active at a time.
  // Priority: temperature setpoint > filter timer > sanitizer timer > toggle cmds.
  // is_busy is true while any step-adjustment is in progress.
  bool     is_busy_{false};
  binary_sensor::BinarySensor *is_sending_bs_{nullptr};

  // Pending toggle commands (queued while busy)
  uint16_t pending_toggle_cmd_{0};
  uint32_t pending_toggle_start_{0};
  uint8_t  pending_toggle_bit_mask_{0};
  bool     pending_toggle_desired_state_{false};

  // Setpoint targets (latest wins, older overwritten)
  bool     want_temp_change_{false};
  uint8_t  want_temp_{0};
  bool     want_filter_change_{false};
  uint8_t  want_filter_{0};
  bool     want_filter_off_{false};
  bool     want_sanitizer_change_{false};
  uint8_t  want_sanitizer_{0};
  bool     want_sanitizer_off_{false};

  // Last error code (for change detection)
  uint8_t  last_error_code_{0};

  // Power state tracking (for controls availability)
  bool     power_on_{false};
  bool     last_power_on_{false};  // previous frame value, for change detection

  // ── Sub-component pointers ─────────────────────────────────────────────────
  sensor::Sensor *actual_temp_sensor_{nullptr};
  sensor::Sensor *error_code_sensor_{nullptr};
  sensor::Sensor *filter_remaining_sensor_{nullptr};
  sensor::Sensor *sanitizer_remaining_sensor_{nullptr};
  text_sensor::TextSensor *filter_remaining_hm_ts_{nullptr};
  text_sensor::TextSensor *sanitizer_remaining_hm_ts_{nullptr};

  // Sub-hour estimate: the pump only reports whole hours remaining. We track
  // the last observed hour-value CHANGE and linearly extrapolate a smoother
  // estimate between updates. Purely observational – whenever the raw value
  // changes for ANY reason (our own select, the physical panel/tablet, or a
  // fresh pump report), we simply re-anchor here, so this is automatically
  // robust against external changes with no special handling needed.
  // 0xFF is used as "never observed yet" sentinel since the documented valid
  // range is 0-8.
  //
  // Anchor time is stored BOTH as millis() (always available, resets every
  // boot) and as a real Unix epoch second (only available/meaningful if
  // time_id_ is configured and synced). The epoch anchor is persisted to NVS
  // so the estimate survives a reboot; millis() cannot be persisted
  // meaningfully since it always restarts at 0.
  struct HourMark {
    uint8_t  hour_value;
    uint32_t epoch_time;  // 0 = unknown/no RTC available when this was saved
  };
  time::RealTimeClock *time_id_{nullptr};
  ESPPreferenceObject  filter_mark_pref_;
  ESPPreferenceObject  sanitizer_mark_pref_;
  uint8_t  filter_hour_mark_value_{0xFF};
  uint32_t filter_hour_mark_millis_{0};
  uint32_t filter_hour_mark_epoch_{0};
  uint8_t  sanitizer_hour_mark_value_{0xFF};
  uint32_t sanitizer_hour_mark_millis_{0};
  uint32_t sanitizer_hour_mark_epoch_{0};

  // Re-anchors on value change (persisting to NVS if a valid RTC is
  // available) and returns the current extrapolated estimate in hours.
  // `active` reflects the pump's own on/off status bit for this function
  // (e.g. state_filter_). Treating "off" as an effective raw value of 0
  // guarantees that ANY off->on transition is seen as a value change and
  // triggers a fresh re-anchor – even if the new cycle happens to start at
  // the exact same hour count as an older, stale NVS-persisted anchor
  // (e.g. "4h" selected again later), which would otherwise be mistaken
  // for a continuation of the old, long-expired countdown.
  float update_hour_estimate_(bool active, uint8_t current_raw, uint8_t &mark_value,
                               uint32_t &mark_millis, uint32_t &mark_epoch,
                               ESPPreferenceObject &pref) {
    uint8_t effective_raw = active ? current_raw : 0;
    bool     time_valid = (time_id_ != nullptr) && time_id_->now().is_valid();
    uint32_t now_epoch   = time_valid ? static_cast<uint32_t>(time_id_->now().timestamp) : 0;
    uint32_t now_ms       = millis();

    if (effective_raw != mark_value) {
      mark_value  = effective_raw;
      mark_millis = now_ms;
      mark_epoch  = now_epoch;  // 0 if no RTC yet – falls back to millis() below
      if (time_valid) {
        HourMark hm{mark_value, mark_epoch};
        pref.save(&hm);
      }
    }

    if (mark_value == 0) return 0.0f;

    float elapsed_h;
    if (time_valid && mark_epoch != 0) {
      // Real elapsed time, correctly spans across a reboot.
      elapsed_h = static_cast<float>(now_epoch - mark_epoch) / 3600.0f;
    } else {
      // No RTC (yet) – best-effort, resets to 0 elapsed on every boot.
      elapsed_h = static_cast<float>(now_ms - mark_millis) / 3600000.0f;
    }
    float est = static_cast<float>(mark_value) - elapsed_h;
    if (est < 0) est = 0;
    return est;
  }

  // Formats a fractional-hours estimate as "H:MM", e.g. 3.667f -> "3:40".
  static std::string format_hours_hm_(float hours) {
    if (hours < 0) hours = 0;
    int total_minutes = static_cast<int>(hours * 60.0f + 0.5f);  // round to nearest minute
    int h = total_minutes / 60;
    int m = total_minutes % 60;
    char buf[16];  // large enough for any int range, silences -Wformat-truncation
    snprintf(buf, sizeof(buf), "%d:%02d", h, m);
    return std::string(buf);
  }

  binary_sensor::BinarySensor *heater_active_bs_{nullptr};
  binary_sensor::BinarySensor *water_filter_bs_{nullptr};
  binary_sensor::BinarySensor *bubble_bs_{nullptr};
  binary_sensor::BinarySensor *water_jet_bs_{nullptr};
  binary_sensor::BinarySensor *sanitizer_bs_{nullptr};
  binary_sensor::BinarySensor *comm_error_bs_{nullptr};
  binary_sensor::BinarySensor *spa_active_bs_{nullptr};  // true = online AND power on

  SpaSwitch *power_sw_{nullptr};
  SpaSwitch *bubble_sw_{nullptr};
  SpaSwitch *water_jet_sw_{nullptr};
  SpaSwitch *fahrenheit_sw_{nullptr};

  SpaTimerSelect *filter_select_{nullptr};
  SpaTimerSelect *sanitizer_select_{nullptr};

  SpaClimate *climate_{nullptr};

  // ── Internal helpers ───────────────────────────────────────────────────────
  void     read_data_(uint8_t c);
  void     data_management_();
  void     set_availability_(bool available);   // comm timeout → freeze states
  void     publish_power_off_();                // power off → publish all-off
  void     send_life_signal_();
  void     set_lc12s_channel_(uint8_t channel, uint16_t network_id);
  void     switch_to_channel_(uint8_t ch);       // configure LC12S + update FCC
  void     scan_tick_();
  void     start_scan_sweep_();                  // begin full channel sweep
  void     send_raw_command_(uint16_t command);
  void     send_command_management_();
  void     send_temperature_setpoint_();
  void     send_special_command_(uint16_t command, bool *change_flag, bool switch_off,
                                  uint8_t target, uint8_t actual);
  void     handle_error_notification_(uint8_t error_code);
  void     process_command_queue_();   // hierarchical command dispatcher
  uint16_t calc_crc_(const uint8_t *data, uint8_t len);
  uint16_t crc_xmodem_update_(uint16_t crc, uint8_t data);
};

}  // namespace intex_spa
}  // namespace esphome

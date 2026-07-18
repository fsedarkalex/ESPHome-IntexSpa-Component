#include "intex_spa.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

// ESP-IDF GPIO (no Arduino pinMode/digitalWrite)
#include "driver/gpio.h"
#include "driver/uart.h"

#include <cstring>
#include <cstdio>

namespace esphome {
namespace intex_spa {

static const char *const TAG = "intex_spa";

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: map timer hours <-> select option strings
//
// The pump encodes filter/sanitizer remaining time as plain integer hours
// (1-8). We display them as "1h", "2h" … "8h" or "Off".
// ─────────────────────────────────────────────────────────────────────────────

std::string IntexSpa::hours_to_option_(uint8_t hours) {
  if (hours == 0) return "Off";
  char buf[8];
  snprintf(buf, sizeof(buf), "%dh", static_cast<int>(hours));
  return std::string(buf);
}

uint8_t IntexSpa::option_to_hours_(const std::string &opt) {
  if (opt == "Off") return 0;
  return static_cast<uint8_t>(std::stoi(opt));
}

// ─────────────────────────────────────────────────────────────────────────────
// SpaSwitch::write_state
// ─────────────────────────────────────────────────────────────────────────────

void SpaSwitch::write_state(bool state) {
  // While the spa is off, the pump only accepts a power-ON command; every
  // other switch is a no-op on the physical unit. Snap the UI back to
  // reality immediately instead of queuing a command that will never be
  // acknowledged (and would otherwise leave "Sending Command" stuck on).
  if (!this->parent_->is_power_on() && this->command_ != CMD_ON_OFF) {
    this->publish_state(false);
    return;
  }
  this->publish_state(state);  // optimistic; corrected on next pump frame
  this->parent_->queue_toggle(this->command_, this->status_bit_, state);
}

// ─────────────────────────────────────────────────────────────────────────────
// SpaTimerSelect::control
// ─────────────────────────────────────────────────────────────────────────────

void SpaTimerSelect::control(const std::string &value) {
  // Filter/sanitizer timers can't be set while the spa is off – the pump
  // ignores these commands entirely. Snap back to "Off" immediately.
  if (!this->parent_->is_power_on()) {
    this->publish_state("Off");
    return;
  }
  this->publish_state(value);
  uint8_t hours = this->parent_->option_to_hours_(value);
  if (function_ == Function::FILTER)
    this->parent_->set_filter_time(hours);
  else
    this->parent_->set_sanitizer_time(hours);
}

// ─────────────────────────────────────────────────────────────────────────────
// SpaClimate
// ─────────────────────────────────────────────────────────────────────────────

climate::ClimateTraits SpaClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
  // add_feature_flags() is the non-deprecated API (replaces set_supports_current_temperature).
  // The flag enum values are defined directly in the esphome::climate namespace.
  // CLIMATE_TRAIT_CURRENT_TEMPERATURE = 1 << 0 (supports showing current temperature)
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(10.0f);
  traits.set_visual_max_temperature(40.0f);
  traits.set_visual_temperature_step(1.0f);
  return traits;
}

void SpaClimate::control(const climate::ClimateCall &call) {
  if (!this->parent_->is_power_on()) {
    // Pump ignores heater/setpoint commands entirely while off. Snap the
    // climate entity back to OFF/current values instead of queuing anything.
    this->mode = climate::CLIMATE_MODE_OFF;
    this->publish_state();
    return;
  }
  if (call.get_mode().has_value()) {
    auto mode      = call.get_mode().value();
    bool want_heat = (mode == climate::CLIMATE_MODE_HEAT);
    bool is_heat   = (this->mode == climate::CLIMATE_MODE_HEAT);
    if (want_heat != is_heat) {
      this->parent_->queue_toggle(CMD_HEATER);
    }
    this->mode = mode;
    this->publish_state();
  }
  if (call.get_target_temperature().has_value()) {
    uint8_t target = static_cast<uint8_t>(call.get_target_temperature().value());
    this->target_temperature = static_cast<float>(target);
    this->parent_->queue_temperature(target);
    this->publish_state();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// IntexSpa::setup
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Intex PureSpa (network_id=0x%04X, channel=0x%02X, model=%d)",
                network_id_, channel_, model_);

  // Configure LC12S control pins using esp-idf GPIO driver (not Arduino)
  // Reset pins first to ensure clean state before reconfiguring
  gpio_reset_pin(static_cast<gpio_num_t>(cs_pin_));
  gpio_reset_pin(static_cast<gpio_num_t>(set_pin_));

  gpio_config_t io_cfg = {};
  io_cfg.pin_bit_mask = (1ULL << cs_pin_) | (1ULL << set_pin_);
  io_cfg.mode         = GPIO_MODE_OUTPUT;
  io_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
  io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_cfg.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&io_cfg);

  gpio_set_level(static_cast<gpio_num_t>(cs_pin_),  0);
  gpio_set_level(static_cast<gpio_num_t>(set_pin_), 0);

  // GPIO pin level confirmation
  ESP_LOGW(TAG, "GPIO%d (CS)  level: %d (should be 0)",
           cs_pin_,  gpio_get_level(static_cast<gpio_num_t>(cs_pin_)));
  ESP_LOGW(TAG, "GPIO%d (SET) level: %d (should be 0)",
           set_pin_, gpio_get_level(static_cast<gpio_num_t>(set_pin_)));

  // UART2 stays active for TX. We use uart_write_bytes(UART_NUM_2) for sending
  // and this->read_byte() (ESPHome UART1) for receiving. This avoids the
  // double-read problem while ensuring both TX and RX pins are correctly muxed.
  {
    uart_driver_delete(UART_NUM_2);
    uart_config_t u2cfg = {};
    u2cfg.baud_rate  = 9600;
    u2cfg.data_bits  = UART_DATA_8_BITS;
    u2cfg.parity     = UART_PARITY_DISABLE;
    u2cfg.stop_bits  = UART_STOP_BITS_1;
    u2cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    u2cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(UART_NUM_2, 256, 256, 0, NULL, 0) == ESP_OK) {
      uart_param_config(UART_NUM_2, &u2cfg);
      uart_set_pin(UART_NUM_2, GPIO_NUM_17, GPIO_NUM_16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
      ESP_LOGW(TAG, "UART2 active: TX via uart_write_bytes, RX via ESPHome UART1");
    }
  }

  // Load NVS channel (informational – does not override configured channel)
  {
    ESPPreferenceObject pref = global_preferences->make_preference<uint8_t>(
        fnv1_hash("intex_spa_channel"));
    uint8_t saved = 0xFF;
    if (pref.load(&saved) && saved <= 0x7F) {
      nvs_channel_ = saved;
      ESP_LOGI(TAG, "NVS channel found: 0x%02X", nvs_channel_);
    }
  }

  // Derive first_command_char from configured channel
  first_command_char_ = (model_ == MODEL_28458)
      ? channel_ : static_cast<uint8_t>(channel_ + 0x7F);
  ESP_LOGCONFIG(TAG, "  channel            = 0x%02X", channel_);
  ESP_LOGCONFIG(TAG, "  first_command_char = 0x%02X", first_command_char_);

  // Startup: try configured channel first
  scan_state_       = ScanState::STARTUP_CONFIG;
  scan_phase_start_ = 0;  // set after LC12S config in loop() step 2
  startup_done_     = false;
  last_good_channel_= channel_;

  // Wire switch commands
  if (power_sw_)      { power_sw_->set_command(CMD_ON_OFF);            power_sw_->set_status_bit(BIT_POWER); }
  if (bubble_sw_)     { bubble_sw_->set_command(CMD_BUBBLE);           bubble_sw_->set_status_bit(BIT_BUBBLE); }
  if (water_jet_sw_)  { water_jet_sw_->set_command(CMD_WATER_JET);     water_jet_sw_->set_status_bit(BIT_WATER_JET); }
  if (fahrenheit_sw_) { fahrenheit_sw_->set_command(CMD_CELSIUS_FAHRENHEIT); }  // no status bit: lives in BYTE_STATUS_FLAGS, not BYTE_STATUS_COMMAND

  // Wire select functions
  if (filter_select_)    filter_select_->set_function(SpaTimerSelect::Function::FILTER);
  if (sanitizer_select_) sanitizer_select_->set_function(SpaTimerSelect::Function::SANITIZER);

  // Configure LC12S in loop() on first call (avoids blocking delays in setup())
  lc12s_configured_ = false;
  lc12s_config_step_ = 0;
  lc12s_config_time_ = 0;

  last_time_receive_data_ = millis();
  last_life_signal_       = millis();

  set_availability_(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// IntexSpa::loop
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::loop() {
  // ── Non-blocking LC12S configuration state machine ─────────────────────────
  // Runs on the first few loop() calls instead of blocking in setup().
  if (!lc12s_configured_) {
    uint32_t now = millis();
    switch (lc12s_config_step_) {
      // ── Phase A: reset LC12S to a neutral default (channel=0, net_id=0) ──
      // This forces a full SET LOW/HIGH cycle on a "clean" configuration
      // before we apply the real one. In practice this has made a second
      // reboot after OTA unnecessary – toggling the module out of and back
      // into a configured state seems to shake loose whatever state an
      // OTA-triggered soft reset can leave it in.
      case 0:  // SET LOW, start waiting
        gpio_set_level(static_cast<gpio_num_t>(set_pin_), 0);
        lc12s_config_time_ = now;
        lc12s_config_step_ = 1;
        return;
      case 1:  // Wait 500ms then send DEFAULT config bytes (SET stays LOW)
        if (now - lc12s_config_time_ < 500) return;
        set_lc12s_channel_(0x00, 0x0000);   // default reset; SET stays LOW
        lc12s_config_time_ = now;
        lc12s_config_step_ = 2;
        return;
      case 2:  // Wait 1000ms (SET still LOW), then bring SET HIGH
        if (now - lc12s_config_time_ < 1000) return;
        gpio_set_level(static_cast<gpio_num_t>(set_pin_), 1);
        ESP_LOGD(TAG, "SET=HIGH (default reset applied: ch=0x00, net=0x0000)");
        lc12s_config_time_ = now;
        lc12s_config_step_ = 3;
        return;
      case 3:  // Hold the default config for 3s, then start the real config
        if (now - lc12s_config_time_ < 3000) return;
        ESP_LOGD(TAG, "Default reset settled – applying real configuration");
        lc12s_config_step_ = 4;
        return;  // fall through to case 4 on the next loop() tick

      // ── Phase B: apply the real configured channel/network_id ──────────
      case 4:  // SET LOW, start waiting
        gpio_set_level(static_cast<gpio_num_t>(set_pin_), 0);
        lc12s_config_time_ = now;
        lc12s_config_step_ = 5;
        return;
      case 5:  // Wait 500ms then send config bytes (SET stays LOW)
        if (now - lc12s_config_time_ < 500) return;
        set_lc12s_channel_(channel_, network_id_);   // real config; sends bytes only, SET stays LOW
        lc12s_config_time_ = now;
        lc12s_config_step_ = 6;
        return;
      case 6:  // Wait 1000ms (SET still LOW), then bring SET HIGH
        if (now - lc12s_config_time_ < 1000) return;
        gpio_set_level(static_cast<gpio_num_t>(set_pin_), 1);
        ESP_LOGD(TAG, "SET=HIGH (config applied)");
        lc12s_config_time_ = now;
        lc12s_config_step_ = 7;
        return;
      case 7:  // Small settle time after SET HIGH, then flush and start
        if (now - lc12s_config_time_ < 100) return;
        // Flush LC12S config response bytes
        while (this->available()) { uint8_t c; this->read_byte(&c); }
        lc12s_configured_       = true;
        last_time_receive_data_ = millis();
        last_life_signal_       = millis();
        scan_phase_start_       = millis();
        scan_state_             = ScanState::STARTUP_CONFIG;
        ESP_LOGW(TAG, "=== LC12S ready: ch=0x%02X FCC=0x%02X net=0x%04X active_scan=%s ===",
                 channel_, first_command_char_, network_id_, active_scan_ ? "YES" : "NO");
        return;
    }
  }

  // ── LC12S channel switch state machine ────────────────────────────────────
  if (lc12s_sw_state_ != LC12SState::IDLE) {
    uint32_t now_sw = millis();
    switch (lc12s_sw_state_) {
      case LC12SState::SET_LOW_WAIT:
        if (now_sw - lc12s_sw_time_ >= 500) {
          lc12s_sw_state_ = LC12SState::SEND_CONFIG;
        }
        return;
      case LC12SState::SEND_CONFIG:
        channel_            = lc12s_sw_channel_;
        first_command_char_ = (model_ == MODEL_28458)
                              ? channel_ : static_cast<uint8_t>(channel_ + 0x7F);
        set_lc12s_channel_(channel_, network_id_);   // real config; SET already LOW, sends bytes only (SET stays LOW)
        lc12s_sw_time_  = now_sw;
        lc12s_sw_state_ = LC12SState::POST_WAIT;
        return;
      case LC12SState::POST_WAIT:
        // SET stays LOW for this whole wait, matching original timing exactly:
        // SET LOW; delay(500); write bytes; delay(1000); SET HIGH.
        if (now_sw - lc12s_sw_time_ >= 1000) {
          lc12s_sw_state_ = LC12SState::SET_HIGH;
        }
        return;
      case LC12SState::SET_HIGH:
        gpio_set_level(static_cast<gpio_num_t>(set_pin_), 1);
        lc12s_sw_time_  = now_sw;
        lc12s_sw_state_ = LC12SState::FLUSH;
        return;
      case LC12SState::FLUSH:
        while (this->available()) { uint8_t c; this->read_byte(&c); }
        rx_state_       = 0;
        data_counter_   = 0;
        memset(data_, 0, sizeof(data_));
        scan_phase_start_       = millis();
        scan_bytes_seen_        = 0;
        last_time_receive_data_ = millis();  // reset so timeout doesn't fire immediately
        lc12s_sw_state_ = LC12SState::IDLE;
        ESP_LOGD(TAG, "ch=0x%02X FCC=0x%02X ready, listening...", channel_, first_command_char_);
        return;
      default: break;
    }
  }

  // Drain UART2 RX buffer silently (UART2 is only used for TX pin routing)
  {
    uint8_t dummy[32];
    uart_read_bytes(UART_NUM_2, dummy, sizeof(dummy), 0);
  }

  // 1. Drain UART RX via ESPHome UART1
  bool in_active_scan = (scan_state_ == ScanState::SWEEP ||
                         scan_state_ == ScanState::CONFIRM ||
                         scan_state_ == ScanState::RECHECK);
  if (!in_active_scan) {
    static uint32_t rx_total = 0;
    static uint32_t rx_log_time = 0;

    if (this->available() > 0) {
      // Bytes arriving means the RF link is alive even if a frame turns out
      // corrupted later (e.g. CRC errors from our own TX echo interfering
      // during command sends). Reset the comm timeout here so a burst of
      // corrupted frames right after sending a command doesn't falsely
      // trigger "Communication with pump lost!".
      last_time_receive_data_ = millis();
    }

    while (this->available()) {
      uint8_t c;
      this->read_byte(&c);
      rx_total++;
      ESP_LOGV(TAG, "RX: 0x%02X (sync=0x%02X %s)", c, first_command_char_,
               c == first_command_char_ ? "MATCH" : "");
      read_data_(c);
    }

    if (millis() - rx_log_time > 5000) {
      rx_log_time = millis();
      ESP_LOGD(TAG, "RX stats: %lu bytes | state=%d cnt=%d | ch=0x%02X",
               (unsigned long)rx_total, (int)rx_state_, (int)data_counter_, channel_);
    }
  }

  // 2. Process complete pump frame
  if (finish_pump_message_) {
    last_time_receive_data_ = millis();

    // Restore availability on first frame after offline period
    if (comm_error_) {
      comm_error_ = false;
      set_availability_(true);
      if (comm_error_bs_) comm_error_bs_->publish_state(false);
      ESP_LOGI(TAG, "Communication with pump restored.");
    }
    if (!startup_done_) {
      startup_done_ = true;
      scan_state_   = ScanState::IDLE;
      ESP_LOGI(TAG, "First frame received – channel 0x%02X confirmed", channel_);
      if (scanning_bs_) scanning_bs_->publish_state(false);
    }
    last_good_channel_ = channel_;

    data_management_();
    process_command_queue_();
    rx_state_            = 0;
    first_send_          = true;
    finish_pump_message_ = false;
  }

  // 3. Controller frame – only update loading state from genuine remote panel frames.
  // Our own TX echo has a valid CRC and creates a feedback loop if we read it.
  if (finish_controller_message_) {
    if (memcmp(data_controller_, last_tx_frame_, SIZE_CONTROLLER_DATA) != 0)
      controller_loading_state_ = data_controller_[BYTE_CONTROLLER_LOADING];
    finish_controller_message_ = false;
  }

  // 4. Toggle command or life signal, both on the same 1600ms cadence.
  // IMPORTANT: a pending toggle must still be sent periodically here, not
  // only inside process_command_queue_ (which only runs after a full valid
  // pump frame arrives) – otherwise, since send_life_signal_() itself
  // refuses to send while is_busy_ is true, nothing would ever prompt the
  // pump again once a toggle is queued, deadlocking all communication.
  // process_command_queue_ can still stop the toggle earlier via status-bit
  // confirmation once a valid frame does arrive.
  if (millis() - last_life_signal_ > 1600) {
    if (pending_toggle_cmd_) {
      send_raw_command_(pending_toggle_cmd_);
    } else {
      send_life_signal_();
    }
    last_life_signal_ = millis();
  }

  // 5. Communication watchdog
  uint32_t since_data = millis() - last_time_receive_data_;
  if (!comm_error_ && since_data > COMM_TIMEOUT_MS) {
    comm_error_ = true;
    ESP_LOGW(TAG, "Communication with pump lost!");
    set_availability_(false);
    if (comm_error_bs_) comm_error_bs_->publish_state(true);
    if (spa_active_bs_) spa_active_bs_->publish_state(false);
  }

  // 6. Startup channel progression
  if (lc12s_sw_state_ == LC12SState::IDLE) {
    if (scan_state_ == ScanState::STARTUP_CONFIG) {
      if (since_data > STARTUP_CONFIG_MS) {
        if (nvs_channel_ != 0xFF && nvs_channel_ != channel_) {
          ESP_LOGW(TAG, "Config ch=0x%02X timed out – trying NVS 0x%02X (15s)",
                   channel_, nvs_channel_);
          scan_state_ = ScanState::STARTUP_NVS;
          switch_to_channel_(nvs_channel_);
        } else {
          ESP_LOGW(TAG, "Config ch=0x%02X timed out – starting scan", channel_);
          start_scan_sweep_();
        }
      }
      return;
    }

    if (scan_state_ == ScanState::STARTUP_NVS) {
      if (since_data > STARTUP_NVS_MS) {
        ESP_LOGW(TAG, "NVS ch=0x%02X timed out – starting scan", nvs_channel_);
        start_scan_sweep_();
      }
      return;
    }
  } else {
    // Channel switch in progress – skip all timeout logic
    return;
  }

  // 7. Active scan state machine
  if (in_active_scan) {
    scan_tick_();
    return;
  }

  // 8. Runtime scan trigger: was connected, now lost for 90s
  if (comm_error_ && startup_done_ && since_data > RUNTIME_SCAN_DELAY_MS) {
    start_scan_sweep_();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// IntexSpa::dump_config
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::dump_config() {
  ESP_LOGCONFIG(TAG, "Intex PureSpa:");
  ESP_LOGCONFIG(TAG, "  Model       : %d",     model_);
  ESP_LOGCONFIG(TAG, "  Network ID  : 0x%04X", network_id_);
  ESP_LOGCONFIG(TAG, "  Channel     : 0x%02X  (first_cmd=0x%02X)", channel_, first_command_char_);
  ESP_LOGCONFIG(TAG, "  CS pin      : GPIO%d", cs_pin_);
  ESP_LOGCONFIG(TAG, "  SET pin     : GPIO%d", set_pin_);
  ESP_LOGCONFIG(TAG, "  Active scan : %s",     active_scan_ ? "YES" : "NO");
  if (nvs_channel_ != 0xFF)
    ESP_LOGCONFIG(TAG, "  NVS channel : 0x%02X", nvs_channel_);
  else
    ESP_LOGCONFIG(TAG, "  NVS channel : (none)");
}

// ─────────────────────────────────────────────────────────────────────────────
// switch_to_channel_ – configure LC12S for a channel and update FCC
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::switch_to_channel_(uint8_t ch) {
  // Start the LC12S switch state machine:
  // SET LOW → wait 500ms → send config → wait 500ms → flush → listen
  gpio_set_level(static_cast<gpio_num_t>(set_pin_), 0);
  lc12s_sw_channel_ = ch;
  lc12s_sw_time_    = millis();
  lc12s_sw_state_   = LC12SState::SET_LOW_WAIT;
  ESP_LOGD(TAG, "Switching to ch=0x%02X (SET LOW, waiting 500ms...)", ch);
}

// ─────────────────────────────────────────────────────────────────────────────
// start_scan_sweep_ – begin full channel sweep (called after timeouts)
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::start_scan_sweep_() {
  scan_state_    = ScanState::SWEEP;
  scan_channel_  = 0;
  scan_candidate_= 0xFF;
  scan_bytes_seen_= 0;
  scan_phase_start_   = millis();
  scan_last_recheck_  = millis();

  ESP_LOGW(TAG, "=== Channel scan started (%s, network_id=0x%04X) ===",
           active_scan_ ? "active" : "passive", network_id_);
  if (scanning_bs_) scanning_bs_->publish_state(true);
  switch_to_channel_(scan_channel_);
  ESP_LOGI(TAG, "Scan: ch=0x%02X (1/128)", scan_channel_);
}

// ─────────────────────────────────────────────────────────────────────────────
// scan_tick_ – unified scan state machine
// States: STARTUP_CONFIG → STARTUP_NVS → SWEEP → CONFIRM → RECHECK → IDLE
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::scan_tick_() {
  uint32_t now = millis();

  // Active scan: send life signal every 1600ms (active mode only)
  if (active_scan_) {
    if (now - last_life_signal_ > 1600) {
      send_raw_command_(0x0000);
      last_life_signal_ = now;
    }
  }

  // Drain RX with verbose logging
  while (this->available()) {
    uint8_t c;
    this->read_byte(&c);
    scan_bytes_seen_++;
    ESP_LOGV(TAG, "Scan RX ch=0x%02X: 0x%02X (sync=0x%02X %s n=%lu)",
             scan_channel_, c, first_command_char_,
             c == first_command_char_ ? "MATCH" : "",
             (unsigned long)scan_bytes_seen_);
    if (scan_state_ == ScanState::CONFIRM || scan_state_ == ScanState::RECHECK)
      read_data_(c);
  }

  // ── RECHECK: briefly check last good channel mid-scan ───────────────────
  if (scan_state_ == ScanState::RECHECK) {
    while (this->available()) { uint8_t c; this->read_byte(&c); read_data_(c); }
    if (finish_pump_message_) {
      // Last good channel is working again!
      finish_pump_message_ = false;
      uint8_t found = last_good_channel_;
      ESP_LOGW(TAG, "=== Recheck: channel 0x%02X is back! ===", found);
      scan_state_ = ScanState::IDLE;
      channel_    = found;
      if (scanning_bs_) scanning_bs_->publish_state(false);
      ESPPreferenceObject pref = global_preferences->make_preference<uint8_t>(
          fnv1_hash("intex_spa_channel"));
      pref.save(&found);
      last_time_receive_data_ = now;
      comm_error_             = false;
      if (comm_error_bs_) comm_error_bs_->publish_state(false);
      return;
    }
    // Recheck timed out (3s) – back to sweep
    if (now - scan_phase_start_ > 3000) {
      scan_state_      = ScanState::SWEEP;
      scan_phase_start_= now;
      switch_to_channel_(scan_channel_);
      ESP_LOGD(TAG, "Recheck done, back to sweep ch=0x%02X", scan_channel_);
    }
    return;
  }

  // ── CONFIRM: validate candidate with CRC-checked frame ──────────────────
  if (scan_state_ == ScanState::CONFIRM) {
    while (this->available()) { uint8_t c; this->read_byte(&c); read_data_(c); }

    if (finish_pump_message_) {
      finish_pump_message_ = false;
      rx_state_ = 0; data_counter_ = 0;

      uint8_t found = scan_candidate_;
      ESP_LOGW(TAG, "=== Scan SUCCESS: channel=0x%02X ===", found);
      scan_state_        = ScanState::IDLE;
      last_good_channel_ = found;
      startup_done_      = true;
      if (scanning_bs_) scanning_bs_->publish_state(false);

      ESPPreferenceObject pref = global_preferences->make_preference<uint8_t>(
          fnv1_hash("intex_spa_channel"));
      pref.save(&found);

      switch_to_channel_(found);
      last_time_receive_data_ = now;
      comm_error_             = false;
      if (comm_error_bs_) comm_error_bs_->publish_state(false);
      return;
    }

    if (now - scan_phase_start_ > SCAN_CONFIRM_MS) {
      ESP_LOGW(TAG, "Confirm timeout ch=0x%02X, continuing sweep", scan_candidate_);
      scan_state_      = ScanState::SWEEP;
      scan_channel_    = (scan_candidate_ + 1) & 0x7F;
      scan_bytes_seen_ = 0;
      scan_phase_start_= now;
      switch_to_channel_(scan_channel_);
      ESP_LOGI(TAG, "Scan: ch=0x%02X (%d/128)", scan_channel_, scan_channel_ + 1);
    }
    return;
  }

  // ── SWEEP: iterate channels, count bytes ────────────────────────────────
  // Every SCAN_RECHECK_MS, briefly check last_good_channel_
  if (last_good_channel_ != 0xFF && now - scan_last_recheck_ > SCAN_RECHECK_MS) {
    scan_last_recheck_ = now;
    if (last_good_channel_ != scan_channel_) {
      ESP_LOGD(TAG, "Scan: rechecking last good channel 0x%02X", last_good_channel_);
      scan_state_      = ScanState::RECHECK;
      scan_phase_start_= now;
      switch_to_channel_(last_good_channel_);
      return;
    }
  }

  // Drain RX, count bytes
  while (this->available()) {
    uint8_t c;
    this->read_byte(&c);
    scan_bytes_seen_++;
    ESP_LOGV(TAG, "Scan RX ch=0x%02X: 0x%02X (n=%lu)", scan_channel_, c, (unsigned long)scan_bytes_seen_);
  }

  uint32_t dwell = active_scan_ ? SCAN_ACTIVE_DWELL_MS : SCAN_PASSIVE_DWELL_MS;
  if (now - scan_phase_start_ < dwell) return;

  if (scan_bytes_seen_ >= 1) {
    // Candidate found – enter confirm phase
    scan_candidate_  = scan_channel_;
    scan_state_      = ScanState::CONFIRM;
    scan_phase_start_= now;
    rx_state_ = 0; data_counter_ = 0; memset(data_, 0, sizeof(data_));
    ESP_LOGW(TAG, "Scan: ch=0x%02X had %lu bytes – confirming (10s)...",
             scan_candidate_, (unsigned long)scan_bytes_seen_);
    // Already on correct channel + network_id, just reset parser
    return;
  }

  // No bytes – next channel
  scan_channel_ = (scan_channel_ + 1) & 0x7F;
  if (scan_channel_ == 0) ESP_LOGW(TAG, "Scan: full sweep done, restarting...");
  scan_bytes_seen_ = 0;
  scan_phase_start_= now;
  switch_to_channel_(scan_channel_);
  ESP_LOGI(TAG, "Scan: ch=0x%02X (%d/128)", scan_channel_, scan_channel_ + 1);
}

// FREEZE: stop publishing anything. Last known values stay in HA.
// Only comm_error and spa_active binary sensors are updated.
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::set_availability_(bool available) {
  if (available) return;  // Restore happens automatically on next data_management_ call

  // Reset power tracking so the next frame triggers a clean power-on publish
  power_on_      = false;
  last_power_on_ = false;

  // Only update the diagnostic sensors – everything else is frozen at last value
  if (spa_active_bs_) spa_active_bs_->publish_state(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// publish_power_off_ – called when spa power turns off.
// Set all function states to off/false so HA reflects reality.
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::publish_power_off_() {
  ESP_LOGI(TAG, "Spa powered off – publishing all-off states");

  // Power switch - commented out because I think this causes issues on quick toggles. We get this in next status frame anyways
  //if (power_sw_)      power_sw_->publish_state(false);

  // Function switches
  if (bubble_sw_)     bubble_sw_->publish_state(false);
  if (water_jet_sw_)  water_jet_sw_->publish_state(false);

  // Function binary sensors
  if (heater_active_bs_)  heater_active_bs_->publish_state(false);
  if (water_filter_bs_)   water_filter_bs_->publish_state(false);
  if (bubble_bs_)         bubble_bs_->publish_state(false);
  if (water_jet_bs_)      water_jet_bs_->publish_state(false);
  if (sanitizer_bs_)      sanitizer_bs_->publish_state(false);

  // Selects → Off
  if (filter_select_)    filter_select_->publish_state("Off");
  if (sanitizer_select_) sanitizer_select_->publish_state("Off");

  // Climate → OFF mode, keep last known temperatures
  if (climate_) {
    climate_->mode   = climate::CLIMATE_MODE_OFF;
    climate_->action = climate::CLIMATE_ACTION_OFF;
    climate_->publish_state();
  }

  // spa_active → false
  if (spa_active_bs_) spa_active_bs_->publish_state(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::set_target_temperature(float temp) {
  queue_temperature(static_cast<uint8_t>(temp));
}

void IntexSpa::set_filter_time(uint8_t hours) {
  queue_filter(hours, hours == 0);
}

void IntexSpa::set_sanitizer_time(uint8_t hours) {
  queue_sanitizer(hours, hours == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// read_data_ – RX state machine
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::read_data_(uint8_t c) {
  switch (rx_state_) {
    case 0:  // wait for sync byte
      data_counter_ = 0;
      memset(data_,            0, sizeof(data_));
      memset(data_controller_, 0, sizeof(data_controller_));
      if (c == first_command_char_) {
        data_[data_counter_++] = c;
        rx_state_ = 1;
      }
      break;

    case 1:  // collect bytes (1:1 port of Yogis ReadData)
      data_[data_counter_++] = c;

      // If first_command_char appears between byte SIZE_CONTROLLER_DATA and
      // SIZE_PUMP_DATA: the first 8 bytes were a controller frame (TX echo or
      // remote panel). Copy them, start fresh with this byte as new frame header.
      if (c == first_command_char_
          && data_counter_ > SIZE_CONTROLLER_DATA
          && data_counter_ < SIZE_PUMP_DATA) {
        memcpy(data_controller_, data_, SIZE_CONTROLLER_DATA);
        uint16_t crc = calc_crc_(data_controller_, SIZE_CONTROLLER_DATA - 2);
        finish_controller_message_ =
            (data_controller_[SIZE_CONTROLLER_DATA - 2] == ((crc & 0xFF00) >> 8) &&
             data_controller_[SIZE_CONTROLLER_DATA - 1] ==  (crc & 0x00FF));
        data_counter_          = 0;
        data_[data_counter_++] = c;
        break;
      }

      // Full pump frame received
      if (data_counter_ > SIZE_PUMP_DATA - 1) {
        uint16_t crc = calc_crc_(data_, SIZE_PUMP_DATA - 2);
        if (data_[SIZE_PUMP_DATA - 2] == ((crc & 0xFF00) >> 8) &&
            data_[SIZE_PUMP_DATA - 1] ==  (crc & 0x00FF)) {
          finish_pump_message_ = true;
        } else {
          ESP_LOGD(TAG, "CRC mismatch: expected 0x%04X, got 0x%02X%02X – frame dropped",
                   crc, data_[SIZE_PUMP_DATA - 2], data_[SIZE_PUMP_DATA - 1]);
        }
        rx_state_ = 0;
      }
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// data_management_ – parse frame, publish all states
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::data_management_() {
  const uint8_t status_cmd  = data_[BYTE_STATUS_COMMAND];
  const uint8_t status_flag = data_[BYTE_STATUS_FLAGS];
  const uint8_t actual_temp = data_[BYTE_ACTUAL_TEMP];
  const uint8_t setpt_temp  = data_[BYTE_SETPOINT_TEMP];
  const uint8_t filter_h    = data_[BYTE_FILTER_TIME];
  const uint8_t sanit_h     = (model_ == MODEL_28458) ? data_[BYTE_SANITIZER_TIME] : 0;
  const uint8_t error_code  = data_[BYTE_ERROR];

  // Decode flags
  const bool power_on        = (status_cmd & BIT_POWER)          != 0;
  const bool heater_standby  = (status_cmd & BIT_HEATER_STANDBY) != 0;
  const bool heater_on       = (status_cmd & BIT_HEATER_ON)      != 0;
  bool new_state_filter      = (status_cmd & BIT_WATER_FILTER)   != 0;
  const bool bubble_on       = (status_cmd & BIT_BUBBLE)         != 0;
  const bool water_jet_on    = (model_ == MODEL_28458) && ((status_cmd & BIT_WATER_JET) != 0);
  bool new_state_sanitizer   = (model_ == MODEL_28458) && ((status_cmd & BIT_SANITIZER) != 0);
  fahrenheit_                = (status_flag & BIT_FAHRENHEIT)    != 0;
  command_received_          = (status_flag & BIT_CMD_RECEIVED)  != 0;

  // DIAGNOSTIC: dump the full raw frame whenever filter/sanitizer status
  // flips, so an unexpected "off" blip can be traced back to real byte
  // content instead of guessed at. Safe/read-only, no behavior change.
  if (new_state_filter != state_filter_ || new_state_sanitizer != state_sanitizer_) {
    ESP_LOGW(TAG, "Status change: filter %d->%d, sanitizer %d->%d | raw frame: "
             "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             (int)state_filter_, (int)new_state_filter,
             (int)state_sanitizer_, (int)new_state_sanitizer,
             data_[0],data_[1],data_[2],data_[3],data_[4],data_[5],data_[6],data_[7],data_[8],
             data_[9],data_[10],data_[11],data_[12],data_[13],data_[14],data_[15],data_[16]);
  }
  state_filter_    = new_state_filter;
  state_sanitizer_ = new_state_sanitizer;

  // Always update actual tracked values so process_command_queue_ can compare
  actual_setpoint_temp_  = setpt_temp;
  actual_filter_time_    = filter_h;
  actual_sanitizer_time_ = sanit_h;

  // TX blackout: suppress UI publishing for 250ms after sending a command
  // to prevent bounce-back. Actual values above are always updated.
  bool in_blackout = (millis() - last_tx_time_ < TX_BLACKOUT_MS);

  // ── Publish sensors (always, skip during blackout to avoid flicker) ───────
  if (!in_blackout) {
    // Only publish when value changed to reduce HA event flood and log noise
    if (actual_temp_sensor_ && actual_temp_sensor_->get_raw_state() != static_cast<float>(actual_temp))
      actual_temp_sensor_->publish_state(static_cast<float>(actual_temp));
    if (error_code_sensor_ && error_code_sensor_->get_raw_state() != static_cast<float>(error_code))
      error_code_sensor_->publish_state(static_cast<float>(error_code));
    if (filter_remaining_sensor_ && filter_remaining_sensor_->get_raw_state() != static_cast<float>(filter_h))
      filter_remaining_sensor_->publish_state(static_cast<float>(filter_h));
    if (sanitizer_remaining_sensor_ && sanitizer_remaining_sensor_->get_raw_state() != static_cast<float>(sanit_h))
      sanitizer_remaining_sensor_->publish_state(static_cast<float>(sanit_h));
  }

  // ── Power state change handling ───────────────────────────────────────────
  if (power_on != last_power_on_) {
    last_power_on_ = power_on;
    if (!power_on) {
      publish_power_off_();
    } else {
      ESP_LOGI(TAG, "Spa powered on");
      if (spa_active_bs_) spa_active_bs_->publish_state(true);
    }
  }
  power_on_ = power_on;

  if (!power_on) {
    // Don't overwrite the optimistic "on" state while a power-ON toggle is
    // still pending – otherwise a pump frame that hasn't caught up yet
    // (still reporting the old "off" state) flips the switch back to off
    // for a moment before the real toggle lands, causing a visible bounce.
    if (!in_blackout && !is_busy_ && power_sw_) power_sw_->publish_state(false);
    if (error_code != last_error_code_) {
      handle_error_notification_(error_code);
      last_error_code_ = error_code;
    }
    return;
  }

  if (in_blackout) return;  // skip all remaining UI publishing during blackout

  // ── Publish binary sensors ───────────────────────────────────────────────
  bool heater_active = heater_on && !heater_standby;
  if (heater_active_bs_)  heater_active_bs_->publish_state(heater_active);
  if (water_filter_bs_)   water_filter_bs_->publish_state(state_filter_);
  if (bubble_bs_)         bubble_bs_->publish_state(bubble_on);
  if (water_jet_bs_)      water_jet_bs_->publish_state(water_jet_on);
  if (sanitizer_bs_)      sanitizer_bs_->publish_state(state_sanitizer_);

  // ── Sync switches – skip while busy to avoid overriding optimistic state ──
  if (!is_busy_) {
    if (power_sw_)      power_sw_->publish_state(true);
    if (bubble_sw_)     bubble_sw_->publish_state(bubble_on);
    if (water_jet_sw_)  water_jet_sw_->publish_state(water_jet_on);
    if (fahrenheit_sw_) fahrenheit_sw_->publish_state(fahrenheit_);

    // Only publish selects when value changed
    if (filter_select_) {
      auto s = hours_to_option_(filter_h);
      if (filter_select_->current_option() != s) filter_select_->publish_state(s);
    }
    if (sanitizer_select_) {
      auto s = hours_to_option_(sanit_h);
      if (sanitizer_select_->current_option() != s) sanitizer_select_->publish_state(s);
    }
  }

  // ── Sync climate entity ──────────────────────────────────────────────────
  if (climate_) {
    auto new_mode   = (heater_on || heater_standby) ? climate::CLIMATE_MODE_HEAT : climate::CLIMATE_MODE_OFF;
    auto new_action = heater_on ? climate::CLIMATE_ACTION_HEATING
                    : (heater_standby ? climate::CLIMATE_ACTION_IDLE : climate::CLIMATE_ACTION_OFF);
    float new_cur = static_cast<float>(actual_temp);
    float new_tgt = want_temp_change_ ? climate_->target_temperature
                                      : static_cast<float>(actual_setpoint_temp_);
    bool changed = (climate_->mode != new_mode || climate_->action != new_action ||
                    climate_->current_temperature != new_cur || climate_->target_temperature != new_tgt);
    if (changed) {
      climate_->mode                = new_mode;
      climate_->action              = new_action;
      climate_->current_temperature = new_cur;
      climate_->target_temperature  = new_tgt;
      climate_->publish_state();
    }
  }

  // ── Error notifications ───────────────────────────────────────────────────
  if (error_code != last_error_code_) {
    handle_error_notification_(error_code);
    last_error_code_ = error_code;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// handle_error_notification_ – logs error codes.
// HA persistent notifications are triggered by the YAML on_value automation
// on the error_code sensor (see intex_spa.yaml). This avoids any dependency
// on the unstable ESPHome C++ API server internals.
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::handle_error_notification_(uint8_t error_code) {
  switch (error_code) {
    case ERR_NONE:     ESP_LOGI(TAG, "Spa error cleared");                            break;
    case ERR_NO_FLOW:  ESP_LOGW(TAG, "E90: No water flow");                           break;
    case ERR_LOW_SALT: ESP_LOGW(TAG, "E91: Salt level low (< 2500 ppm)");             break;
    case ERR_HIGH_SALT:ESP_LOGW(TAG, "E92: Salt level high (> 2500 ppm)");            break;
    case ERR_TEMP_TOO_LOW:  ESP_LOGW(TAG, "E94: Water temperature too low");          break;
    case ERR_TEMP_TOO_HIGH: ESP_LOGW(TAG, "E95: Water temperature too high (~50C)");  break;
    case ERR_SYSTEM:   ESP_LOGW(TAG, "E96: System error");                            break;
    case ERR_DRY_FIRE: ESP_LOGW(TAG, "E97: Dry-fire protection triggered");           break;
    case ERR_TEMP_SENSOR: ESP_LOGW(TAG, "E99: Temperature sensor broken");            break;
    default:           ESP_LOGW(TAG, "Unknown error code: %d", error_code);           break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// process_command_queue_ – hierarchical command dispatcher
//
// Priority (highest first):
//   1. Temperature setpoint (step up/down one degree per frame)
//   2. Filter timer (step up/down)
//   3. Sanitizer timer (step up/down)
//   4. Pending toggle command (single-shot)
//
// Only one step is sent per pump frame. is_busy_ is true while any
// step-adjustment or single-shot command is in progress.
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::process_command_queue_() {
  bool was_busy = is_busy_;

  // Safety net: when the spa is off, only a power-ON toggle is a real,
  // executable command (the pump ignores everything else while off).
  // Discard anything else that may have slipped into the queue so we never
  // sit "busy" waiting for a command the pump will never acknowledge, and
  // never accidentally power the spa on via a side effect.
  if (!power_on_) {
    want_temp_change_      = false;
    want_filter_change_    = false;
    want_sanitizer_change_ = false;
    if (pending_toggle_cmd_ != CMD_ON_OFF) pending_toggle_cmd_ = 0;
  }

  // ── 1. Temperature setpoint ──────────────────────────────────────────────
  if (want_temp_change_) {
    if (want_temp_ == actual_setpoint_temp_) {
      want_temp_change_ = false;
      ESP_LOGI(TAG, "Setpoint reached: %d°", want_temp_);
    } else {
      is_busy_ = true;
      if (was_busy != is_busy_ && is_sending_bs_) is_sending_bs_->publish_state(true);
      // Only send one step per pump frame cycle; actual_setpoint_temp_ is
      // updated by data_management_ before this call, so we always see fresh value.
      uint16_t cmd = (want_temp_ > actual_setpoint_temp_) ? CMD_INCREASE : CMD_DECREASE;
      ESP_LOGD(TAG, "Setpoint step: actual=%d want=%d cmd=0x%04X",
               actual_setpoint_temp_, want_temp_, cmd);
      send_raw_command_(cmd);
      return;
    }
  }

  // ── 2. Filter timer ───────────────────────────────────────────────────────
  else if (want_filter_change_) {
    bool done = want_filter_off_ ? (actual_filter_time_ == 0)
                                 : (actual_filter_time_ == want_filter_);
    if (done) {
      want_filter_change_ = false;
      ESP_LOGI(TAG, "Filter timer reached: %d h", want_filter_);
    } else {
      is_busy_ = true;
      send_raw_command_(CMD_WATER_FILTER);
      if (was_busy != is_busy_ && is_sending_bs_) is_sending_bs_->publish_state(true);
      return;
    }
  }

  // ── 3. Sanitizer timer ────────────────────────────────────────────────────
  else if (want_sanitizer_change_ && model_ == MODEL_28458) {
    bool done = want_sanitizer_off_ ? (actual_sanitizer_time_ == 0)
                                    : (actual_sanitizer_time_ == want_sanitizer_);
    if (done) {
      want_sanitizer_change_ = false;
      ESP_LOGI(TAG, "Sanitizer timer reached: %d h", want_sanitizer_);
    } else {
      is_busy_ = true;
      send_raw_command_(CMD_SANITIZER);
      if (was_busy != is_busy_ && is_sending_bs_) is_sending_bs_->publish_state(true);
      return;
    }
  }

  // ── 4. Pending toggle command ─────────────────────────────────────────────
  // Send once per pump frame. Stop when:
  //  a) status bit confirms desired state (most reliable, avoids over-toggling)
  //  b) command_received_ flag set (pump acknowledged)
  //  c) 5s timeout
  else if (pending_toggle_cmd_) {
    is_busy_ = true;
    if (was_busy != is_busy_ && is_sending_bs_) is_sending_bs_->publish_state(true);

    bool state_confirmed = false;
    if (pending_toggle_bit_mask_ != 0) {
      bool actual = (data_[BYTE_STATUS_COMMAND] & pending_toggle_bit_mask_) != 0;
      state_confirmed = (actual == pending_toggle_desired_state_);
    }

    if (state_confirmed || command_received_) {
      ESP_LOGD(TAG, "Toggle 0x%04X done (confirmed=%d cmd_rcvd=%d)",
               pending_toggle_cmd_, state_confirmed, command_received_);
      pending_toggle_cmd_ = 0;
    } else if (millis() - pending_toggle_start_ > 5000) {
      ESP_LOGW(TAG, "Toggle 0x%04X timed out", pending_toggle_cmd_);
      pending_toggle_cmd_ = 0;
    } else {
      send_raw_command_(pending_toggle_cmd_);
      return;
    }
  }

  // ── Idle ──────────────────────────────────────────────────────────────────
  if (is_busy_) {
    is_busy_ = false;
    if (is_sending_bs_) is_sending_bs_->publish_state(false);
    ESP_LOGD(TAG, "Command queue idle");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Legacy stubs – kept for compilation; new code uses process_command_queue_
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::send_command_management_() {}
void IntexSpa::send_temperature_setpoint_() {}
void IntexSpa::send_special_command_(uint16_t, bool *, bool, uint8_t, uint8_t) {}

// ─────────────────────────────────────────────────────────────────────────────
// send_life_signal_ (from original SendLifeFct)
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::send_life_signal_() {
  if (!comm_error_ && !is_busy_)
    send_raw_command_(0x0000);
}

// ─────────────────────────────────────────────────────────────────────────────
// send_raw_command_ (from original SendCommand)
//
//  Frame layout (8 bytes):
//   [0] first_command_char
//   [1] command HIGH byte
//   [2] command LOW byte
//   [3] controller_loading_state
//   [4] 0x00
//   [5] 0x00
//   [6] CRC HIGH
//   [7] CRC LOW
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::send_raw_command_(uint16_t command) {
  uint8_t buf[SIZE_CONTROLLER_DATA];
  buf[0] = first_command_char_;
  buf[1] = static_cast<uint8_t>((command & 0xFF00) >> 8);
  buf[2] = static_cast<uint8_t>( command & 0x00FF);
  buf[3] = controller_loading_state_;
  buf[4] = 0x00;
  buf[5] = 0x00;

  uint16_t crc = calc_crc_(buf, 6);
  buf[6] = static_cast<uint8_t>((crc & 0xFF00) >> 8);
  buf[7] = static_cast<uint8_t>( crc & 0x00FF);

  // Send via UART2 directly (GPIO17 TX) – this->write_array uses UART1
  // which may not have GPIO17 correctly muxed after OTA flash.
  uart_write_bytes(UART_NUM_2, reinterpret_cast<const char*>(buf), SIZE_CONTROLLER_DATA);
  memcpy(last_tx_frame_, buf, SIZE_CONTROLLER_DATA);
  last_tx_time_ = millis();
  ESP_LOGD(TAG, "TX: %02X %02X %02X %02X %02X %02X %02X %02X",
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
}

// ─────────────────────────────────────────────────────────────────────────────
// set_lc12s_channel_ (from original SetSettings)
//
//  Sends the LC12S configuration frame. Layout is identical to original code.
//  Uses esp-idf gpio_set_level instead of Arduino digitalWrite.
// ─────────────────────────────────────────────────────────────────────────────

void IntexSpa::set_lc12s_channel_(uint8_t channel, uint16_t network_id) {
  uint8_t cfg[20]{};
  cfg[1]  = 0xAA;
  cfg[2]  = 0x5A;
  cfg[3]  = LC12S_DEVICE_ID_HIGH;
  cfg[4]  = LC12S_DEVICE_ID_LOW;
  cfg[5]  = static_cast<uint8_t>((network_id & 0xFF00) >> 8);
  cfg[6]  = static_cast<uint8_t>( network_id & 0x00FF);
  cfg[7]  = 0x00;
  cfg[8]  = 0x00;  // RF power – 0x00 as in original Spa.ino SetSettings
  cfg[9]  = 0x00;
  cfg[10] = 0x04;  // baud rate 9600
  cfg[11] = 0x00;
  cfg[12] = channel;
  cfg[13] = 0x00;
  cfg[14] = 0x00;
  cfg[15] = 0x00;
  cfg[16] = 0x12;
  cfg[17] = 0x00;

  // Checksum: sum of bytes 1..16
  for (int i = 1; i < 17; i++) cfg[18] += cfg[i];

  ESP_LOGD(TAG, "LC12S config frame: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
           cfg[1],cfg[2],cfg[3],cfg[4],cfg[5],cfg[6],cfg[7],cfg[8],
           cfg[9],cfg[10],cfg[11],cfg[12],cfg[13],cfg[14],cfg[15],cfg[16],cfg[17],cfg[18]);

  // SET pin is already LOW (caller set it and waited 500ms).
  // Send config bytes. Caller is responsible for waiting 1000ms and THEN
  // bringing SET HIGH – matching the original SetSettingsChannel() exactly:
  //   SET LOW; delay(500); write bytes; delay(1000); SET HIGH;
  // Raising SET immediately after writing (as a previous version of this
  // code did) can interrupt the LC12S before it finishes processing the
  // config frame, leaving it in a state where it never starts transmitting.
  uart_write_bytes(UART_NUM_2, reinterpret_cast<const char*>(cfg + 1), 18);

  ESP_LOGD(TAG, "LC12S config bytes sent: channel=0x%02X, network_id=0x%04X (SET still LOW)", channel, network_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// CRC-XMODEM (polynomial 0x1021, start value 0)
// Ported verbatim from original calc_crc / crc_xmodem_update.
// ─────────────────────────────────────────────────────────────────────────────

uint16_t IntexSpa::crc_xmodem_update_(uint16_t crc, uint8_t data) {
  crc ^= static_cast<uint16_t>(data) << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  return crc;
}

uint16_t IntexSpa::calc_crc_(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0;
  while (len--) crc = crc_xmodem_update_(crc, *data++);
  return crc;
}

}  // namespace intex_spa
}  // namespace esphome

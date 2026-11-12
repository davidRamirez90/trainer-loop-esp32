#include "board_v1.h"
#ifndef ESP_PLATFORM
#include "../host/simulator/platform_services.h"
#include <ctime>
#endif

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "trainer_loop/time_service.h"

#include <cstddef>
#endif

namespace trainer_loop::board_v1 {

#ifdef ESP_PLATFORM
namespace {

constexpr char kTag[] = "trainer_board";
constexpr std::uint32_t kI2cClockHz = 100000U;
constexpr int kI2cTimeoutMs = 100;
constexpr unsigned kI2cReadAttempts = 2U;
constexpr std::uint8_t kTca9554OutputRegister = 0x01U;
constexpr std::uint8_t kTca9554ConfigRegister = 0x03U;
constexpr std::uint8_t kTca9554V1Outputs = 0x87U;
constexpr std::uint8_t kTca9554V1OutputMask = 0x78U;
constexpr std::uint8_t kFt3168PowerModeRegister = 0xA5U;
constexpr std::uint8_t kFt3168DeviceIdRegister = 0xA0U;
constexpr std::uint8_t kFt3168DeviceId = 0x03U;
constexpr std::uint8_t kFt3168FingerCountRegister = 0x02U;
constexpr std::uint8_t kFt3168TouchDataFirstRegister = 0x02U;
constexpr std::size_t kFt3168TouchDataBytes = 11U;
constexpr std::uint16_t kPmuAddress = 0x34U;
constexpr std::uint16_t kRtcAddress = 0x51U;
constexpr std::uint16_t kTouchAddress = 0x38U;
constexpr std::uint8_t kPcf85063SecondsRegister = 0x04U;
constexpr std::size_t kPcf85063DateTimeBytes = 7U;
constexpr int kDisplayBufferLines = 40;

const std::uint8_t kSh8601Control1[] = {0x28U};
const std::uint8_t kSh8601Brightness[] = {0xD0U};
const std::uint8_t kSh8601ColorEnhance[] = {0x00U};

// This is the documented V1 SH8601 sequence expressed through Espressif's
// panel driver. The driver supplies MADCTL/COLMOD; these commands cover the
// V1 sleep/normal/display and brightness settings without importing the
// vendor's GPL Arduino implementation.
const sh8601_lcd_init_cmd_t kSh8601InitCommands[] = {
    {0x11, nullptr, 0U, 120U},  // sleep out
    {0x13, nullptr, 0U, 0U},    // normal display mode
    {0x20, nullptr, 0U, 0U},    // inversion off
    {0x53, kSh8601Control1, sizeof(kSh8601Control1), 0U},
    {0x51, kSh8601Brightness, sizeof(kSh8601Brightness), 0U},
    {0x58, kSh8601ColorEnhance, sizeof(kSh8601ColorEnhance), 0U},
    {0x29, nullptr, 0U, 10U},   // display on
};

esp_err_t write_register(i2c_master_dev_handle_t device, std::uint8_t reg,
                         std::uint8_t value) {
  const std::uint8_t bytes[] = {reg, value};
  return i2c_master_transmit(device, bytes, sizeof(bytes), kI2cTimeoutMs);
}

esp_err_t read_registers(i2c_master_dev_handle_t device, std::uint8_t reg,
                         std::uint8_t* values, std::size_t value_count) {
  esp_err_t error = ESP_FAIL;
  // The ESP-IDF synchronous master resets its FSM after an invalid
  // transaction; replay the register read once to recover transient state
  // errors without making input polling unbounded.
  for (unsigned attempt = 0U; attempt < kI2cReadAttempts; ++attempt) {
    error = i2c_master_transmit_receive(device, &reg, sizeof(reg), values,
                                        value_count, kI2cTimeoutMs);
    if (error == ESP_OK) return ESP_OK;
  }
  return error;
}

esp_err_t read_register(i2c_master_dev_handle_t device, std::uint8_t reg,
                        std::uint8_t* value) {
  return read_registers(device, reg, value, 1U);
}

bool decode_bcd(std::uint8_t raw, std::uint8_t mask, std::uint8_t* value) {
  if (value == nullptr) return false;
  raw = static_cast<std::uint8_t>(raw & mask);
  if ((raw & 0x0fU) > 9U || ((raw >> 4U) & 0x0fU) > 9U) return false;
  *value = static_cast<std::uint8_t>(((raw >> 4U) * 10U) + (raw & 0x0fU));
  return true;
}

esp_err_t add_i2c_device(i2c_master_bus_handle_t bus, std::uint16_t address,
                         i2c_master_dev_handle_t* device) {
  i2c_device_config_t config{};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = kI2cClockHz;
  return i2c_master_bus_add_device(bus, &config, device);
}

void remove_i2c_device(i2c_master_dev_handle_t* device) {
  if (device != nullptr && *device != nullptr) {
    (void)i2c_master_bus_rm_device(*device);
    *device = nullptr;
  }
}

}  // namespace

struct BoardV1::HardwareHandles {
  i2c_master_bus_handle_t i2c_bus = nullptr;
  i2c_master_dev_handle_t expander = nullptr;
  i2c_master_dev_handle_t rtc = nullptr;
  i2c_master_dev_handle_t touch = nullptr;
  esp_lcd_panel_io_handle_t panel_io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;
  bool spi_initialized = false;
};

void release_hardware(BoardV1::HardwareHandles** handles) {
  if (handles == nullptr || *handles == nullptr) return;
  BoardV1::HardwareHandles* hardware = *handles;
  if (hardware->panel != nullptr) {
    (void)esp_lcd_panel_del(hardware->panel);
    hardware->panel = nullptr;
  }
  if (hardware->panel_io != nullptr) {
    (void)esp_lcd_panel_io_del(hardware->panel_io);
    hardware->panel_io = nullptr;
  }
  if (hardware->spi_initialized) {
    (void)spi_bus_free(SPI2_HOST);
    hardware->spi_initialized = false;
  }
  remove_i2c_device(&hardware->touch);
  remove_i2c_device(&hardware->rtc);
  remove_i2c_device(&hardware->expander);
  if (hardware->i2c_bus != nullptr) {
    (void)i2c_del_master_bus(hardware->i2c_bus);
    hardware->i2c_bus = nullptr;
  }
  delete hardware;
  *handles = nullptr;
}

esp_err_t configure_input(gpio_num_t pin) {
  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << static_cast<unsigned int>(pin);
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  return gpio_config(&config);
}

esp_err_t initialize_i2c(BoardV1::HardwareHandles* hardware) {
  i2c_master_bus_config_t config{};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = static_cast<gpio_num_t>(PinMap::kI2cSda);
  config.scl_io_num = static_cast<gpio_num_t>(PinMap::kI2cScl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7U;
  config.flags.enable_internal_pullup = true;
  return i2c_new_master_bus(&config, &hardware->i2c_bus);
}

esp_err_t initialize_expander(BoardV1::HardwareHandles* hardware) {
  esp_err_t error = i2c_master_probe(hardware->i2c_bus, PinMap::kExpanderAddress,
                                     kI2cTimeoutMs);
  if (error != ESP_OK) return error;
  error = add_i2c_device(hardware->i2c_bus, PinMap::kExpanderAddress,
                         &hardware->expander);
  if (error != ESP_OK) return error;

  // P0/P1/P2 are the V1 LCD reset, display-power-enable, and touch-reset
  // nets. P7 is the SD chip-select and must remain high while the shared
  // storage bus is idle. This matches the manufacturer's V1 bring-up order.
  error = write_register(hardware->expander, kTca9554ConfigRegister,
                         kTca9554V1OutputMask);
  if (error == ESP_OK) {
    error = write_register(hardware->expander, kTca9554OutputRegister, 0x80U);
  }
  if (error == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(20U));
    error = write_register(hardware->expander, kTca9554OutputRegister,
                           kTca9554V1Outputs);
  }
  if (error == ESP_OK) {
    // Allow the FT3168 to complete its reset/startup sequence after the
    // expander releases its reset line.
    vTaskDelay(pdMS_TO_TICKS(150U));
  }
  return error;
}

esp_err_t initialize_touch(BoardV1::HardwareHandles* hardware) {
  esp_err_t error = i2c_master_probe(hardware->i2c_bus, kTouchAddress,
                                     kI2cTimeoutMs);
  if (error != ESP_OK) return error;
  error = add_i2c_device(hardware->i2c_bus, kTouchAddress, &hardware->touch);
  if (error != ESP_OK) return error;
  error = write_register(hardware->touch, kFt3168PowerModeRegister, 0x01U);
  if (error != ESP_OK) return error;
  // Allow the FT3168 to finish startup before its identification read.
  vTaskDelay(pdMS_TO_TICKS(150U));
  std::uint8_t device_id = 0U;
  error = read_register(hardware->touch, kFt3168DeviceIdRegister, &device_id);
  if (error != ESP_OK) return error;
  return device_id == kFt3168DeviceId ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t initialize_display(BoardV1::HardwareHandles* hardware) {
  spi_bus_config_t bus_config{};
  bus_config.data0_io_num = PinMap::kDisplayD0;
  bus_config.data1_io_num = PinMap::kDisplayD1;
  bus_config.sclk_io_num = PinMap::kDisplayClock;
  bus_config.data2_io_num = PinMap::kDisplayD2;
  bus_config.data3_io_num = PinMap::kDisplayD3;
  bus_config.max_transfer_sz = PinMap::kDisplayWidth * kDisplayBufferLines * 2;
  esp_err_t error = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (error != ESP_OK) return error;
  hardware->spi_initialized = true;

  esp_lcd_panel_io_spi_config_t io_config{};
  io_config.cs_gpio_num = PinMap::kDisplayCs;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 0;
  io_config.pclk_hz = 40U * 1000U * 1000U;
  io_config.trans_queue_depth = 10U;
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;
  error = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                                   &io_config, &hardware->panel_io);
  if (error != ESP_OK) return error;

  sh8601_vendor_config_t vendor_config{};
  vendor_config.init_cmds = kSh8601InitCommands;
  vendor_config.init_cmds_size =
      sizeof(kSh8601InitCommands) / sizeof(kSh8601InitCommands[0]);
  vendor_config.flags.use_qspi_interface = 1U;

  esp_lcd_panel_dev_config_t panel_config{};
  panel_config.reset_gpio_num = -1;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
  panel_config.bits_per_pixel = 16U;
  panel_config.vendor_config = &vendor_config;
  error = esp_lcd_new_panel_sh8601(hardware->panel_io, &panel_config,
                                   &hardware->panel);
  if (error != ESP_OK) return error;
  error = esp_lcd_panel_reset(hardware->panel);
  if (error == ESP_OK) error = esp_lcd_panel_init(hardware->panel);
  if (error == ESP_OK) error = esp_lcd_panel_set_gap(hardware->panel, 0, 0);
  if (error == ESP_OK) error = esp_lcd_panel_disp_on_off(hardware->panel, true);
  return error;
}

bool probe_optional(i2c_master_bus_handle_t bus, std::uint16_t address) {
  return i2c_master_probe(bus, address, kI2cTimeoutMs) == ESP_OK;
}
#endif

BoardV1::~BoardV1() {
#ifdef ESP_PLATFORM
  release_hardware(&handles_);
#endif
}

Status BoardV1::initialize() {
#ifdef ESP_PLATFORM
  if (handles_ != nullptr && status_.display_ready) return Status::ok();
  release_hardware(&handles_);
  status_ = BoardStatus{};

  handles_ = new HardwareHandles();
  esp_err_t error = configure_input(static_cast<gpio_num_t>(PinMap::kBoot));
  if (error == ESP_OK) {
    error = configure_input(static_cast<gpio_num_t>(PinMap::kTouchInterrupt));
  }
  if (error == ESP_OK) error = initialize_i2c(handles_);
  if (error == ESP_OK) error = initialize_expander(handles_);
  if (error != ESP_OK) {
    status_.reason = "V1 I2C/expander initialization failed";
    ESP_LOGE(kTag, "%s: %s", status_.reason, esp_err_to_name(error));
    release_hardware(&handles_);
    return Status::failure(ErrorCode::kTransport, status_.reason);
  }

  status_.pmu_ready = probe_optional(handles_->i2c_bus, kPmuAddress);
  status_.rtc_ready = probe_optional(handles_->i2c_bus, kRtcAddress);
  if (status_.rtc_ready && add_i2c_device(handles_->i2c_bus, kRtcAddress, &handles_->rtc) != ESP_OK) {
    status_.rtc_ready = false;
  }
  if (!status_.pmu_ready) ESP_LOGW(kTag, "AXP2101 probe at 0x%02x failed", kPmuAddress);
  if (!status_.rtc_ready) ESP_LOGW(kTag, "PCF85063 probe at 0x%02x failed", kRtcAddress);

  error = initialize_touch(handles_);
  if (error == ESP_OK) {
    status_.touch_ready = true;
  } else {
    ESP_LOGW(kTag, "FT3168 probe/init failed: %s", esp_err_to_name(error));
    remove_i2c_device(&handles_->touch);
  }

  error = initialize_display(handles_);
  if (error != ESP_OK) {
    status_.reason = "V1 SH8601 QSPI initialization failed";
    ESP_LOGE(kTag, "%s: %s", status_.reason, esp_err_to_name(error));
    release_hardware(&handles_);
    return Status::failure(ErrorCode::kTransport, status_.reason);
  }
  status_.display_ready = true;
  status_.reason = status_.touch_ready
                       ? "V1 buses and SH8601/FT3168 drivers initialized"
                       : "V1 display initialized; FT3168 is unavailable";
  ESP_LOGI(kTag, "%s (PMU=%s RTC=%s)", status_.reason,
           status_.pmu_ready ? "ready" : "absent",
           status_.rtc_ready ? "ready" : "absent");
  // This flag remains false until the display colors/orientation, touch
  // corners, power behavior, and storage-card path pass physical acceptance.
  status_.physical_v1_audited = false;
  return Status::ok();
#else
  status_.display_ready = true;
  status_.touch_ready = true;
  status_.rtc_ready = HostPlatformServices::instance().has_utc();
  status_.sd_present = true;
  status_.pmu_ready = true;
  status_.reason = "host simulator board services initialized";
  return Status::ok();
#endif
}

bool BoardV1::boot_pressed() const {
#ifdef ESP_PLATFORM
  return gpio_get_level(static_cast<gpio_num_t>(PinMap::kBoot)) == 0;
#else
  return HostPlatformServices::instance().boot_pressed();
#endif
}

Status BoardV1::read_touch(TouchPoint* point) const {
  if (point == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "touch point is null");
  *point = TouchPoint{};
#ifdef ESP_PLATFORM
  if (handles_ == nullptr || handles_->touch == nullptr || !status_.touch_ready) {
    return Status::failure(ErrorCode::kUnsupported, "FT3168 is not ready");
  }
  std::uint8_t data[kFt3168TouchDataBytes]{};
  const esp_err_t error = read_registers(handles_->touch,
                                         kFt3168TouchDataFirstRegister, data,
                                         sizeof(data));
  if (error != ESP_OK) {
    return Status::failure(ErrorCode::kTransport, "FT3168 read failed");
  }

  const std::uint8_t fingers = data[kFt3168FingerCountRegister - kFt3168TouchDataFirstRegister];
  point->fingers = fingers > 2U ? 2U : fingers;
  point->touched = point->fingers != 0U;
  if (!point->touched) return Status::ok();
  const std::size_t x_offset = 1U;
  const std::size_t y_offset = 3U;
  point->x = static_cast<std::uint16_t>(((data[x_offset] & 0x0FU) << 8U) | data[x_offset + 1U]);
  point->y = static_cast<std::uint16_t>(((data[y_offset] & 0x0FU) << 8U) | data[y_offset + 1U]);
  if (point->x >= PinMap::kDisplayWidth || point->y >= PinMap::kDisplayHeight) {
    *point = TouchPoint{};
    return Status::failure(ErrorCode::kBounds, "FT3168 coordinate is outside V1 panel");
  }
  return Status::ok();
#else
  return Status::ok();
#endif
}

Status BoardV1::read_rtc(RtcReading* reading) const {
  if (reading == nullptr) return Status::failure(ErrorCode::kInvalidArgument, "RTC reading is null");
  *reading = RtcReading{};
#ifdef ESP_PLATFORM
  if (handles_ == nullptr || handles_->i2c_bus == nullptr || !status_.rtc_ready) {
    return Status::failure(ErrorCode::kUnsupported, "PCF85063 is not ready");
  }
  std::uint8_t data[kPcf85063DateTimeBytes]{};
  const std::uint8_t first_register = kPcf85063SecondsRegister;
  const esp_err_t error = i2c_master_transmit_receive(
      handles_->rtc, &first_register, sizeof(first_register), data, sizeof(data),
      kI2cTimeoutMs);
  if (error != ESP_OK) return Status::failure(ErrorCode::kTransport, "PCF85063 read failed");

  reading->oscillator_stopped = (data[0] & 0x80U) != 0U;
  std::uint8_t month = 0U;
  std::uint8_t day = 0U;
  std::uint8_t hour = 0U;
  std::uint8_t minute = 0U;
  std::uint8_t second = 0U;
  std::uint8_t year = 0U;
  if (!decode_bcd(data[0], 0x7fU, &second) || !decode_bcd(data[1], 0x7fU, &minute) ||
      !decode_bcd(data[2], 0x3fU, &hour) || !decode_bcd(data[3], 0x3fU, &day) ||
      !decode_bcd(data[5], 0x1fU, &month) || !decode_bcd(data[6], 0xffU, &year)) {
    return Status::failure(ErrorCode::kMalformedInput, "PCF85063 BCD fields are invalid");
  }
  reading->year = static_cast<std::uint16_t>(2000U + year);
  reading->month = month;
  reading->day = day;
  reading->hour = hour;
  reading->minute = minute;
  reading->second = second;
  const CalendarDateTime calendar{static_cast<int>(reading->year), month, day, hour, minute, second};
  const auto utc = utc_ms_from_calendar(calendar);
  if (!utc) return Status::failure(utc.error->code, utc.error->message);
  reading->utc_ms = *utc.value;
  reading->valid = !reading->oscillator_stopped;
  if (!reading->valid) return Status::failure(ErrorCode::kTimeUntrusted, "PCF85063 oscillator-stop flag is set");
  return Status::ok();
#else
  const auto& services = HostPlatformServices::instance();
  if (!services.has_utc()) return Status::failure(ErrorCode::kTimeUntrusted, "simulated RTC has no configured UTC");
  const std::int64_t utc_ms = services.utc_ms();
  const std::time_t seconds = static_cast<std::time_t>(utc_ms / 1000LL);
  std::tm calendar{};
  if (::gmtime_r(&seconds, &calendar) == nullptr) return Status::failure(ErrorCode::kTimeUntrusted, "simulated RTC UTC is invalid");
  reading->valid = true;
  reading->utc_ms = utc_ms;
  reading->year = static_cast<std::uint16_t>(calendar.tm_year + 1900);
  reading->month = static_cast<std::uint8_t>(calendar.tm_mon + 1);
  reading->day = static_cast<std::uint8_t>(calendar.tm_mday);
  reading->hour = static_cast<std::uint8_t>(calendar.tm_hour);
  reading->minute = static_cast<std::uint8_t>(calendar.tm_min);
  reading->second = static_cast<std::uint8_t>(calendar.tm_sec);
  return Status::ok();
#endif
}

void* BoardV1::display_panel() const {
#ifdef ESP_PLATFORM
  return handles_ == nullptr ? nullptr : static_cast<void*>(handles_->panel);
#else
  return nullptr;
#endif
}

void* BoardV1::display_io() const {
#ifdef ESP_PLATFORM
  return handles_ == nullptr ? nullptr : static_cast<void*>(handles_->panel_io);
#else
  return nullptr;
#endif
}

}  // namespace trainer_loop::board_v1

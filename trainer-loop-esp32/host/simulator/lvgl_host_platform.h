#pragma once

// Small host implementation of the ESP-IDF display/timer/task boundary used
// by the production presenter. LVGL, its object tree, renderer and callbacks
// remain the real managed LVGL 9.2.2 implementation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

struct HostLvglPanel {
  static constexpr int kWidth = 368;
  static constexpr int kHeight = 448;
  std::array<std::uint16_t, static_cast<std::size_t>(kWidth * kHeight)> pixels{};
  struct HostLvglIo* io = nullptr;
};
struct HostLvglIo {
  bool (*flush_done)(HostLvglIo*, struct esp_lcd_panel_io_event_data_t*, void*) = nullptr;
  void* flush_user_data = nullptr;
};
inline HostLvglPanel g_host_lvgl_panel;
inline HostLvglIo g_host_lvgl_io;

using esp_lcd_panel_handle_t = HostLvglPanel*;
using esp_lcd_panel_io_handle_t = HostLvglIo*;
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
struct esp_lcd_panel_io_event_data_t {};
struct esp_lcd_panel_io_callbacks_t {
  bool (*on_color_trans_done)(esp_lcd_panel_io_handle_t,
                              esp_lcd_panel_io_event_data_t*, void*) = nullptr;
};
inline esp_err_t esp_lcd_panel_io_register_event_callbacks(
    esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t* callbacks, void* user_data) {
  if (io && callbacks) {
    io->flush_done = callbacks->on_color_trans_done;
    io->flush_user_data = user_data;
  }
  return ESP_OK;
}
inline esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                            int x1, int y1, int x2, int y2,
                                            const void* source) {
  if (!panel || !source || x1 < 0 || y1 < 0 || x2 > HostLvglPanel::kWidth ||
      y2 > HostLvglPanel::kHeight || x2 <= x1 || y2 <= y1) return -1;
  const auto* src = static_cast<const std::uint16_t*>(source);
  const int width = x2 - x1;
  for (int row = 0; row < y2 - y1; ++row) {
    std::memcpy(panel->pixels.data() + (y1 + row) * HostLvglPanel::kWidth + x1,
                src + row * width, static_cast<std::size_t>(width) * sizeof(*src));
  }
  if (panel->io && panel->io->flush_done) {
    (void)panel->io->flush_done(panel->io, nullptr, panel->io->flush_user_data);
  }
  return ESP_OK;
}
inline const char* esp_err_to_name(esp_err_t) { return "host display error"; }

constexpr int MALLOC_CAP_DMA = 1;
constexpr int MALLOC_CAP_INTERNAL = 2;
inline void* heap_caps_malloc(std::size_t size, int) { return std::malloc(size); }
inline void heap_caps_free(void* ptr) { std::free(ptr); }

struct HostSemaphore { std::mutex mutex; };
using SemaphoreHandle_t = HostSemaphore*;
using TickType_t = unsigned;
using UBaseType_t = unsigned;
using BaseType_t = int;
using TaskHandle_t = void*;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdPASS = 1;
constexpr TickType_t portMAX_DELAY = 0xffffffffU;
inline TickType_t pdMS_TO_TICKS(unsigned ms) { return ms; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new HostSemaphore; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t) {
  if (!semaphore) return 0;
  semaphore->mutex.lock();
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  if (!semaphore) return 0;
  semaphore->mutex.unlock();
  return pdTRUE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }
inline BaseType_t xTaskCreate(void (*)(void*), const char*, std::uint32_t,
                              void*, UBaseType_t, TaskHandle_t* output) {
  if (output) *output = nullptr;
  return pdPASS;
}
inline void vTaskDelay(TickType_t) {}
inline void vTaskDelete(TaskHandle_t) {}

using esp_timer_handle_t = void*;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
  void (*callback)(void*) = nullptr;
  void* arg = nullptr;
  int dispatch_method = ESP_TIMER_TASK;
  const char* name = nullptr;
  bool skip_unhandled_events = false;
};
inline esp_err_t esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* timer) {
  if (timer) *timer = reinterpret_cast<void*>(1);
  return ESP_OK;
}
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t, std::uint64_t) { return ESP_OK; }

#define ESP_LOGE(tag, format, ...) ((void)0)
#define ESP_LOGI(tag, format, ...) ((void)0)
#define ESP_LOGW(tag, format, ...) ((void)0)

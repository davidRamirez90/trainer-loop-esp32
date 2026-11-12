#include "lvgl_presenter.h"

#if __has_include("lvgl.h")
#include "lvgl.h"
#define TRAINER_LOOP_LVGL_AVAILABLE 1
#else
#define TRAINER_LOOP_LVGL_AVAILABLE 0
#endif

#if TRAINER_LOOP_LVGL_AVAILABLE && defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#elif TRAINER_LOOP_LVGL_AVAILABLE && defined(TRAINER_LOOP_HOST_SIM)
#include "lvgl_host_platform.h"
#include "lvgl_host.h"
#endif

#if TRAINER_LOOP_LVGL_AVAILABLE && (defined(ESP_PLATFORM) || defined(TRAINER_LOOP_HOST_SIM))
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>
#endif

namespace trainer_loop {

#if TRAINER_LOOP_LVGL_AVAILABLE && (defined(ESP_PLATFORM) || defined(TRAINER_LOOP_HOST_SIM))
namespace {

constexpr char kTag[] = "trainer_lvgl";
constexpr std::size_t kDrawBufferLines = 40U;
constexpr std::uint32_t kTickPeriodMs = 2U;
constexpr std::uint32_t kTaskMinDelayMs = 5U;
constexpr std::uint32_t kTaskMaxDelayMs = 50U;
constexpr std::uint32_t kTaskStackSize = 6144U;
constexpr UBaseType_t kTaskPriority = 2U;
constexpr std::size_t kPairingRows = 4U;
constexpr std::size_t kHistoryRows = 4U;
constexpr std::size_t kSettingsRows = 5U;
constexpr std::size_t kSurfaceCount = 10U;
constexpr std::size_t kProgressTracks = 2U;
constexpr lv_style_prop_t kNoTransitionProperties[] = {static_cast<lv_style_prop_t>(0)};
const lv_style_transition_dsc_t kNoTransition = {
    kNoTransitionProperties, nullptr, nullptr, 0U, 0U};

// Visual tokens are intentionally kept in the presenter because this device
// has one fixed 368x448 canvas. Keeping the vocabulary here makes the
// physical-pixel decisions explicit and prevents screen-specific hex values
// from accumulating again.
namespace theme {
constexpr int kInset = 16;
constexpr int kContentWidth = 336;
constexpr int kHeaderY = 12;
constexpr int kBottomPrimaryY = 344;
constexpr int kBottomSecondaryY = 400;
constexpr int kButtonHeight = 48;
constexpr int kRadius = 10;
constexpr int kPausedDiscardY = 276;
constexpr int kPausedResumeY = 332;
constexpr int kPausedEndSaveY = 396;

lv_color_t ink() { return lv_color_hex(0xFFFFFF); }
lv_color_t muted() { return lv_color_hex(0xA8B0BE); }
lv_color_t subtle() { return lv_color_hex(0x697484); }
lv_color_t canvas() { return lv_color_hex(0x000000); }
lv_color_t surface() { return lv_color_hex(0x0B1116); }
lv_color_t surface_alt() { return lv_color_hex(0x141B24); }
lv_color_t accent() { return lv_color_hex(0x45D6A5); }
lv_color_t accent_pressed() { return lv_color_hex(0x8AF5CF); }
lv_color_t accent_deep() { return lv_color_hex(0x183C32); }
lv_color_t warning() { return lv_color_hex(0xFFB547); }
lv_color_t danger() { return lv_color_hex(0xFF6B6B); }
lv_color_t disabled() { return lv_color_hex(0x252A30); }
}  // namespace theme

struct UiObjects {
  lv_obj_t* surfaces[kSurfaceCount]{};
  lv_obj_t* progress_tracks[kProgressTracks]{};
  lv_obj_t* progress_fills[kProgressTracks]{};
  lv_obj_t* settings_rows[kSettingsRows]{};
  lv_obj_t* settings_labels[kSettingsRows]{};
  lv_obj_t* settings_values[kSettingsRows]{};
  lv_obj_t* completion_mark = nullptr;
  lv_obj_t* footer = nullptr;
  lv_obj_t* extra[4]{};
  lv_obj_t* extra_labels[4]{};
  lv_obj_t* workout_card = nullptr;
  lv_obj_t* status_marks[3]{};
  lv_obj_t* status = nullptr;
  lv_obj_t* workout = nullptr;
  lv_obj_t* target_caption = nullptr;
  lv_obj_t* target_value = nullptr;
  lv_obj_t* actual_caption = nullptr;
  lv_obj_t* actual_value = nullptr;
  lv_obj_t* sensors = nullptr;
  lv_obj_t* timing = nullptr;
  lv_obj_t* terrain = nullptr;
  lv_obj_t* safety = nullptr;
  lv_obj_t* profile_bars[kUiProfileBars]{};
  lv_obj_t* setup_title = nullptr;
  lv_obj_t* setup_instruction = nullptr;
  lv_obj_t* setup_ssid = nullptr;
  lv_obj_t* setup_password = nullptr;
  lv_obj_t* setup_url = nullptr;
  lv_obj_t* setup_note = nullptr;
  lv_obj_t* action = nullptr;
  lv_obj_t* action_label = nullptr;
  lv_obj_t* history_button = nullptr;
  lv_obj_t* history_button_label = nullptr;
  lv_obj_t* candidate_buttons[kPairingRows]{};
  lv_obj_t* candidate_labels[kPairingRows]{};
  lv_obj_t* history_rows[kHistoryRows]{};
  lv_obj_t* history_labels[kHistoryRows]{};
};

}  // namespace

struct LvglPresenter::Runtime {
  struct PairingButtonContext {
    LvglPresenter::Runtime* runtime = nullptr;
    std::size_t index = 0U;
  };
  struct HistoryButtonContext {
    LvglPresenter::Runtime* runtime = nullptr;
    std::size_t index = 0U;
  };
  struct NavigationButtonContext {
    LvglPresenter::Runtime* runtime = nullptr;
    UiCommandType command = UiCommandType::kBack;
  };

  esp_lcd_panel_handle_t panel = nullptr;
  esp_lcd_panel_io_handle_t panel_io = nullptr;
  board_v1::BoardV1* board = nullptr;
  lv_display_t* display = nullptr;
  lv_indev_t* touch = nullptr;
  esp_timer_handle_t tick_timer = nullptr;
  SemaphoreHandle_t lock = nullptr;
  TaskHandle_t task = nullptr;
  void* buffer_1 = nullptr;
  void* buffer_2 = nullptr;
  std::size_t buffer_bytes = 0U;
  UiObjects objects;
  UiScreen screen = UiScreen::kHome;
  UiCommandType action_command = UiCommandType::kBack;
  bool stop = false;
  UiCommandQueue* commands = nullptr;
#if defined(TRAINER_LOOP_HOST_SIM)
  int host_pointer_x = 0;
  int host_pointer_y = 0;
  bool host_pointer_pressed = false;
#endif
  PairingButtonContext candidate_contexts[kPairingRows]{};
  HistoryButtonContext history_row_contexts[kHistoryRows]{};
  NavigationButtonContext history_context;
  NavigationButtonContext workout_card_context;
  NavigationButtonContext extra_contexts[4]{};
};

#if defined(TRAINER_LOOP_HOST_SIM)
LvglPresenter::~LvglPresenter() {
  if (runtime_ == nullptr) return;
  runtime_->stop = true;
  if (runtime_->touch) lv_indev_delete(runtime_->touch);
  if (runtime_->display) lv_display_delete(runtime_->display);
  if (runtime_->buffer_1) heap_caps_free(runtime_->buffer_1);
  if (runtime_->buffer_2) heap_caps_free(runtime_->buffer_2);
  if (runtime_->lock) vSemaphoreDelete(runtime_->lock);
  delete runtime_;
  runtime_ = nullptr;
  initialized_ = false;
  lv_deinit();
}
#endif

namespace {

bool notify_flush_ready(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                        void* user_ctx) {
  auto* display = static_cast<lv_display_t*>(user_ctx);
  if (display != nullptr) lv_display_flush_ready(display);
  return false;
}

void flush_callback(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels) {
  auto* runtime = static_cast<LvglPresenter::Runtime*>(lv_display_get_user_data(display));
  if (runtime == nullptr || runtime->panel == nullptr || area == nullptr || pixels == nullptr) {
    lv_display_flush_ready(display);
    return;
  }
  const int width = area->x2 - area->x1 + 1;
  const int height = area->y2 - area->y1 + 1;
  if (width <= 0 || height <= 0) {
    lv_display_flush_ready(display);
    return;
  }
  // The SH8601 panel is configured for big-endian RGB565 on the QSPI bus.
#if defined(ESP_PLATFORM)
  lv_draw_sw_rgb565_swap(pixels, static_cast<std::uint32_t>(width * height));
#endif
  const esp_err_t error = esp_lcd_panel_draw_bitmap(
      runtime->panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
  if (error != ESP_OK) {
    ESP_LOGE(kTag, "display flush failed: %s", esp_err_to_name(error));
    // A failed enqueue has no completion callback; unblock LVGL and leave the
    // fault visible through the next application snapshot.
    lv_display_flush_ready(display);
  }
}

void touch_callback(lv_indev_t* indev, lv_indev_data_t* data) {
  if (data == nullptr) return;
  data->state = LV_INDEV_STATE_RELEASED;
  auto* runtime = static_cast<LvglPresenter::Runtime*>(lv_indev_get_user_data(indev));
  if (runtime == nullptr) return;
#if defined(TRAINER_LOOP_HOST_SIM)
  data->state = runtime->host_pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = static_cast<lv_coord_t>(runtime->host_pointer_x);
  data->point.y = static_cast<lv_coord_t>(runtime->host_pointer_y);
#else
  if (runtime->board == nullptr) return;
  board_v1::TouchPoint point;
  const Status status = runtime->board->read_touch(&point);
  if (status && point.touched) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = static_cast<lv_coord_t>(point.x);
    data->point.y = static_cast<lv_coord_t>(point.y);
  }
#endif
}

void action_callback(lv_event_t* event) {
  if (event == nullptr) return;
  auto* runtime = static_cast<LvglPresenter::Runtime*>(lv_event_get_user_data(event));
  if (runtime == nullptr || runtime->commands == nullptr) return;
  (void)runtime->commands->push(UiCommand{runtime->action_command});
}

void candidate_callback(lv_event_t* event) {
  if (event == nullptr) return;
  auto* context = static_cast<LvglPresenter::Runtime::PairingButtonContext*>(
      lv_event_get_user_data(event));
  if (context == nullptr || context->runtime == nullptr || context->runtime->commands == nullptr) return;
  (void)context->runtime->commands->push(UiCommand{UiCommandType::kSelectDevice, context->index});
}

void history_callback(lv_event_t* event) {
  if (event == nullptr) return;
  auto* context = static_cast<LvglPresenter::Runtime::HistoryButtonContext*>(
      lv_event_get_user_data(event));
  if (context == nullptr || context->runtime == nullptr || context->runtime->commands == nullptr) return;
  (void)context->runtime->commands->push(UiCommand{UiCommandType::kOpenHistoryEntry, context->index});
}

void navigation_callback(lv_event_t* event) {
  if (event == nullptr) return;
  auto* context = static_cast<LvglPresenter::Runtime::NavigationButtonContext*>(
      lv_event_get_user_data(event));
  if (context == nullptr || context->runtime == nullptr || context->runtime->commands == nullptr) return;
  (void)context->runtime->commands->push(UiCommand{context->command});
}

void tick_callback(void*) { lv_tick_inc(kTickPeriodMs); }

void lvgl_task(void* argument) {
  auto* runtime = static_cast<LvglPresenter::Runtime*>(argument);
  if (runtime == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  while (!runtime->stop) {
    std::uint32_t delay_ms = kTaskMaxDelayMs;
    if (xSemaphoreTake(runtime->lock, portMAX_DELAY) == pdTRUE) {
      delay_ms = lv_timer_handler();
      xSemaphoreGive(runtime->lock);
    }
    delay_ms = std::max(kTaskMinDelayMs, std::min(kTaskMaxDelayMs, delay_ms));
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
  runtime->task = nullptr;
  vTaskDelete(nullptr);
}

lv_obj_t* make_label(lv_obj_t* parent, int x, int y, int width, int height,
                    const lv_font_t* font, lv_color_t color,
                    lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) return nullptr;
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
  lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  return label;
}

void place_label(lv_obj_t* label, int x, int y, int width, int height,
                 const lv_font_t* font, lv_color_t color, const char* text,
                 lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  if (label == nullptr) return;
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
  lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_label_set_text(label, text == nullptr ? "" : text);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}

void style_surface(lv_obj_t* surface, lv_color_t background, lv_color_t border,
                   lv_coord_t radius = theme::kRadius) {
  if (surface == nullptr) return;
  lv_obj_set_style_bg_color(surface, background, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(surface, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(surface, border, LV_PART_MAIN);
  lv_obj_set_style_radius(surface, radius, LV_PART_MAIN);
  lv_obj_set_style_pad_all(surface, 0, LV_PART_MAIN);
}

void style_progress_track(lv_obj_t* track, lv_color_t background) {
  style_surface(track, background, background, 5);
}

void style_progress_fill(lv_obj_t* fill, lv_color_t color) {
  style_surface(fill, color, color, 5);
}

void style_button(lv_obj_t* button, lv_color_t base, lv_color_t pressed,
                  lv_color_t disabled, lv_coord_t radius = 12) {
  const auto selector = [](lv_state_t state) {
    return static_cast<lv_style_selector_t>(static_cast<std::uint32_t>(LV_PART_MAIN) |
                                            static_cast<std::uint32_t>(state));
  };
  lv_obj_set_style_bg_color(button, base, selector(LV_STATE_DEFAULT));
  lv_obj_set_style_bg_color(button, pressed, selector(LV_STATE_PRESSED));
  lv_obj_set_style_bg_color(button, pressed, selector(LV_STATE_CHECKED));
  lv_obj_set_style_bg_color(button, disabled, selector(LV_STATE_DISABLED));
  lv_obj_set_style_border_width(button, 1, selector(LV_STATE_DEFAULT));
  lv_obj_set_style_border_color(button, theme::accent_deep(), selector(LV_STATE_DEFAULT));
  lv_obj_set_style_border_color(button, theme::accent_pressed(), selector(LV_STATE_PRESSED));
  lv_obj_set_style_border_width(button, 0, selector(LV_STATE_DISABLED));
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(button, radius, LV_PART_MAIN);
  lv_obj_set_style_transition(button, &kNoTransition, LV_PART_MAIN);
}

std::string friendly_upload_state(const std::string& status) {
  if (status == "UPLOADED") return "Uploaded";
  if (status == "PENDING" || status == "FIT_PENDING") return "Upload pending";
  if (status == "UPLOADING") return "Uploading";
  if (status == "RETRY_WAIT") return "Upload will retry";
  if (status == "AUTH_BLOCKED") return "Upload blocked - check credentials";
  if (status == "PERMANENT_ERROR") return "Upload failed - retry from Home";
  return status.empty() ? "Saved locally" : status;
}

std::string ascii_ui_text(std::string value) {
  const auto replace_all = [&](const char* from, const char* to) {
    const std::string needle = from;
    const std::string replacement = to;
    std::size_t position = 0U;
    while ((position = value.find(needle, position)) != std::string::npos) {
      value.replace(position, needle.size(), replacement);
      position += replacement.size();
    }
  };
  replace_all("—", " - ");
  replace_all("•", " | ");
  replace_all("·", " | ");
  replace_all("…", "...");
  replace_all("²", "2");
  return value;
}

std::string compact_workout_name(const std::string& name) {
  const std::size_t parenthesis = name.find(" (");
  const std::string compact = parenthesis == std::string::npos ? name : name.substr(0U, parenthesis);
  if (compact.size() <= 20U) return compact;
  return compact.substr(0U, 17U) + "...";
}

bool create_ui(LvglPresenter::Runtime* runtime) {
  lv_obj_t* root = lv_screen_active();
  if (root == nullptr) return false;
  lv_obj_set_style_bg_color(root, theme::canvas(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

  const lv_color_t white = theme::ink();
  const lv_color_t muted = theme::muted();
  const lv_color_t accent = theme::accent();
  const lv_color_t warning = theme::warning();

  // Labels are retained and repositioned per state. This keeps the runtime
  // allocation small while allowing each screen to have an intentional
  // composition instead of inheriting the active-ride dashboard geometry.
  runtime->objects.status = make_label(root, theme::kInset, theme::kHeaderY,
                                       theme::kContentWidth, 20, &lv_font_montserrat_14, muted);
  runtime->objects.workout = make_label(root, theme::kInset, 34, theme::kContentWidth,
                                        30, &lv_font_montserrat_24, white);
  runtime->objects.target_caption = make_label(root, theme::kInset, 78, 160, 18,
                                               &lv_font_montserrat_14, muted);
  runtime->objects.target_value = make_label(root, theme::kInset, 94, 160, 52,
                                             &lv_font_montserrat_48, white);
  runtime->objects.actual_caption = make_label(root, 192, 78, 160, 18,
                                               &lv_font_montserrat_14, muted);
  runtime->objects.actual_value = make_label(root, 192, 94, 160, 52,
                                             &lv_font_montserrat_48, accent);
  runtime->objects.sensors = make_label(root, theme::kInset, 158, theme::kContentWidth,
                                        36, &lv_font_montserrat_24, white);
  runtime->objects.timing = make_label(root, theme::kInset, 202, theme::kContentWidth,
                                       38, &lv_font_montserrat_24, white);
  runtime->objects.terrain = make_label(root, theme::kInset, 250, theme::kContentWidth,
                                        60, &lv_font_montserrat_24, white);
  runtime->objects.safety = make_label(root, theme::kInset, 316, theme::kContentWidth,
                                       32, &lv_font_montserrat_14, warning);

  for (std::size_t index = 0U; index < kSurfaceCount; ++index) {
    runtime->objects.surfaces[index] = lv_obj_create(root);
    if (runtime->objects.surfaces[index] == nullptr) return false;
    style_surface(runtime->objects.surfaces[index], theme::surface(), theme::surface());
    lv_obj_add_flag(runtime->objects.surfaces[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (std::size_t index = 0U; index < kProgressTracks; ++index) {
    runtime->objects.progress_tracks[index] = lv_obj_create(root);
    runtime->objects.progress_fills[index] = lv_obj_create(root);
    if (runtime->objects.progress_tracks[index] == nullptr ||
        runtime->objects.progress_fills[index] == nullptr) return false;
    style_progress_track(runtime->objects.progress_tracks[index], theme::surface_alt());
    style_progress_fill(runtime->objects.progress_fills[index], accent);
    lv_obj_add_flag(runtime->objects.progress_tracks[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(runtime->objects.progress_fills[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (std::size_t index = 0U; index < kUiProfileBars; ++index) {
    runtime->objects.profile_bars[index] = lv_obj_create(root);
    if (runtime->objects.profile_bars[index] == nullptr) return false;
    style_surface(runtime->objects.profile_bars[index], accent, accent, 3);
    lv_obj_add_flag(runtime->objects.profile_bars[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (std::size_t index = 0U; index < 3U; ++index) {
    runtime->objects.status_marks[index] = lv_obj_create(root);
    if (runtime->objects.status_marks[index] == nullptr) return false;
    style_surface(runtime->objects.status_marks[index], accent, accent, 4);
    lv_obj_add_flag(runtime->objects.status_marks[index], LV_OBJ_FLAG_HIDDEN);
  }
  runtime->objects.setup_title = make_label(root, theme::kInset, 18, theme::kContentWidth,
                                            30, &lv_font_montserrat_24, white);
  runtime->objects.setup_instruction = make_label(root, theme::kInset, 52, theme::kContentWidth,
                                                  22, &lv_font_montserrat_14, muted);
  runtime->objects.setup_ssid = make_label(root, 28, 84, 312, 32, &lv_font_montserrat_24, white);
  runtime->objects.setup_password = make_label(root, 28, 124, 312, 32, &lv_font_montserrat_24, accent);
  runtime->objects.setup_url = make_label(root, theme::kInset, 188, theme::kContentWidth,
                                           32, &lv_font_montserrat_24, white);
  runtime->objects.setup_note = make_label(root, theme::kInset, 228, theme::kContentWidth,
                                            58, &lv_font_montserrat_14, muted);
  runtime->objects.footer = make_label(root, theme::kInset, 430, theme::kContentWidth,
                                       14, &lv_font_montserrat_14, theme::subtle());
  runtime->objects.completion_mark = make_label(root, 144, 46, 80, 52,
                                                &lv_font_montserrat_48, accent);

  for (std::size_t index = 0U; index < kSettingsRows; ++index) {
    runtime->objects.settings_rows[index] = lv_obj_create(root);
    runtime->objects.settings_labels[index] = make_label(root, 28, 0, 180, 22,
                                                          &lv_font_montserrat_14, white);
    runtime->objects.settings_values[index] = make_label(root, 212, 0, 112, 22,
                                                          &lv_font_montserrat_14, white,
                                                          LV_TEXT_ALIGN_RIGHT);
    if (runtime->objects.settings_rows[index] == nullptr ||
        runtime->objects.settings_labels[index] == nullptr ||
        runtime->objects.settings_values[index] == nullptr) return false;
    style_surface(runtime->objects.settings_rows[index], theme::surface(), theme::surface());
    lv_obj_add_flag(runtime->objects.settings_rows[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(runtime->objects.settings_labels[index], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(runtime->objects.settings_values[index], LV_OBJ_FLAG_HIDDEN);
  }
  if (runtime->objects.status == nullptr || runtime->objects.workout == nullptr ||
      runtime->objects.target_caption == nullptr || runtime->objects.target_value == nullptr ||
      runtime->objects.actual_caption == nullptr || runtime->objects.actual_value == nullptr ||
      runtime->objects.sensors == nullptr || runtime->objects.timing == nullptr ||
      runtime->objects.terrain == nullptr || runtime->objects.safety == nullptr ||
      runtime->objects.setup_title == nullptr || runtime->objects.setup_instruction == nullptr ||
      runtime->objects.setup_ssid == nullptr || runtime->objects.setup_password == nullptr ||
      runtime->objects.setup_url == nullptr || runtime->objects.setup_note == nullptr ||
      runtime->objects.footer == nullptr || runtime->objects.completion_mark == nullptr) {
    return false;
  }
  lv_obj_add_flag(runtime->objects.setup_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.setup_instruction, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.setup_ssid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.setup_password, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.setup_url, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.setup_note, LV_OBJ_FLAG_HIDDEN);

  for (std::size_t i = 0; i < 4; ++i) {
    auto* button = lv_button_create(root);
    runtime->objects.extra[i] = button;
    if (!button) return false;
    lv_obj_set_pos(button, theme::kInset + static_cast<int>(i % 2) * 172,
                   294 + static_cast<int>(i / 2) * 50);
    lv_obj_set_size(button, 164, 44);
    style_button(button, theme::accent_deep(), accent, theme::disabled(), theme::kRadius);
    runtime->extra_contexts[i].runtime = runtime;
    lv_obj_add_event_cb(button, navigation_callback, LV_EVENT_CLICKED, &runtime->extra_contexts[i]);
    runtime->objects.extra_labels[i] = lv_label_create(button);
    if (!runtime->objects.extra_labels[i]) return false;
    lv_obj_set_style_text_font(runtime->objects.extra_labels[i], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(runtime->objects.extra_labels[i], white, LV_PART_MAIN);
    lv_obj_center(runtime->objects.extra_labels[i]);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  }

  runtime->objects.workout_card = lv_button_create(root);
  if (runtime->objects.workout_card == nullptr) return false;
  style_button(runtime->objects.workout_card, theme::surface(), theme::surface_alt(),
               theme::disabled(), theme::kRadius);
  lv_obj_add_flag(runtime->objects.workout_card, LV_OBJ_FLAG_HIDDEN);
  runtime->workout_card_context.runtime = runtime;
  runtime->workout_card_context.command = UiCommandType::kOpenWorkoutPreview;
  lv_obj_add_event_cb(runtime->objects.workout_card, navigation_callback, LV_EVENT_CLICKED,
                      &runtime->workout_card_context);

  runtime->objects.action = lv_button_create(root);
  if (runtime->objects.action == nullptr) return false;
  lv_obj_set_pos(runtime->objects.action, theme::kInset, theme::kBottomPrimaryY);
  lv_obj_set_size(runtime->objects.action, theme::kContentWidth, theme::kButtonHeight);
  style_button(runtime->objects.action, accent, theme::accent_pressed(), theme::disabled(), theme::kRadius);
  lv_obj_add_event_cb(runtime->objects.action, action_callback, LV_EVENT_CLICKED, runtime);
  runtime->objects.action_label = lv_label_create(runtime->objects.action);
  if (runtime->objects.action_label == nullptr) return false;
  lv_obj_set_style_text_font(runtime->objects.action_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(runtime->objects.action_label, theme::canvas(), LV_PART_MAIN);
  lv_obj_center(runtime->objects.action_label);

  runtime->objects.history_button = lv_button_create(root);
  if (runtime->objects.history_button == nullptr) return false;
  lv_obj_set_pos(runtime->objects.history_button, theme::kInset, theme::kBottomSecondaryY);
  lv_obj_set_size(runtime->objects.history_button, theme::kContentWidth, 40);
  style_button(runtime->objects.history_button, theme::surface_alt(), theme::accent(),
               theme::disabled(), theme::kRadius);
  lv_obj_add_flag(runtime->objects.history_button, LV_OBJ_FLAG_HIDDEN);
  runtime->history_context.runtime = runtime;
  runtime->history_context.command = UiCommandType::kOpenHistory;
  lv_obj_add_event_cb(runtime->objects.history_button, navigation_callback, LV_EVENT_CLICKED,
                      &runtime->history_context);
  runtime->objects.history_button_label = lv_label_create(runtime->objects.history_button);
  if (runtime->objects.history_button_label == nullptr) return false;
  lv_obj_set_style_text_font(runtime->objects.history_button_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(runtime->objects.history_button_label, white, LV_PART_MAIN);
  lv_obj_center(runtime->objects.history_button_label);

  for (std::size_t index = 0U; index < kPairingRows; ++index) {
    runtime->objects.candidate_buttons[index] = lv_button_create(root);
    if (runtime->objects.candidate_buttons[index] == nullptr) return false;
    lv_obj_set_pos(runtime->objects.candidate_buttons[index], theme::kInset,
                   94 + static_cast<int>(index) * 56);
    lv_obj_set_size(runtime->objects.candidate_buttons[index], theme::kContentWidth, 48);
    style_button(runtime->objects.candidate_buttons[index], theme::surface(), accent,
                 theme::disabled(), theme::kRadius);
    lv_obj_add_flag(runtime->objects.candidate_buttons[index], LV_OBJ_FLAG_HIDDEN);
    runtime->candidate_contexts[index].runtime = runtime;
    runtime->candidate_contexts[index].index = index;
    lv_obj_add_event_cb(runtime->objects.candidate_buttons[index], candidate_callback,
                        LV_EVENT_CLICKED, &runtime->candidate_contexts[index]);
    runtime->objects.candidate_labels[index] = lv_label_create(runtime->objects.candidate_buttons[index]);
    if (runtime->objects.candidate_labels[index] == nullptr) return false;
    lv_obj_set_style_text_font(runtime->objects.candidate_labels[index], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(runtime->objects.candidate_labels[index], white, LV_PART_MAIN);
    lv_obj_set_height(runtime->objects.candidate_labels[index], 42);
    lv_label_set_long_mode(runtime->objects.candidate_labels[index], LV_LABEL_LONG_CLIP);
    lv_obj_set_width(runtime->objects.candidate_labels[index], 320);
    lv_obj_set_style_pad_left(runtime->objects.candidate_labels[index], 6, LV_PART_MAIN);
    lv_obj_center(runtime->objects.candidate_labels[index]);
  }

  for (std::size_t index = 0U; index < kHistoryRows; ++index) {
    runtime->objects.history_rows[index] = lv_button_create(root);
    if (runtime->objects.history_rows[index] == nullptr) return false;
    lv_obj_set_pos(runtime->objects.history_rows[index], theme::kInset,
                   88 + static_cast<int>(index) * 60);
    lv_obj_set_size(runtime->objects.history_rows[index], theme::kContentWidth, 52);
    style_button(runtime->objects.history_rows[index], theme::surface(), accent,
                 theme::disabled(), 6);
    lv_obj_add_flag(runtime->objects.history_rows[index], LV_OBJ_FLAG_HIDDEN);
    runtime->history_row_contexts[index].runtime = runtime;
    runtime->history_row_contexts[index].index = index;
    lv_obj_add_event_cb(runtime->objects.history_rows[index], history_callback, LV_EVENT_CLICKED,
                        &runtime->history_row_contexts[index]);
    runtime->objects.history_labels[index] = lv_label_create(runtime->objects.history_rows[index]);
    if (runtime->objects.history_labels[index] == nullptr) return false;
    lv_obj_set_style_text_font(runtime->objects.history_labels[index], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(runtime->objects.history_labels[index], white, LV_PART_MAIN);
    lv_label_set_long_mode(runtime->objects.history_labels[index], LV_LABEL_LONG_CLIP);
    lv_obj_set_width(runtime->objects.history_labels[index], 320);
    lv_obj_center(runtime->objects.history_labels[index]);
  }

  lv_label_set_text(runtime->objects.status, "Starting TrainerLoop  |  loading sensors");
  lv_label_set_text(runtime->objects.workout, "Please wait while the device becomes ready");
  lv_label_set_text(runtime->objects.target_caption, "TARGET");
  lv_label_set_text(runtime->objects.target_value, "-- W");
  lv_label_set_text(runtime->objects.actual_caption, "ACTUAL");
  lv_label_set_text(runtime->objects.actual_value, "-- W");
  lv_label_set_text(runtime->objects.sensors, "HR --     CAD --");
  lv_label_set_text(runtime->objects.timing, "00:00 active     00:00 elapsed");
  lv_label_set_text(runtime->objects.terrain, "--.- km/h     --.-% grade\n--.- km        +-- m virtual");
  lv_label_set_text(runtime->objects.safety, "Starting connections and local storage...");
  lv_label_set_text(runtime->objects.action_label, "BACK");
  lv_label_set_text(runtime->objects.history_button_label, "HISTORY");
  lv_obj_add_flag(runtime->objects.action, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.completion_mark, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime->objects.footer, LV_OBJ_FLAG_HIDDEN);
  return true;
}

void format_duration(char* output, std::size_t length, std::uint32_t milliseconds) {
  const std::uint32_t total_seconds = milliseconds / 1000U;
  std::snprintf(output, length, "%02lu:%02lu",
                static_cast<unsigned long>(total_seconds / 60U),
                static_cast<unsigned long>(total_seconds % 60U));
}

const char* optional_integer(const std::optional<int>& value, char* output, std::size_t length) {
  if (!value.has_value()) return "--";
  std::snprintf(output, length, "%d", *value);
  return output;
}

const char* optional_decimal(const std::optional<double>& value, char* output, std::size_t length) {
  if (!value.has_value()) return "--";
  std::snprintf(output, length, "%.0f", *value);
  return output;
}

}  // namespace

Status LvglPresenter::initialize(void* display_panel, void* display_io,
                                 board_v1::BoardV1* board) {
  if (initialized_) return Status::ok();
#if defined(TRAINER_LOOP_HOST_SIM)
  if (display_panel == nullptr || display_io == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "LVGL host display surface is missing");
  }
#else
  if (display_panel == nullptr || display_io == nullptr || board == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "LVGL requires an initialized V1 display");
  }
#endif

  auto* runtime = new Runtime();
  runtime->panel = static_cast<esp_lcd_panel_handle_t>(display_panel);
  runtime->panel_io = static_cast<esp_lcd_panel_io_handle_t>(display_io);
#if defined(TRAINER_LOOP_HOST_SIM)
  runtime->panel->io = runtime->panel_io;
#endif
  runtime->board = board;
  runtime->commands = &commands_;
  runtime->lock = xSemaphoreCreateMutex();
  if (runtime->lock == nullptr) {
    delete runtime;
    return Status::failure(ErrorCode::kStorage, "LVGL mutex allocation failed");
  }

  lv_init();
  runtime->display = lv_display_create(board_v1::PinMap::kDisplayWidth,
                                       board_v1::PinMap::kDisplayHeight);
  runtime->buffer_bytes = board_v1::PinMap::kDisplayWidth * kDrawBufferLines * sizeof(lv_color16_t);
  runtime->buffer_1 = heap_caps_malloc(runtime->buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  runtime->buffer_2 = heap_caps_malloc(runtime->buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (runtime->display == nullptr || runtime->buffer_1 == nullptr || runtime->buffer_2 == nullptr) {
    if (runtime->buffer_1 != nullptr) heap_caps_free(runtime->buffer_1);
    if (runtime->buffer_2 != nullptr) heap_caps_free(runtime->buffer_2);
    vSemaphoreDelete(runtime->lock);
    delete runtime;
    return Status::failure(ErrorCode::kStorage, "LVGL display buffer allocation failed");
  }
  lv_display_set_buffers(runtime->display, runtime->buffer_1, runtime->buffer_2,
                         runtime->buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_color_format(runtime->display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_user_data(runtime->display, runtime);
  lv_display_set_flush_cb(runtime->display, flush_callback);

  const esp_lcd_panel_io_callbacks_t callbacks = {
      .on_color_trans_done = notify_flush_ready,
  };
  esp_err_t error = esp_lcd_panel_io_register_event_callbacks(runtime->panel_io, &callbacks,
                                                               runtime->display);
  if (error != ESP_OK || !create_ui(runtime)) {
    ESP_LOGE(kTag, "LVGL display setup failed: %s",
             error == ESP_OK ? "UI allocation" : esp_err_to_name(error));
    return Status::failure(ErrorCode::kTransport, "LVGL display setup failed");
  }

  runtime->touch = lv_indev_create();
  if (runtime->touch == nullptr) {
    return Status::failure(ErrorCode::kStorage, "LVGL touch input allocation failed");
  }
  lv_indev_set_type(runtime->touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(runtime->touch, runtime->display);
  lv_indev_set_user_data(runtime->touch, runtime);
  lv_indev_set_read_cb(runtime->touch, touch_callback);

  const esp_timer_create_args_t tick_args = {
      .callback = tick_callback,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "lvgl_tick",
      .skip_unhandled_events = false,
  };
  error = esp_timer_create(&tick_args, &runtime->tick_timer);
  if (error == ESP_OK) error = esp_timer_start_periodic(runtime->tick_timer, kTickPeriodMs * 1000U);
  if (error != ESP_OK) return Status::failure(ErrorCode::kTransport, "LVGL tick timer setup failed");

  if (xTaskCreate(lvgl_task, "trainer_lvgl", kTaskStackSize, runtime, kTaskPriority,
                  &runtime->task) != pdPASS) {
    return Status::failure(ErrorCode::kStorage, "LVGL task allocation failed");
  }
  runtime_ = runtime;
  initialized_ = true;
  ESP_LOGI(kTag, "V1 LVGL UI ready (%ux%u, %u-line buffers)",
           board_v1::PinMap::kDisplayWidth, board_v1::PinMap::kDisplayHeight,
           static_cast<unsigned>(kDrawBufferLines));
  return Status::ok();
}

#if defined(TRAINER_LOOP_HOST_SIM)
Status LvglPresenter::initialize_host() {
  std::fill(g_host_lvgl_panel.pixels.begin(), g_host_lvgl_panel.pixels.end(), 0U);
  const Status initialized = initialize(&g_host_lvgl_panel, &g_host_lvgl_io, nullptr);
  if (initialized) {
    const Status rendered = present(UiSnapshot{});
    if (!rendered) return rendered;
    host_pump(100U);
  }
  return initialized;
}

void LvglPresenter::host_pump(std::uint32_t delta_ms) {
  if (!initialized_ || runtime_ == nullptr) return;
  const std::uint32_t elapsed = std::min<std::uint32_t>(delta_ms, 1000U);
  lv_tick_inc(elapsed);
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) == pdTRUE) {
    (void)lv_timer_handler();
    lv_refr_now(runtime_->display);
    xSemaphoreGive(runtime_->lock);
  }
}

void LvglPresenter::host_pointer(int x, int y, bool pressed) {
  if (!initialized_ || runtime_ == nullptr) return;
  lv_obj_update_layout(lv_display_get_screen_active(runtime_->display));
  runtime_->host_pointer_x = std::clamp(x, 0, board_v1::PinMap::kDisplayWidth - 1);
  runtime_->host_pointer_y = std::clamp(y, 0, board_v1::PinMap::kDisplayHeight - 1);
  runtime_->host_pointer_pressed = pressed;
  // Deliver each host pointer sample through LVGL's production indev reader.
  // This preserves a down/up pair even when a CLI command sends both without
  // waiting for the periodic indev timer.
  lv_tick_inc(32U);
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) == pdTRUE) {
    lv_indev_read(runtime_->touch);
    (void)lv_timer_handler();
    lv_refr_now(runtime_->display);
    xSemaphoreGive(runtime_->lock);
  }
}

const std::uint16_t* LvglPresenter::host_framebuffer() const {
  return initialized_ ? g_host_lvgl_panel.pixels.data() : nullptr;
}

Status LvglPresenter::host_screenshot_png(const std::string& path) const {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "LVGL host presenter is not initialized");
  }
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
    return Status::failure(ErrorCode::kTimeout, "LVGL host screenshot lock timed out");
  }
  lv_obj_update_layout(lv_display_get_screen_active(runtime_->display));
  lv_refr_now(runtime_->display);
  xSemaphoreGive(runtime_->lock);
  if (!lvgl_host_write_png(path, g_host_lvgl_panel.pixels.data(),
                           board_v1::PinMap::kDisplayWidth,
                           board_v1::PinMap::kDisplayHeight)) {
    return Status::failure(ErrorCode::kStorage, "could not write LVGL host PNG");
  }
  return Status::ok();
}

std::vector<LvglPresenter::HostControl> LvglPresenter::host_controls() const {
  std::vector<HostControl> controls;
  if (!initialized_ || runtime_ == nullptr) return controls;
  lv_obj_update_layout(lv_display_get_screen_active(runtime_->display));
  const auto add = [&](const char* id, lv_obj_t* object, lv_obj_t* label) {
    if (!object || !lv_obj_is_visible(object) || !lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE)) return;
    lv_area_t bounds{};
    lv_obj_get_coords(object, &bounds);
    const char* text = label ? lv_label_get_text(label) : "";
    controls.push_back(HostControl{id, text ? text : "", bounds.x1, bounds.y1,
                                   bounds.x2 - bounds.x1 + 1, bounds.y2 - bounds.y1 + 1,
                                   !lv_obj_has_state(object, LV_STATE_DISABLED)});
  };
  const auto& objects = runtime_->objects;
  add("primary", objects.action, objects.action_label);
  add("secondary", objects.history_button, objects.history_button_label);
  add("workout-card", objects.workout_card, objects.target_caption);
  for (std::size_t i = 0; i < 4U; ++i) {
    char id[16];
    std::snprintf(id, sizeof(id), "extra-%zu", i);
    add(id, objects.extra[i], objects.extra_labels[i]);
  }
  for (std::size_t i = 0; i < kPairingRows; ++i) {
    char id[16];
    std::snprintf(id, sizeof(id), "device-%zu", i);
    add(id, objects.candidate_buttons[i], objects.candidate_labels[i]);
  }
  for (std::size_t i = 0; i < kHistoryRows; ++i) {
    char id[16];
    std::snprintf(id, sizeof(id), "history-%zu", i);
    add(id, objects.history_rows[i], objects.history_labels[i]);
  }
  return controls;
}
#endif

Status LvglPresenter::show_setup(const SetupCredentials& credentials) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "LVGL presenter is not initialized");
  }
  if (credentials.ssid.empty() || credentials.passphrase.empty() || credentials.url.empty()) {
    return Status::failure(ErrorCode::kInvalidArgument, "setup display credentials are incomplete");
  }
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
    return Status::failure(ErrorCode::kTimeout, "LVGL setup display lock timed out");
  }

  lv_obj_add_flag(runtime_->objects.status, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.workout, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.target_caption, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.target_value, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.actual_caption, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.actual_value, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.sensors, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.timing, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.terrain, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.safety, LV_OBJ_FLAG_HIDDEN);
  for (auto* bar : runtime_->objects.profile_bars) lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  for (auto* mark : runtime_->objects.status_marks) lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.action, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
  for (std::size_t index = 0U; index < kPairingRows; ++index) {
    lv_obj_add_flag(runtime_->objects.candidate_buttons[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (std::size_t index = 0U; index < kHistoryRows; ++index) {
    lv_obj_add_flag(runtime_->objects.history_rows[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (auto* surface : runtime_->objects.surfaces) lv_obj_add_flag(surface, LV_OBJ_FLAG_HIDDEN);
  for (auto* track : runtime_->objects.progress_tracks) lv_obj_add_flag(track, LV_OBJ_FLAG_HIDDEN);
  for (auto* fill : runtime_->objects.progress_fills) lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
  for (auto* row : runtime_->objects.settings_rows) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  for (auto* label : runtime_->objects.settings_labels) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  for (auto* value : runtime_->objects.settings_values) lv_obj_add_flag(value, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.completion_mark, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.footer, LV_OBJ_FLAG_HIDDEN);

  for (auto* button : runtime_->objects.extra) lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);

  char ssid[64];
  char password[80];
  char url[96];
  std::snprintf(ssid, sizeof(ssid), "SSID  %s", credentials.ssid.c_str());
  std::snprintf(password, sizeof(password), "PASS  %s", credentials.passphrase.c_str());
  std::snprintf(url, sizeof(url), "OPEN  %s", credentials.url.c_str());
  lv_obj_set_pos(runtime_->objects.surfaces[0], theme::kInset, 78);
  lv_obj_set_size(runtime_->objects.surfaces[0], theme::kContentWidth, 96);
  style_surface(runtime_->objects.surfaces[0], theme::surface(), theme::surface());
  lv_obj_move_background(runtime_->objects.surfaces[0]);
  lv_obj_remove_flag(runtime_->objects.surfaces[0], LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(runtime_->objects.setup_title, "Set up TrainerLoop");
  lv_label_set_text(runtime_->objects.setup_instruction, "Join this temporary Wi-Fi network");
  lv_label_set_text(runtime_->objects.setup_ssid, ssid);
  lv_label_set_text(runtime_->objects.setup_password, password);
  lv_label_set_text(runtime_->objects.setup_url, url);
  lv_label_set_text(runtime_->objects.setup_note,
                    "Open the address in your browser.\n"
                    "Setup access expires after 10 minutes.\n"
                    "Your API key is never shown or logged.");
  lv_obj_remove_flag(runtime_->objects.setup_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.setup_instruction, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.setup_ssid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.setup_password, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.setup_url, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.setup_note, LV_OBJ_FLAG_HIDDEN);
  xSemaphoreGive(runtime_->lock);
  return Status::ok();
}

namespace {

void hide_object(lv_obj_t* object) {
  if (object != nullptr) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void show_object(lv_obj_t* object) {
  if (object != nullptr) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void hide_presenter_content(LvglPresenter::Runtime* runtime) {
  if (runtime == nullptr) return;
  auto& objects = runtime->objects;
  for (auto* object : objects.surfaces) hide_object(object);
  for (auto* object : objects.progress_tracks) hide_object(object);
  for (auto* object : objects.progress_fills) hide_object(object);
  for (auto* object : objects.profile_bars) hide_object(object);
  for (auto* object : objects.settings_rows) hide_object(object);
  for (auto* object : objects.settings_labels) hide_object(object);
  for (auto* object : objects.settings_values) hide_object(object);
  for (auto* object : objects.extra) hide_object(object);
  for (auto* object : objects.status_marks) hide_object(object);
  for (auto* object : objects.candidate_buttons) hide_object(object);
  for (auto* object : objects.history_rows) hide_object(object);
  hide_object(objects.status);
  hide_object(objects.workout);
  hide_object(objects.target_caption);
  hide_object(objects.target_value);
  hide_object(objects.actual_caption);
  hide_object(objects.actual_value);
  hide_object(objects.sensors);
  hide_object(objects.timing);
  hide_object(objects.terrain);
  hide_object(objects.safety);
  hide_object(objects.setup_title);
  hide_object(objects.setup_instruction);
  hide_object(objects.setup_ssid);
  hide_object(objects.setup_password);
  hide_object(objects.setup_url);
  hide_object(objects.setup_note);
  hide_object(objects.completion_mark);
  hide_object(objects.footer);
  hide_object(objects.workout_card);
  hide_object(objects.action);
  hide_object(objects.history_button);
}

void show_surface(LvglPresenter::Runtime* runtime, std::size_t index, int x, int y,
                  int width, int height, lv_color_t background = theme::surface(),
                  lv_color_t border = theme::surface()) {
  if (runtime == nullptr || index >= kSurfaceCount) return;
  lv_obj_t* surface = runtime->objects.surfaces[index];
  lv_obj_set_pos(surface, x, y);
  lv_obj_set_size(surface, width, height);
  style_surface(surface, background, border);
  lv_obj_move_background(surface);
  show_object(surface);
}

void show_progress(LvglPresenter::Runtime* runtime, std::size_t index, int x, int y,
                   int width, int height, unsigned percent, lv_color_t color = theme::accent()) {
  if (runtime == nullptr || index >= kProgressTracks) return;
  const unsigned clamped = std::min(percent, 100U);
  lv_obj_t* track = runtime->objects.progress_tracks[index];
  lv_obj_t* fill = runtime->objects.progress_fills[index];
  lv_obj_set_pos(track, x, y);
  lv_obj_set_size(track, width, height);
  style_progress_track(track, theme::surface_alt());
  lv_obj_move_background(track);
  lv_obj_set_pos(fill, x, y);
  lv_obj_set_size(fill, std::max(2, static_cast<int>((width * clamped) / 100U)), height);
  style_progress_fill(fill, color);
  lv_obj_move_foreground(fill);
  show_object(track);
  show_object(fill);
}

void show_workout_card(LvglPresenter::Runtime* runtime, int x, int y, int width, int height,
                       bool enabled) {
  if (runtime == nullptr || runtime->objects.workout_card == nullptr) return;
  lv_obj_t* card = runtime->objects.workout_card;
  runtime->workout_card_context.command = UiCommandType::kOpenWorkoutPreview;
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  style_button(card, theme::surface(), theme::surface_alt(), theme::disabled(), theme::kRadius);
  lv_obj_set_state(card, LV_STATE_DISABLED, !enabled);
  // The card is a single touch target, while its text and profile bars stay
  // passive. Keeping the background behind those labels makes the affordance
  // clear without turning the status rail into a second set of buttons.
  lv_obj_move_background(card);
  show_object(card);
}

void show_status_mark(LvglPresenter::Runtime* runtime, std::size_t index, int x, int y,
                      lv_color_t color) {
  if (runtime == nullptr || index >= 3U) return;
  lv_obj_t* mark = runtime->objects.status_marks[index];
  lv_obj_set_pos(mark, x, y);
  lv_obj_set_size(mark, 8, 8);
  style_surface(mark, color, color, 4);
  lv_obj_move_foreground(mark);
  show_object(mark);
}

lv_color_t link_status_color(const std::string& status) {
  const std::string state = compact_link_state_label(status);
  if (state == "OK") return theme::accent();
  if (state == "RETRY" || state == "CONN") return theme::warning();
  return theme::danger();
}

void show_home_status_rail(LvglPresenter::Runtime* runtime, const UiSnapshot& snapshot) {
  if (runtime == nullptr) return;
  auto& objects = runtime->objects;
  const char* trainer = compact_link_state_label(snapshot.trainer_status);
  const char* heart_rate = compact_link_state_label(snapshot.heart_rate_status);
  const char* wifi = compact_link_state_label(snapshot.wifi_status);
  const lv_color_t trainer_color = link_status_color(snapshot.trainer_status);
  const lv_color_t heart_rate_color = link_status_color(snapshot.heart_rate_status);
  const lv_color_t wifi_color = link_status_color(snapshot.wifi_status);
  const std::string trainer_text = std::string("TRN ") + trainer;
  const std::string heart_rate_text = std::string("HR ") + heart_rate;
  const std::string wifi_text = std::string("WI-FI ") + wifi;
  place_label(objects.actual_caption, 36, 244, 84, 20, &lv_font_montserrat_14,
              trainer_color, trainer_text.c_str(), LV_TEXT_ALIGN_LEFT);
  place_label(objects.actual_value, 140, 244, 84, 20, &lv_font_montserrat_14,
              heart_rate_color, heart_rate_text.c_str(), LV_TEXT_ALIGN_LEFT);
  place_label(objects.sensors, 244, 244, 96, 20, &lv_font_montserrat_14,
              wifi_color, wifi_text.c_str(), LV_TEXT_ALIGN_LEFT);
  show_status_mark(runtime, 0U, 24, 250, trainer_color);
  show_status_mark(runtime, 1U, 128, 250, heart_rate_color);
  show_status_mark(runtime, 2U, 232, 250, wifi_color);
}

void show_primary(LvglPresenter::Runtime* runtime, const char* label,
                  UiCommandType command, bool enabled, lv_color_t background = theme::accent(),
                  lv_color_t pressed = theme::accent_pressed(),
                  lv_color_t text_color = theme::canvas(), int y = theme::kBottomPrimaryY,
                  int height = theme::kButtonHeight) {
  if (runtime == nullptr) return;
  runtime->action_command = command;
  lv_obj_set_pos(runtime->objects.action, theme::kInset, y);
  lv_obj_set_size(runtime->objects.action, theme::kContentWidth, height);
  style_button(runtime->objects.action, background, pressed, theme::disabled(), theme::kRadius);
  lv_label_set_text(runtime->objects.action_label, label == nullptr ? "" : label);
  lv_obj_set_style_text_font(runtime->objects.action_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(runtime->objects.action_label, enabled ? text_color : theme::muted(), LV_PART_MAIN);
  lv_obj_center(runtime->objects.action_label);
  lv_obj_set_state(runtime->objects.action, LV_STATE_DISABLED, !enabled);
  show_object(runtime->objects.action);
}

void style_secondary_button(lv_obj_t* button, lv_obj_t* label, const char* text,
                            bool enabled, lv_color_t label_color) {
  if (button == nullptr || label == nullptr) return;
  style_button(button, theme::surface(), theme::accent(), theme::disabled(), theme::kRadius);
  lv_label_set_text(label, text == nullptr ? "" : text);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, enabled ? label_color : theme::muted(), LV_PART_MAIN);
  lv_obj_center(label);
  lv_obj_set_state(button, LV_STATE_DISABLED, !enabled);
}

void show_secondary(LvglPresenter::Runtime* runtime, const char* label,
                    UiCommandType command, bool enabled, int y = theme::kBottomSecondaryY,
                    int height = 40) {
  if (runtime == nullptr) return;
  runtime->history_context.command = command;
  lv_obj_set_pos(runtime->objects.history_button, theme::kInset, y);
  lv_obj_set_size(runtime->objects.history_button, theme::kContentWidth, height);
  style_secondary_button(runtime->objects.history_button, runtime->objects.history_button_label,
                         label, enabled, theme::ink());
  show_object(runtime->objects.history_button);
}

void show_extra(LvglPresenter::Runtime* runtime, std::size_t index, const char* label,
                UiCommandType command, bool enabled, int x, int y, int width, int height,
                lv_color_t background = theme::surface(), lv_color_t text_color = theme::ink()) {
  if (runtime == nullptr || index >= 4U) return;
  lv_obj_t* button = runtime->objects.extra[index];
  runtime->extra_contexts[index].command = command;
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  style_button(button, background, theme::accent(), theme::disabled(), 8);
  lv_label_set_text(runtime->objects.extra_labels[index], label == nullptr ? "" : label);
  lv_obj_set_style_text_font(runtime->objects.extra_labels[index], &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(runtime->objects.extra_labels[index], enabled ? text_color : theme::muted(), LV_PART_MAIN);
  lv_obj_center(runtime->objects.extra_labels[index]);
  lv_obj_set_state(button, LV_STATE_DISABLED, !enabled);
  show_object(button);
}

void show_danger_secondary(LvglPresenter::Runtime* runtime, const char* label,
                           UiCommandType command, bool enabled, int y = 300,
                           int height = 40) {
  if (runtime == nullptr) return;
  lv_obj_t* button = runtime->objects.extra[0];
  runtime->extra_contexts[0].command = command;
  lv_obj_set_pos(button, theme::kInset, y);
  lv_obj_set_size(button, theme::kContentWidth, height);
  style_secondary_button(button, runtime->objects.extra_labels[0], label, enabled, theme::danger());
  show_object(button);
}

void show_common_labels(LvglPresenter::Runtime* runtime, const char* status,
                        const char* title, lv_color_t status_color = theme::muted(),
                        lv_color_t title_color = theme::ink()) {
  show_object(runtime->objects.status);
  show_object(runtime->objects.workout);
  place_label(runtime->objects.status, theme::kInset, theme::kHeaderY, theme::kContentWidth,
              20, &lv_font_montserrat_14, status_color, status);
  place_label(runtime->objects.workout, theme::kInset, 34, theme::kContentWidth, 30,
              &lv_font_montserrat_24, title_color, title);
}

}  // namespace

void render_snapshot(LvglPresenter::Runtime* runtime, const UiSnapshot& snapshot) {
  if (runtime == nullptr) return;
  auto& objects = runtime->objects;
  hide_presenter_content(runtime);

  const std::string feedback = ascii_ui_text(ui_feedback_message(snapshot));
  const char* feedback_text = feedback.empty() ? "Ready" : feedback.c_str();

  if (snapshot.screen == UiScreen::kHome || snapshot.screen == UiScreen::kPreview) {
    char status[128];
    char duration[16];
    char ftp[24];
    char preview_summary[80];
    const std::string date_label = ui_date_label(snapshot.local_date);
    const char* cache = snapshot.cache_status.empty() ? "status unavailable" : snapshot.cache_status.c_str();
    const bool has_workout = !snapshot.workout_name.empty();
    format_duration(duration, sizeof(duration), snapshot.workout_duration_ms);
    std::snprintf(ftp, sizeof(ftp), "%d W", snapshot.profile.ftp_watts);
    std::snprintf(preview_summary, sizeof(preview_summary), "%s  |  %u intervals",
                  duration, static_cast<unsigned>(snapshot.interval_count));

    if (snapshot.screen == UiScreen::kHome) {
      std::snprintf(status, sizeof(status), "%s  |  %s",
                    date_label.empty() ? "Today" : date_label.c_str(), cache);
      show_common_labels(runtime, status, "TODAY");
      if (!has_workout) {
        show_surface(runtime, 0U, 16, 72, 336, 154, theme::surface(), theme::accent_deep());
        place_label(objects.target_caption, 32, 88, 304, 20, &lv_font_montserrat_14,
                    theme::muted(), "NO WORKOUT CACHED", LV_TEXT_ALIGN_CENTER);
        place_label(objects.target_value, 32, 116, 304, 24, &lv_font_montserrat_14,
                    theme::ink(), "Connect Wi-Fi to sync today's plan", LV_TEXT_ALIGN_CENTER);
        place_label(objects.timing, 32, 194, 304, 20, &lv_font_montserrat_14,
                    snapshot.busy ? theme::warning() : theme::subtle(),
                    snapshot.busy ? "Syncing today's plan" : "Saved rides are safe.",
                    LV_TEXT_ALIGN_CENTER);
        show_home_status_rail(runtime, snapshot);
        const std::string empty_feedback = snapshot.busy
                                                ? "Connecting..."
                                                : (snapshot.fault.empty()
                                                       ? "Connect Wi-Fi, then tap Retry."
                                                       : "Connect Wi-Fi, then tap Retry.");
        place_label(objects.terrain, 16, 272, 336, 18, &lv_font_montserrat_14,
                    snapshot.busy ? theme::warning() : theme::muted(),
                    empty_feedback.c_str(), LV_TEXT_ALIGN_CENTER);
        show_primary(runtime, snapshot.busy ? "SYNCING..." : "RETRY",
                     UiCommandType::kRetry, snapshot.retry_available && !snapshot.busy,
                     theme::accent(), theme::accent_pressed(), theme::canvas(), 304, 48);
        show_extra(runtime, 0U, "DEVICES", UiCommandType::kOpenDevices,
                   snapshot.devices_available && !snapshot.busy, 16, 376, 104, 48);
        show_extra(runtime, 1U, "HISTORY", UiCommandType::kOpenHistory,
                   snapshot.history_available && !snapshot.busy, 132, 376, 104, 48);
        show_extra(runtime, 2U, "SETTINGS", UiCommandType::kOpenSettings,
                   snapshot.settings_available && !snapshot.busy, 248, 376, 104, 48);
        return;
      }

      const std::string card_title = ascii_ui_text(compact_workout_name(snapshot.workout_name));
      show_workout_card(runtime, 16, 72, 336, 154, !snapshot.busy);
      place_label(objects.target_caption, 32, 88, 270, 30, &lv_font_montserrat_24,
                  theme::ink(), card_title.c_str());
      place_label(objects.target_value, 32, 124, 248, 20, &lv_font_montserrat_14,
                  theme::muted(), preview_summary);
      place_label(objects.actual_caption, 304, 100, 28, 24, &lv_font_montserrat_24,
                  theme::accent(), ">", LV_TEXT_ALIGN_CENTER);
      place_label(objects.timing, 32, 207, 300, 18, &lv_font_montserrat_14,
                  theme::subtle(), "Tap for workout details", LV_TEXT_ALIGN_LEFT);
      for (std::size_t index = 0U; index < snapshot.workout_profile_count && index < kUiProfileBars; ++index) {
        const std::uint8_t level = snapshot.workout_profile[index];
        const lv_coord_t height = static_cast<lv_coord_t>(6 + (static_cast<unsigned>(level) * 30U) / 100U);
        lv_obj_set_pos(objects.profile_bars[index], 32 + static_cast<int>(index) * 38, 204 - height);
        lv_obj_set_size(objects.profile_bars[index], 28, height);
        style_surface(objects.profile_bars[index], level >= 70U ? theme::warning() : theme::accent(),
                      level >= 70U ? theme::warning() : theme::accent(), 3);
        show_object(objects.profile_bars[index]);
      }

      show_home_status_rail(runtime, snapshot);
      show_extra(runtime, 0U, "DEVICES", UiCommandType::kOpenDevices,
                 snapshot.devices_available && !snapshot.busy, 16, 344, 104, 48);
      show_extra(runtime, 1U, "HISTORY", UiCommandType::kOpenHistory,
                 snapshot.history_available && !snapshot.busy, 132, 344, 104, 48);
      show_extra(runtime, 2U, "SETTINGS", UiCommandType::kOpenSettings,
                 snapshot.settings_available && !snapshot.busy, 248, 344, 104, 48);
      return;
    }

    std::snprintf(status, sizeof(status), "WORKOUT PREVIEW  |  %s", cache);
    const std::string title = ascii_ui_text(compact_workout_name(snapshot.workout_name));
    show_common_labels(runtime, status, title.empty() ? "Workout preview" : title.c_str());
    show_surface(runtime, 0U, 16, 70, 160, 68);
    show_surface(runtime, 1U, 192, 70, 160, 68);
    place_label(objects.target_caption, 24, 78, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "DURATION");
    place_label(objects.target_value, 24, 94, 144, 34, &lv_font_montserrat_24,
                theme::ink(), duration, LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_caption, 200, 78, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "FTP");
    place_label(objects.actual_value, 200, 94, 144, 34, &lv_font_montserrat_24,
                theme::ink(), ftp, LV_TEXT_ALIGN_CENTER);
    show_surface(runtime, 2U, 16, 154, 336, 52, theme::surface(), theme::surface());
    for (std::size_t index = 0U; index < snapshot.workout_profile_count && index < kUiProfileBars; ++index) {
      const std::uint8_t level = snapshot.workout_profile[index];
      const lv_coord_t height = static_cast<lv_coord_t>(6 + (static_cast<unsigned>(level) * 36U) / 100U);
      lv_obj_set_pos(objects.profile_bars[index], 24 + static_cast<int>(index) * 40, 198 - height);
      lv_obj_set_size(objects.profile_bars[index], 32, height);
      style_surface(objects.profile_bars[index], level >= 70U ? theme::warning() : theme::accent(),
                    level >= 70U ? theme::warning() : theme::accent(), 3);
      show_object(objects.profile_bars[index]);
    }
    place_label(objects.sensors, 16, 214, 336, 24, &lv_font_montserrat_14, theme::ink(),
                preview_summary, LV_TEXT_ALIGN_CENTER);
    const char* trainer = compact_link_state_label(snapshot.trainer_status);
    const char* heart_rate = compact_link_state_label(snapshot.heart_rate_status);
    char readiness[96];
    std::snprintf(readiness, sizeof(readiness), "TRN %s  |  HR %s  |  WI-FI %s", trainer, heart_rate,
                  compact_link_state_label(snapshot.wifi_status));
    place_label(objects.timing, 16, 242, 336, 22, &lv_font_montserrat_14, theme::muted(), readiness,
                LV_TEXT_ALIGN_CENTER);
    char ready_text[128];
    std::snprintf(ready_text, sizeof(ready_text), "Trainer %s\nWorkout %s",
                  snapshot.start_available ? "ready" : "not ready",
                  snapshot.workout_name.empty() ? "not cached" : "cached");
    place_label(objects.terrain, 16, 270, 336, 42, &lv_font_montserrat_14,
                snapshot.start_available ? theme::accent() : theme::warning(), ready_text,
                LV_TEXT_ALIGN_CENTER);
    show_primary(runtime, snapshot.start_available ? "START WORKOUT" : "START UNAVAILABLE",
                 UiCommandType::kStart, snapshot.start_available && !snapshot.busy,
                 theme::accent(), theme::accent_pressed(), theme::canvas(), 320, 48);
    if (snapshot.next_workout_available) {
      show_extra(runtime, 0U, "BACK", UiCommandType::kBack, !snapshot.busy, 16, 376, 160, 48);
      show_extra(runtime, 1U, "NEXT", UiCommandType::kNextWorkout, !snapshot.busy, 192, 376, 160, 48);
    } else {
      show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy, 376, 48);
    }
    return;
  }

  if (snapshot.screen == UiScreen::kActive) {
    char status[96];
    char title[96];
    char target[24];
    char actual[24];
    char heart_rate_value[24];
    char cadence_value[24];
    char sensor_line[64];
    char timing[80];
    char terrain[112];
    char progress_label[96];
    char interval_left[16];
    format_duration(interval_left, sizeof(interval_left), snapshot.interval_remaining_ms);
    std::snprintf(status, sizeof(status), "TRN %s  |  HR %s",
                  compact_link_state_label(snapshot.trainer_status),
                  compact_link_state_label(snapshot.heart_rate_status));
    const std::string active_workout_name = compact_workout_name(
        snapshot.workout_name.empty() ? "Workout" : snapshot.workout_name);
    std::snprintf(title, sizeof(title), "%s  |  %s",
                  snapshot.interval_status.empty() ? "Interval" : snapshot.interval_status.c_str(),
                  active_workout_name.c_str());
    std::snprintf(target, sizeof(target), "%d", snapshot.target_power_w);
    std::snprintf(actual, sizeof(actual), "%s", snapshot.actual_power_w.has_value()
                                                   ? std::to_string(*snapshot.actual_power_w).c_str()
                                                   : "--");
    std::snprintf(sensor_line, sizeof(sensor_line), "HR %s     CAD %s",
                  optional_integer(snapshot.heart_rate_bpm, heart_rate_value, sizeof(heart_rate_value)),
                  optional_decimal(snapshot.cadence_rpm, cadence_value, sizeof(cadence_value)));
    char active_time[16];
    format_duration(active_time, sizeof(active_time), snapshot.elapsed_active_ms);
    char interval_time[16];
    format_duration(interval_time, sizeof(interval_time), snapshot.interval_remaining_ms);
    std::snprintf(timing, sizeof(timing), "Elapsed %s  |  Interval %s left", active_time, interval_time);
    if (snapshot.speed_kph.has_value() && snapshot.grade_percent.has_value() &&
        snapshot.distance_km.has_value() && snapshot.relative_ascent_m.has_value()) {
      std::snprintf(terrain, sizeof(terrain), "%.1f km/h   %+.1f%% grade\n%.1f km   +%.0f m virtual",
                    *snapshot.speed_kph, *snapshot.grade_percent, *snapshot.distance_km,
                    *snapshot.relative_ascent_m);
    } else {
      std::snprintf(terrain, sizeof(terrain), "--.- km/h   --.-%% grade\n--.- km   +-- m virtual");
    }
    std::snprintf(progress_label, sizeof(progress_label), "Interval %s left  |  Workout %u%%",
                  interval_left, static_cast<unsigned>(snapshot.workout_progress_percent));
    show_common_labels(runtime, status, title);
    // Keep the interval name and workout name legible in the narrow header.
    place_label(objects.workout, theme::kInset, 34, theme::kContentWidth, 22,
                &lv_font_montserrat_14, theme::ink(), title);
    show_surface(runtime, 0U, 16, 68, 160, 74);
    show_surface(runtime, 1U, 192, 68, 160, 74);
    place_label(objects.target_caption, 24, 76, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "TARGET", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_value, 24, 90, 144, 48, &lv_font_montserrat_48,
                theme::ink(), target, LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_caption, 200, 76, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "ACTUAL", LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_value, 200, 90, 144, 48, &lv_font_montserrat_48,
                theme::accent(), actual, LV_TEXT_ALIGN_CENTER);
    show_surface(runtime, 2U, 16, 150, 336, 46);
    place_label(objects.sensors, 24, 158, 320, 28, &lv_font_montserrat_24,
                theme::ink(), sensor_line, LV_TEXT_ALIGN_CENTER);
    place_label(objects.timing, 16, 207, 336, 22, &lv_font_montserrat_14,
                theme::ink(), timing, LV_TEXT_ALIGN_CENTER);
    const unsigned interval_total = snapshot.interval_elapsed_ms + snapshot.interval_remaining_ms;
    const unsigned interval_percent = interval_total == 0U
                                          ? 0U
                                          : static_cast<unsigned>((100ULL * snapshot.interval_elapsed_ms) /
                                                                   interval_total);
    show_progress(runtime, 0U, 76, 239, 216, 8, interval_percent);
    show_progress(runtime, 1U, 76, 265, 216, 8, snapshot.workout_progress_percent);
    place_label(objects.safety, 16, 278, 336, 22, &lv_font_montserrat_14,
                theme::muted(), progress_label, LV_TEXT_ALIGN_CENTER);
    place_label(objects.terrain, 16, 302, 336, 46, &lv_font_montserrat_14,
                theme::ink(), terrain, LV_TEXT_ALIGN_CENTER);
    show_primary(runtime, "PAUSE", UiCommandType::kPause,
                 snapshot.pause_available && !snapshot.busy, theme::accent(),
                 theme::accent_pressed(), theme::canvas(), 384, 48);
    return;
  }

  if (snapshot.screen == UiScreen::kPaused) {
    const bool resistance_unknown = snapshot.safety_message.find("STOP PEDALING") != std::string::npos;
    if (resistance_unknown) {
      show_common_labels(runtime, "WORKOUT PAUSED", "STOP PEDALING", theme::warning(), theme::danger());
      place_label(objects.target_caption, 16, 82, 336, 18, &lv_font_montserrat_14,
                  theme::muted(), "RESISTANCE", LV_TEXT_ALIGN_CENTER);
      place_label(objects.target_value, 16, 96, 336, 36, &lv_font_montserrat_24,
                  theme::danger(), "UNKNOWN", LV_TEXT_ALIGN_CENTER);
      place_label(objects.sensors, 16, 154, 336, 26, &lv_font_montserrat_24,
                  theme::warning(), "Trainer disconnected", LV_TEXT_ALIGN_CENTER);
      place_label(objects.timing, 16, 190, 336, 24, &lv_font_montserrat_14,
                  theme::muted(), "Reconnecting...", LV_TEXT_ALIGN_CENTER);
      place_label(objects.terrain, 16, 236, 336, 24, &lv_font_montserrat_14,
                  theme::muted(), "Ride data retained", LV_TEXT_ALIGN_CENTER);
      show_danger_secondary(runtime, "DISCARD RIDE", UiCommandType::kOpenDiscardConfirmation,
                            snapshot.discard_available && !snapshot.busy);
      show_primary(runtime, "END & SAVE", UiCommandType::kOpenStopConfirmation,
                   snapshot.end_save_available && !snapshot.busy, theme::danger(),
                   theme::warning(), theme::canvas());
      return;
    }
    char duration[16];
    format_duration(duration, sizeof(duration), snapshot.elapsed_active_ms);
    show_common_labels(runtime, "WORKOUT PAUSED", "Workout paused", theme::warning());
    place_label(objects.target_caption, 16, 82, 336, 18, &lv_font_montserrat_14,
                theme::muted(), "ACTIVE TIME", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_value, 16, 96, 336, 58, &lv_font_montserrat_48,
                theme::ink(), duration, LV_TEXT_ALIGN_CENTER);
    place_label(objects.sensors, 16, 176, 336, 26, &lv_font_montserrat_24,
                theme::accent(), "Trainer pause confirmed", LV_TEXT_ALIGN_CENTER);
    char paused_context[64];
    char paused_interval_left[16];
    format_duration(paused_interval_left, sizeof(paused_interval_left), snapshot.interval_remaining_ms);
    std::snprintf(paused_context, sizeof(paused_context), "%s  |  %s left",
                  snapshot.interval_status.empty() ? "Workout paused" : snapshot.interval_status.c_str(),
                  paused_interval_left);
    place_label(objects.timing, 16, 214, 336, 24, &lv_font_montserrat_14,
                theme::muted(), paused_context, LV_TEXT_ALIGN_CENTER);
    show_danger_secondary(runtime, "DISCARD RIDE", UiCommandType::kOpenDiscardConfirmation,
                          snapshot.discard_available && !snapshot.busy, theme::kPausedDiscardY);
    show_primary(runtime, "RESUME", UiCommandType::kResume,
                 snapshot.resume_available && !snapshot.busy, theme::accent(),
                 theme::accent_pressed(), theme::canvas(), theme::kPausedResumeY,
                 theme::kButtonHeight);
    show_secondary(runtime, "END & SAVE", UiCommandType::kOpenStopConfirmation,
                   snapshot.end_save_available && !snapshot.busy, theme::kPausedEndSaveY);
    return;
  }

  if (snapshot.screen == UiScreen::kDiscardConfirmation) {
    char duration[16];
    format_duration(duration, sizeof(duration), stop_confirmation_duration_ms(snapshot));
    show_common_labels(runtime, "CANCEL WORKOUT?", "", theme::muted());
    show_surface(runtime, 0U, 16, 70, 336, 222, theme::surface(), theme::danger());
    place_label(objects.workout, 28, 96, 312, 24, &lv_font_montserrat_14,
                theme::ink(), "This ride will be discarded.", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_caption, 32, 144, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "DURATION", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_value, 32, 158, 144, 48, &lv_font_montserrat_48,
                theme::ink(), duration, LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_caption, 192, 144, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "STATE", LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_value, 192, 160, 144, 34, &lv_font_montserrat_24,
                theme::warning(), "PAUSED", LV_TEXT_ALIGN_CENTER);
    show_primary(runtime, "DISCARD RIDE", UiCommandType::kDiscardWorkout,
                 snapshot.discard_available && !snapshot.busy, theme::danger(),
                 theme::warning(), theme::canvas());
    show_secondary(runtime, "KEEP PAUSED", UiCommandType::kBack, !snapshot.busy);
    return;
  }

  if (snapshot.screen == UiScreen::kStopConfirmation) {
    char duration[16];
    format_duration(duration, sizeof(duration), stop_confirmation_duration_ms(snapshot));
    show_common_labels(runtime, "END WORKOUT?", "", theme::muted());
    show_surface(runtime, 0U, 16, 70, 336, 222, theme::surface(), theme::accent_deep());
    place_label(objects.workout, 28, 96, 312, 24, &lv_font_montserrat_14,
                theme::ink(), "Your partial ride will be saved.", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_caption, 32, 144, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "DURATION", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_value, 32, 158, 144, 48, &lv_font_montserrat_48,
                theme::ink(), duration, LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_caption, 192, 144, 144, 18, &lv_font_montserrat_14,
                theme::muted(), "STATE", LV_TEXT_ALIGN_CENTER);
    place_label(objects.actual_value, 192, 160, 144, 34, &lv_font_montserrat_24,
                theme::warning(), "PAUSED", LV_TEXT_ALIGN_CENTER);
    show_primary(runtime, "END & SAVE", UiCommandType::kEndAndSave,
                 snapshot.end_save_available && !snapshot.busy, theme::danger(),
                 theme::warning(), theme::canvas());
    show_secondary(runtime, "STAY PAUSED", UiCommandType::kBack, !snapshot.busy);
    return;
  }

  if (snapshot.screen == UiScreen::kCompletion) {
    char duration[16];
    char summary_line[96];
    char terrain_summary[96];
    const SessionSummary* summary = snapshot.completion_summary.has_value()
                                        ? &*snapshot.completion_summary : nullptr;
    const std::uint32_t wall_ms = summary == nullptr ? snapshot.elapsed_wall_ms : summary->wall_ms;
    format_duration(duration, sizeof(duration), wall_ms);
    const int average_power = summary == nullptr ? 0 : summary->average_power_w;
    const int average_hr = summary == nullptr ? 0 : summary->average_heart_rate_bpm;
    const double distance_km = summary == nullptr ? 0.0 : static_cast<double>(summary->distance_cm) / 100000.0;
    const double ascent_m = summary == nullptr ? 0.0 : static_cast<double>(summary->ascent_cm) / 100.0;
    std::snprintf(summary_line, sizeof(summary_line), "AVG %d W  |  HR %d",
                  average_power, average_hr);
    std::snprintf(terrain_summary, sizeof(terrain_summary), "%.1f km  |  +%.0f m virtual",
                  distance_km, ascent_m);
    const std::string title = compact_workout_name(snapshot.workout_name.empty()
                                                       ? "Workout saved" : snapshot.workout_name);
    show_common_labels(runtime, "WORKOUT SAVED", "Workout saved", theme::accent());
    place_label(objects.completion_mark, 144, 56, 80, 42, &lv_font_montserrat_24,
                theme::accent(), "OK", LV_TEXT_ALIGN_CENTER);
    show_object(objects.completion_mark);
    place_label(objects.workout, 16, 102, 336, 24, &lv_font_montserrat_14,
                theme::ink(), (title + "  |  Complete").c_str(), LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_caption, 16, 140, 336, 18, &lv_font_montserrat_14,
                theme::muted(), "DURATION", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_value, 16, 152, 336, 48, &lv_font_montserrat_48,
                theme::ink(), duration, LV_TEXT_ALIGN_CENTER);
    show_surface(runtime, 3U, 16, 216, 336, 50);
    place_label(objects.sensors, 24, 224, 320, 18, &lv_font_montserrat_14,
                theme::ink(), summary_line, LV_TEXT_ALIGN_CENTER);
    place_label(objects.timing, 24, 246, 320, 18, &lv_font_montserrat_14,
                theme::muted(), terrain_summary, LV_TEXT_ALIGN_CENTER);
    const std::string upload = friendly_upload_state(snapshot.completion_upload_status);
    show_surface(runtime, 4U, 16, 278, 336, 54,
                 snapshot.completion_upload_status == "UPLOADED" ? theme::surface() : theme::accent_deep(),
                 snapshot.completion_upload_status == "UPLOADED" ? theme::surface() : theme::accent_deep());
    place_label(objects.safety, 24, 288, 320, 34, &lv_font_montserrat_14,
                snapshot.completion_upload_status == "UPLOADED" ? theme::accent() : theme::warning(),
                upload.c_str(), LV_TEXT_ALIGN_CENTER);
    show_primary(runtime, "HOME", UiCommandType::kBack, !snapshot.busy);
    return;
  }

  if (snapshot.screen == UiScreen::kPairing) {
    const char* mode = snapshot.pairing_scan_active ? "Scanning for devices" :
                       snapshot.pairing_scan_pending ? "Waiting to scan" : "Choose a device";
    show_common_labels(runtime, "DEVICES", "Pair devices");
    place_label(objects.sensors, 16, 68, 336, 20, &lv_font_montserrat_14,
                theme::muted(), mode, LV_TEXT_ALIGN_CENTER);
    for (std::size_t index = 0U; index < snapshot.device_candidates.size() && index < kPairingRows; ++index) {
      const DeviceIdentity& candidate = snapshot.device_candidates[index];
      char text[128];
      const char* name = candidate.name.empty() ? "Unnamed device" : candidate.name.c_str();
      const char* role = candidate.role == PeripheralRole::kTrainer ? "Trainer" : "Heart rate";
      std::snprintf(text, sizeof(text), "%s  |  %s\nSignal %d dBm", role, name,
                    static_cast<int>(candidate.rssi));
      lv_label_set_text(objects.candidate_labels[index], text);
      lv_obj_set_style_text_color(objects.candidate_labels[index], theme::ink(), LV_PART_MAIN);
      show_object(objects.candidate_buttons[index]);
      show_object(objects.candidate_labels[index]);
    }
    if (snapshot.device_candidates.empty()) {
      place_label(objects.safety, 16, 296, 336, 26, &lv_font_montserrat_14,
                  theme::muted(), feedback_text, LV_TEXT_ALIGN_CENTER);
    }
    show_primary(runtime, snapshot.pairing_scan_active ? "STOP SCAN" : "SCAN AGAIN",
                 snapshot.pairing_scan_active || snapshot.pairing_scan_pending
                     ? UiCommandType::kBack : UiCommandType::kOpenDevices,
                 !snapshot.busy);
    show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy);
    return;
  }

  if (snapshot.screen == UiScreen::kHistory) {
    show_common_labels(runtime, "HISTORY  |  SAVED ON DEVICE", "History");
    place_label(objects.sensors, 16, 64, 336, 20, &lv_font_montserrat_14,
                theme::muted(), "Recent rides  |  Wi-Fi is not required", LV_TEXT_ALIGN_CENTER);
    for (std::size_t index = 0U; index < snapshot.history_entries.size() && index < kHistoryRows; ++index) {
      const UiHistoryEntry& entry = snapshot.history_entries[index];
      char duration[16];
      char text[192];
      format_duration(duration, sizeof(duration), entry.summary.wall_ms);
      const char* completion = entry.summary.interrupted ? "Partial" :
                               (entry.summary.complete ? "Complete" : "Saved");
      const std::string upload = friendly_upload_state(entry.upload_status);
      const std::string workout_name = compact_workout_name(
          entry.summary.workout_name.empty() ? "Unnamed workout" : entry.summary.workout_name);
      std::snprintf(text, sizeof(text), "%s  |  %s\n%s  |  %s",
                    workout_name.c_str(),
                    duration, completion, upload.c_str());
      lv_label_set_text(objects.history_labels[index], text);
      lv_obj_set_style_text_color(objects.history_labels[index],
                                  entry.upload_status == "UPLOADED" ? theme::accent() : theme::ink(),
                                  LV_PART_MAIN);
      show_object(objects.history_rows[index]);
      show_object(objects.history_labels[index]);
    }
    if (snapshot.history_entries.empty()) {
      place_label(objects.safety, 16, 180, 336, 26, &lv_font_montserrat_14,
                  theme::muted(), "No saved rides yet", LV_TEXT_ALIGN_CENTER);
    }
    if (!snapshot.history_entries.empty()) {
      show_primary(runtime, "RETRY UPLOAD", UiCommandType::kRetryUploads, !snapshot.busy);
      show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy);
    } else {
      show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy, 376, 48);
    }
    return;
  }

  if (snapshot.screen == UiScreen::kHistoryDetail) {
    const UiHistoryEntry* detail = snapshot.history_detail.has_value() ? &*snapshot.history_detail : nullptr;
    const SessionSummary* summary = detail == nullptr ? nullptr : &detail->summary;
    char duration[16];
    char metrics[96];
    char terrain[96];
    format_duration(duration, sizeof(duration), summary == nullptr ? 0U : summary->wall_ms);
    std::snprintf(metrics, sizeof(metrics), "AVG %d W  |  HR %d",
                  summary == nullptr ? 0 : static_cast<int>(summary->average_power_w),
                  summary == nullptr ? 0 : static_cast<int>(summary->average_heart_rate_bpm));
    std::snprintf(terrain, sizeof(terrain), "%.1f km  |  +%.0f m virtual",
                  summary == nullptr ? 0.0 : static_cast<double>(summary->distance_cm) / 100000.0,
                  summary == nullptr ? 0.0 : static_cast<double>(summary->ascent_cm) / 100.0);
    const std::string detail_workout_name = compact_workout_name(
        summary == nullptr || summary->workout_name.empty() ? "Saved workout" : summary->workout_name);
    show_common_labels(runtime, "RIDE DETAILS", detail_workout_name.c_str());
    place_label(objects.target_caption, 16, 76, 336, 18, &lv_font_montserrat_14,
                theme::muted(), "DURATION", LV_TEXT_ALIGN_CENTER);
    place_label(objects.target_value, 16, 90, 336, 48, &lv_font_montserrat_48,
                theme::ink(), duration, LV_TEXT_ALIGN_CENTER);
    show_surface(runtime, 5U, 16, 154, 336, 52);
    place_label(objects.sensors, 24, 162, 320, 18, &lv_font_montserrat_14,
                theme::ink(), metrics, LV_TEXT_ALIGN_CENTER);
    place_label(objects.timing, 24, 186, 320, 18, &lv_font_montserrat_14,
                theme::muted(), terrain, LV_TEXT_ALIGN_CENTER);
    const std::string upload = detail == nullptr ? snapshot.fault : friendly_upload_state(detail->upload_status);
    place_label(objects.safety, 16, 236, 336, 32, &lv_font_montserrat_14,
                detail != nullptr && detail->upload_status == "UPLOADED" ? theme::accent() : theme::warning(),
                upload.c_str(), LV_TEXT_ALIGN_CENTER);
    show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy, 376, 48);
    return;
  }

  if (snapshot.screen == UiScreen::kSettings) {
    static const char* names[kSettingsRows] = {"FTP", "Rider weight", "Bike weight", "Crr", "CdA"};
    static const char* units[kSettingsRows] = {"W", "kg", "kg", "", "m2"};
    const double values[kSettingsRows] = {
        static_cast<double>(snapshot.profile.ftp_watts), snapshot.profile.rider_kg,
        snapshot.profile.bike_kg, snapshot.profile.crr, snapshot.profile.cda};
    show_common_labels(runtime, "SETTINGS", "Rider & physics");
    for (std::size_t index = 0U; index < kSettingsRows; ++index) {
      const int y = 66 + static_cast<int>(index) * 38;
      const bool selected = index == std::min(snapshot.profile_field, kSettingsRows - 1U);
      show_object(objects.settings_rows[index]);
      show_object(objects.settings_labels[index]);
      show_object(objects.settings_values[index]);
      lv_obj_set_pos(objects.settings_rows[index], 16, y);
      lv_obj_set_size(objects.settings_rows[index], 336, 34);
      style_surface(objects.settings_rows[index], selected ? theme::accent_deep() : theme::surface(),
                    selected ? theme::accent_deep() : theme::surface());
      char value[32];
      if (index == 0U) std::snprintf(value, sizeof(value), "%.0f %s", values[index], units[index]);
      else if (index == 3U) std::snprintf(value, sizeof(value), "%.4f", values[index]);
      else if (index == 4U) std::snprintf(value, sizeof(value), "%.2f %s", values[index], units[index]);
      else std::snprintf(value, sizeof(value), "%.1f %s", values[index], units[index]);
      place_label(objects.settings_labels[index], 28, y + 6, 180, 22,
                  &lv_font_montserrat_14, theme::ink(), names[index]);
      place_label(objects.settings_values[index], 212, y + 6, 112, 22,
                  &lv_font_montserrat_14, selected ? theme::accent() : theme::ink(), value,
                  LV_TEXT_ALIGN_RIGHT);
    }
    place_label(objects.safety, 16, 260, 336, 18, &lv_font_montserrat_14,
                theme::muted(), "Changes apply to the next ride.", LV_TEXT_ALIGN_CENTER);
    show_extra(runtime, 0U, "-", UiCommandType::kProfileDecrease, !snapshot.busy,
               16, 286, 56, 40);
    show_extra(runtime, 1U, "+", UiCommandType::kProfileIncrease, !snapshot.busy,
               80, 286, 56, 40);
    show_extra(runtime, 2U, "NEXT FIELD", UiCommandType::kProfileNext, !snapshot.busy,
               144, 286, 120, 40);
    show_primary(runtime, "SAVE", UiCommandType::kSaveProfile, !snapshot.busy,
                 theme::accent(), theme::accent_pressed(), theme::canvas(), 336, 48);
    show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy, 400, 40);
    return;
  }

  if (snapshot.screen == UiScreen::kError) {
    show_common_labels(runtime, "ACTION NEEDED", "Save incomplete", theme::warning());
    place_label(objects.safety, 16, 84, 336, 70, &lv_font_montserrat_14,
                theme::warning(), feedback_text, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(objects.safety, LV_LABEL_LONG_WRAP);
    show_primary(runtime, "RETRY SAVE", UiCommandType::kRetry, snapshot.retry_available && !snapshot.busy);
    show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy);
    return;
  }

  show_common_labels(runtime, "TRAINERLOOP", "Upload status");
  place_label(objects.safety, 16, 94, 336, 58, &lv_font_montserrat_14,
              theme::muted(), feedback_text, LV_TEXT_ALIGN_CENTER);
  show_secondary(runtime, "BACK", UiCommandType::kBack, !snapshot.busy, 376, 48);
}

Status LvglPresenter::present(const UiSnapshot& snapshot) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "LVGL presenter is not initialized");
  }
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
    return Status::failure(ErrorCode::kTimeout, "LVGL update lock timed out");
  }

  // The screen is rebuilt from the snapshot using the shared design tokens
  // above. The legacy dashboard path remains below as a source-compatible
  // fallback during this migration, but is unreachable once the aligned
  // renderer succeeds.
  render_snapshot(runtime_, snapshot);
  runtime_->screen = snapshot.screen;
  xSemaphoreGive(runtime_->lock);
  return Status::ok();

#if 0  // Retained only as a historical reference for the pre-alignment layout.
  lv_obj_remove_flag(runtime_->objects.status, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.workout, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.target_caption, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.target_value, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.actual_caption, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.actual_value, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.sensors, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.timing, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.terrain, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.safety, LV_OBJ_FLAG_HIDDEN);
  for (auto* bar : runtime_->objects.profile_bars) lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(runtime_->objects.action, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.setup_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.setup_instruction, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.setup_ssid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.setup_password, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.setup_url, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.setup_note, LV_OBJ_FLAG_HIDDEN);

  char target[24];
  char actual[24];
  char heart_rate[16];
  char cadence[16];
  char sensors[64];
  char timing[64];
  char terrain[96];
  char status[192];
  char workout[192];
  char safety[192];
  char active[16];
  char wall[16];
  format_duration(active, sizeof(active), snapshot.elapsed_active_ms);
  format_duration(wall, sizeof(wall), snapshot.elapsed_wall_ms);
  std::snprintf(target, sizeof(target), "%d W", snapshot.target_power_w);
  if (snapshot.actual_power_w.has_value()) {
    std::snprintf(actual, sizeof(actual), "%d W", *snapshot.actual_power_w);
  } else {
    std::snprintf(actual, sizeof(actual), "-- W");
  }
  std::snprintf(sensors, sizeof(sensors), "HR %s     CAD %s",
                optional_integer(snapshot.heart_rate_bpm, heart_rate, sizeof(heart_rate)),
                optional_decimal(snapshot.cadence_rpm, cadence, sizeof(cadence)));
  std::snprintf(timing, sizeof(timing), "%s active     %s elapsed", active, wall);
  if (snapshot.speed_kph.has_value() && snapshot.grade_percent.has_value() &&
      snapshot.distance_km.has_value() && snapshot.relative_ascent_m.has_value()) {
    std::snprintf(terrain, sizeof(terrain), "%.1f km/h     %+.1f%% grade\n%.1f km        +%.0f m %s",
                  *snapshot.speed_kph, *snapshot.grade_percent, *snapshot.distance_km,
                  *snapshot.relative_ascent_m, snapshot.simulation_is_virtual ? "virtual" : "ride");
  } else {
    std::snprintf(terrain, sizeof(terrain), "--.- km/h     --.-%% grade\n--.- km        +-- m virtual");
  }
  std::snprintf(status, sizeof(status), "TRN %s   HR %s   Wi-Fi %s",
                compact_link_state_label(snapshot.trainer_status),
                compact_link_state_label(snapshot.heart_rate_status),
                compact_link_state_label(snapshot.wifi_status));
  std::snprintf(workout, sizeof(workout), "%s%s%s",
                snapshot.workout_name.empty() ? "No workout selected" : snapshot.workout_name.c_str(),
                snapshot.interval_status.empty() ? "" : "  |  ",
                snapshot.interval_status.empty() ? "" : snapshot.interval_status.c_str());
  const std::string feedback = ui_feedback_message(snapshot);
  const char* safety_text = feedback.c_str();
  std::snprintf(safety, sizeof(safety), "%s", safety_text[0] == '\0' ? "Ready" : safety_text);

  lv_label_set_text(runtime_->objects.status, status);
  lv_label_set_text(runtime_->objects.workout, workout);
  lv_label_set_text(runtime_->objects.target_caption, "TARGET");
  lv_label_set_text(runtime_->objects.actual_caption, "ACTUAL");
  lv_label_set_text(runtime_->objects.target_value, target);
  lv_label_set_text(runtime_->objects.actual_value, actual);
  lv_label_set_text(runtime_->objects.sensors, sensors);
  lv_label_set_text(runtime_->objects.timing, timing);
  lv_label_set_text(runtime_->objects.terrain, terrain);
  lv_label_set_text(runtime_->objects.safety, safety);

  if (snapshot.screen == UiScreen::kActive && snapshot.interval_count > 0U) {
    char active_time[16];
    char wall_time[16];
    char interval_left[16];
    char active_timing[64];
    format_duration(active_time, sizeof(active_time), snapshot.elapsed_active_ms);
    format_duration(wall_time, sizeof(wall_time), snapshot.elapsed_wall_ms);
    format_duration(interval_left, sizeof(interval_left), snapshot.interval_remaining_ms);
    std::snprintf(active_timing, sizeof(active_timing), "%s active   %s elapsed", active_time, wall_time);
    lv_label_set_text(runtime_->objects.timing, active_timing);
    if (snapshot.safety_message.empty() && snapshot.fault.empty()) {
      char progress[80];
      std::snprintf(progress, sizeof(progress), "Interval %u/%u  •  %s left  •  %u%% workout",
                    static_cast<unsigned>(snapshot.interval_index + 1U),
                    static_cast<unsigned>(snapshot.interval_count),
                    interval_left,
                    static_cast<unsigned>(snapshot.workout_progress_percent));
      lv_label_set_text(runtime_->objects.safety, progress);
    }
  }

  if (snapshot.screen == UiScreen::kHome || snapshot.screen == UiScreen::kPreview) {
    char duration[16];
    char ftp[24];
    char today[192];
    const std::string date_label = ui_date_label(snapshot.local_date);
    format_duration(duration, sizeof(duration), snapshot.workout_duration_ms);
    std::snprintf(ftp, sizeof(ftp), "%d W", snapshot.profile.ftp_watts);
    std::snprintf(status, sizeof(status), "%s  •  %s",
                  date_label.empty() ? "Today" : date_label.c_str(),
                  snapshot.cache_status.empty() ? "status unavailable" : snapshot.cache_status.c_str());
    const char* context = snapshot.screen == UiScreen::kPreview ? "Preview" : "Today";
    const char* workout_name = snapshot.workout_name.empty()
                                   ? "No workout selected"
                                   : snapshot.workout_name.c_str();
    if (snapshot.today_workout_count > 1U) {
      std::snprintf(today, sizeof(today), "%s %u/%u  |  %s\n%s • %u intervals",
                    context,
                    static_cast<unsigned>(snapshot.today_workout_index + 1U),
                    static_cast<unsigned>(snapshot.today_workout_count),
                    workout_name, duration,
                    static_cast<unsigned>(snapshot.interval_count));
    } else {
      if (snapshot.workout_name.empty()) {
        std::snprintf(today, sizeof(today), "%s  |  %s", context, workout_name);
      } else {
        std::snprintf(today, sizeof(today), "%s  |  %s\n%s • %u interval%s",
                      context, workout_name, duration,
                      static_cast<unsigned>(snapshot.interval_count),
                      snapshot.interval_count == 1U ? "" : "s");
      }
    }
    lv_label_set_text(runtime_->objects.workout, today);
    lv_label_set_text(runtime_->objects.target_caption, "DURATION");
    lv_label_set_text(runtime_->objects.target_value,
                      snapshot.workout_name.empty() ? "--:--" : duration);
    lv_label_set_text(runtime_->objects.actual_caption, "FTP");
    lv_label_set_text(runtime_->objects.actual_value, ftp);
    char readiness[96];
    std::snprintf(readiness, sizeof(readiness), "Trainer %s   HR %s",
                  compact_link_state_label(snapshot.trainer_status),
                  compact_link_state_label(snapshot.heart_rate_status));
    lv_label_set_text(runtime_->objects.sensors, readiness);
    for (std::size_t index = 0U; index < snapshot.workout_profile_count && index < kUiProfileBars; ++index) {
      const std::uint8_t level = snapshot.workout_profile[index];
      const lv_coord_t height = static_cast<lv_coord_t>(4 + (static_cast<unsigned>(level) * 10U) / 100U);
      lv_obj_set_pos(runtime_->objects.profile_bars[index], 12 + static_cast<int>(index) * 42,
                     202 - height);
      lv_obj_set_size(runtime_->objects.profile_bars[index], 36, height);
      lv_obj_set_style_bg_color(runtime_->objects.profile_bars[index],
                                level >= 70U ? lv_color_hex(0xFFB547) : lv_color_hex(0x45D6A5),
                                LV_PART_MAIN);
      lv_obj_remove_flag(runtime_->objects.profile_bars[index], LV_OBJ_FLAG_HIDDEN);
    }
    if (snapshot.today_workout_count > 1U && snapshot.safety_message.empty() &&
        (snapshot.fault.empty() || snapshot.fault == "Multiple workouts found — use NEXT" ||
         snapshot.fault.find("selected") != std::string::npos)) {
      char selection[64];
      std::snprintf(selection, sizeof(selection), "Workout %u of %u • tap NEXT",
                    static_cast<unsigned>(snapshot.today_workout_index + 1U),
                    static_cast<unsigned>(snapshot.today_workout_count));
      lv_label_set_text(runtime_->objects.safety, selection);
    }
  }
  if (snapshot.screen == UiScreen::kPaused) {
    lv_label_set_text(runtime_->objects.status, "WORKOUT PAUSED");
    lv_label_set_text(runtime_->objects.workout, "Workout paused");
    lv_label_set_text(runtime_->objects.safety,
                      snapshot.safety_message.empty() ? "Trainer pause confirmed" : snapshot.safety_message.c_str());
  }
  if (snapshot.screen == UiScreen::kStopConfirmation) {
    char duration[16];
    format_duration(duration, sizeof(duration), snapshot.elapsed_wall_ms);
    lv_label_set_text(runtime_->objects.status, "END WORKOUT?");
    lv_label_set_text(runtime_->objects.workout, "Save this ride locally?");
    lv_label_set_text(runtime_->objects.target_caption, "DURATION");
    lv_label_set_text(runtime_->objects.target_value, duration);
    lv_label_set_text(runtime_->objects.actual_caption, "STATE");
    lv_label_set_text(runtime_->objects.actual_value, "PAUSED");
    lv_label_set_text(runtime_->objects.sensors, "END & SAVE creates a ride");
    lv_label_set_text(runtime_->objects.timing, "");
    lv_label_set_text(runtime_->objects.terrain, "Resume remains available after Cancel");
    lv_label_set_text(runtime_->objects.safety,
                      snapshot.safety_message.empty() ? "Choose END & SAVE or CANCEL"
                                                       : snapshot.safety_message.c_str());
  }
  lv_obj_set_width(runtime_->objects.action, snapshot.screen == UiScreen::kHome ? 168 : 344);

  for (std::size_t index = 0U; index < kPairingRows; ++index) {
    lv_obj_add_flag(runtime_->objects.candidate_buttons[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (std::size_t index = 0U; index < kHistoryRows; ++index) {
    lv_obj_add_flag(runtime_->objects.history_rows[index], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_add_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
  if (snapshot.screen == UiScreen::kPairing) {
    lv_label_set_text(runtime_->objects.status,
                      snapshot.pairing_scan_active
                          ? "PAIRING  |  SCANNING"
                          : snapshot.pairing_scan_pending
                                ? "PAIRING  |  WAITING"
                                : "PAIRING  |  CHOOSE DEVICE");
    char pair_status[192];
    std::snprintf(pair_status, sizeof(pair_status), "Trainer: %s\nHR: %s",
                  snapshot.trainer_status.c_str(), snapshot.heart_rate_status.c_str());
    lv_label_set_text(runtime_->objects.workout, pair_status);
    lv_label_set_text(runtime_->objects.target_caption, "");
    lv_label_set_text(runtime_->objects.actual_caption, "");
    lv_label_set_text(runtime_->objects.target_value, "");
    lv_label_set_text(runtime_->objects.actual_value, "");
    lv_label_set_text(runtime_->objects.sensors, "");
    lv_label_set_text(runtime_->objects.timing, "");
    lv_label_set_text(runtime_->objects.terrain, "");
    const std::string pairing_message = snapshot.fault.empty()
                                            ? (snapshot.pairing_scan_active
                                                   ? "Scan ends in 15 seconds • tap STOP SCAN to leave"
                                                   : snapshot.pairing_scan_pending
                                                         ? "Waiting for device setup • scan starts automatically"
                                                         : "Scan finished • choose a row or tap SCAN AGAIN")
                                            : snapshot.fault;
    lv_label_set_text(runtime_->objects.safety, pairing_message.c_str());
    for (std::size_t index = 0U; index < snapshot.device_candidates.size() && index < kPairingRows; ++index) {
      const DeviceIdentity& candidate = snapshot.device_candidates[index];
      char text[128];
      const char* name = candidate.name.empty() ? "Unnamed device" : candidate.name.c_str();
      const char* role = candidate.role == PeripheralRole::kTrainer ? "TRAINER" : "HR";
      std::snprintf(text, sizeof(text), "%s  %s\n%02X:%02X:%02X:%02X:%02X:%02X  %d dBm", role, name,
                    static_cast<unsigned>(candidate.address[0]),
                    static_cast<unsigned>(candidate.address[1]),
                    static_cast<unsigned>(candidate.address[2]),
                    static_cast<unsigned>(candidate.address[3]),
                    static_cast<unsigned>(candidate.address[4]),
                    static_cast<unsigned>(candidate.address[5]),
                    static_cast<int>(candidate.rssi));
      lv_label_set_text(runtime_->objects.candidate_labels[index], text);
      lv_obj_set_state(runtime_->objects.candidate_buttons[index], LV_STATE_DISABLED, false);
      lv_obj_remove_flag(runtime_->objects.candidate_buttons[index], LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (snapshot.screen == UiScreen::kHistory) {
    lv_label_set_text(runtime_->objects.status, "History  |  saved on device");
    lv_label_set_text(runtime_->objects.workout, "Recent rides — Wi-Fi is not required");
    lv_label_set_text(runtime_->objects.target_caption, "");
    lv_label_set_text(runtime_->objects.actual_caption, "");
    lv_label_set_text(runtime_->objects.target_value, "");
    lv_label_set_text(runtime_->objects.actual_value, "");
    lv_label_set_text(runtime_->objects.sensors, "");
    lv_label_set_text(runtime_->objects.timing, "");
    lv_label_set_text(runtime_->objects.terrain, "");
    lv_label_set_text(runtime_->objects.safety,
                      snapshot.fault.empty()
                          ? (snapshot.history_entries.empty() ? "No saved rides yet"
                                                               : "Local summaries are retained offline")
                          : snapshot.fault.c_str());
    for (std::size_t index = 0U; index < snapshot.history_entries.size() && index < kHistoryRows; ++index) {
      const UiHistoryEntry& entry = snapshot.history_entries[index];
      char duration[16];
      char text[192];
      format_duration(duration, sizeof(duration), entry.summary.wall_ms);
      const char* completion = entry.summary.interrupted ? "Partial" :
                               (entry.summary.complete ? "Complete" : "Saved");
      const std::string upload_status = friendly_upload_state(entry.upload_status);
      std::snprintf(text, sizeof(text), "%s  •  %s\n%s  •  %s",
                    entry.summary.workout_name.empty() ? "Unnamed workout" : entry.summary.workout_name.c_str(),
                    duration, completion,
                    upload_status.c_str());
      lv_label_set_text(runtime_->objects.history_labels[index], text);
      lv_obj_remove_flag(runtime_->objects.history_rows[index], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_state(runtime_->objects.history_rows[index], LV_STATE_DISABLED, snapshot.busy);
    }
  }
  if (snapshot.screen == UiScreen::kHistoryDetail) {
    char duration[16];
    char summary_line[96];
    char terrain_summary[96];
    const UiHistoryEntry* detail = snapshot.history_detail.has_value() ? &*snapshot.history_detail : nullptr;
    const SessionSummary* summary = detail == nullptr ? nullptr : &detail->summary;
    format_duration(duration, sizeof(duration), summary == nullptr ? 0U : summary->wall_ms);
    std::snprintf(summary_line, sizeof(summary_line), "AVG %d W     HR %d",
                  summary == nullptr ? 0 : static_cast<int>(summary->average_power_w),
                  summary == nullptr ? 0 : static_cast<int>(summary->average_heart_rate_bpm));
    std::snprintf(terrain_summary, sizeof(terrain_summary), "%.1f km     +%.0f m virtual",
                  summary == nullptr ? 0.0 : static_cast<double>(summary->distance_cm) / 100000.0,
                  summary == nullptr ? 0.0 : static_cast<double>(summary->ascent_cm) / 100.0);
    lv_label_set_text(runtime_->objects.status, "RIDE DETAILS");
    lv_label_set_text(runtime_->objects.workout,
                      summary == nullptr || summary->workout_name.empty()
                          ? "Saved workout" : summary->workout_name.c_str());
    lv_label_set_text(runtime_->objects.target_caption, "DURATION");
    lv_label_set_text(runtime_->objects.target_value, duration);
    lv_label_set_text(runtime_->objects.actual_caption, "UPLOAD");
    lv_label_set_text(runtime_->objects.actual_value,
                      detail == nullptr ? "--" : compact_upload_state(detail->upload_status));
    lv_label_set_text(runtime_->objects.sensors, summary_line);
    lv_label_set_text(runtime_->objects.timing, "");
    lv_label_set_text(runtime_->objects.terrain, terrain_summary);
    const std::string upload_status = detail == nullptr
                                          ? snapshot.fault
                                          : friendly_upload_state(detail->upload_status);
    lv_label_set_text(runtime_->objects.safety, upload_status.c_str());
  }
  if (snapshot.screen == UiScreen::kCompletion) {
    char duration[16];
    char summary_line[96];
    char terrain_summary[96];
    const std::uint32_t wall_ms = snapshot.completion_summary.has_value()
                                      ? snapshot.completion_summary->wall_ms
                                      : snapshot.elapsed_wall_ms;
    format_duration(duration, sizeof(duration), wall_ms);
    const int average_power = snapshot.completion_summary.has_value()
                                  ? snapshot.completion_summary->average_power_w
                                  : 0;
    const int average_hr = snapshot.completion_summary.has_value()
                               ? snapshot.completion_summary->average_heart_rate_bpm
                               : 0;
    const double distance_km = snapshot.completion_summary.has_value()
                                   ? static_cast<double>(snapshot.completion_summary->distance_cm) / 100000.0
                                   : 0.0;
    const double ascent_m = snapshot.completion_summary.has_value()
                                ? static_cast<double>(snapshot.completion_summary->ascent_cm) / 100.0
                                : 0.0;
    std::snprintf(summary_line, sizeof(summary_line), "AVG %d W     HR %d", average_power, average_hr);
    std::snprintf(terrain_summary, sizeof(terrain_summary), "%.1f km     +%.0f m virtual",
                  distance_km, ascent_m);
    lv_label_set_text(runtime_->objects.status, "RIDE SAVED");
    lv_label_set_text(runtime_->objects.workout,
                      snapshot.workout_name.empty() ? "Workout complete" : snapshot.workout_name.c_str());
    lv_label_set_text(runtime_->objects.target_caption, "DURATION");
    lv_label_set_text(runtime_->objects.target_value, duration);
    lv_label_set_text(runtime_->objects.actual_caption, "UPLOAD");
    lv_label_set_text(runtime_->objects.actual_value,
                      compact_upload_state(snapshot.completion_upload_status));
    lv_label_set_text(runtime_->objects.sensors, summary_line);
    lv_label_set_text(runtime_->objects.timing, "");
    lv_label_set_text(runtime_->objects.terrain, terrain_summary);
    const std::string upload_message = friendly_upload_state(snapshot.completion_upload_status);
    lv_label_set_text(runtime_->objects.safety, upload_message.c_str());
  }

  for (auto* button : runtime_->objects.extra) lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  const auto extra = [&](std::size_t i, const char* label, UiCommandType command, bool enabled) {
    lv_label_set_text(runtime_->objects.extra_labels[i], label);
    runtime_->extra_contexts[i].command = command;
    lv_obj_remove_flag(runtime_->objects.extra[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_state(runtime_->objects.extra[i], LV_STATE_DISABLED, !enabled || snapshot.busy);
  };
  if (snapshot.screen == UiScreen::kHome || snapshot.screen == UiScreen::kPreview ||
      snapshot.screen == UiScreen::kSettings) {
    lv_label_set_text(runtime_->objects.timing, "");
    lv_label_set_text(runtime_->objects.terrain, "");
    if (snapshot.screen == UiScreen::kSettings) {
      static const char* names[] = {"FTP (W)", "Rider weight (kg)", "Bike weight (kg)", "Crr", "CdA (m2)"};
      const std::size_t field = std::min<std::size_t>(snapshot.profile_field, 4);
      char value[32];
      const double values[] = {static_cast<double>(snapshot.profile.ftp_watts), snapshot.profile.rider_kg,
                               snapshot.profile.bike_kg, snapshot.profile.crr, snapshot.profile.cda};
      std::snprintf(value, sizeof(value), field == 0 ? "%.0f" : field == 3 ? "%.4f" : "%.2f", values[field]);
      lv_label_set_text(runtime_->objects.workout, "Settings — Save applies all five values");
      lv_label_set_text(runtime_->objects.target_caption, names[field]);
      lv_label_set_text(runtime_->objects.target_value, value);
      lv_label_set_text(runtime_->objects.actual_caption, "");
      lv_label_set_text(runtime_->objects.actual_value, "");
      lv_label_set_text(runtime_->objects.sensors, "Local profile overrides");
      extra(0, "-", UiCommandType::kProfileDecrease, true);
      extra(1, "+", UiCommandType::kProfileIncrease, true);
      extra(2, "NEXT FIELD", UiCommandType::kProfileNext, true);
      extra(3, "SAVE", UiCommandType::kSaveProfile, true);
    } else {
      extra(0, "SETTINGS", UiCommandType::kOpenSettings, snapshot.settings_available);
      extra(1, "RETRY", UiCommandType::kRetry, snapshot.retry_available);
      extra(2, "DEVICES", UiCommandType::kOpenDevices, snapshot.devices_available);
      extra(3, "HISTORY", UiCommandType::kOpenHistory, snapshot.history_available);
    }
  }
  if (snapshot.screen == UiScreen::kError && snapshot.retry_available) {
    set_action(runtime_, "RETRY SAVE", UiCommandType::kRetry, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kSettings) {
    set_action(runtime_, "BACK / CANCEL", UiCommandType::kBack, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kPairing) {
    set_action(runtime_, snapshot.pairing_scan_active ? "STOP SCAN"
                                : snapshot.pairing_scan_pending ? "CANCEL WAIT" : "SCAN AGAIN",
               snapshot.pairing_scan_active || snapshot.pairing_scan_pending
                   ? UiCommandType::kBack
                   : UiCommandType::kOpenDevices,
               !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kStopConfirmation) {
    set_action(runtime_, "END & SAVE", UiCommandType::kEndAndSave, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kActive && snapshot.pause_available) {
    set_action(runtime_, "PAUSE", UiCommandType::kPause, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kPaused && snapshot.resume_available) {
    set_action(runtime_, "RESUME", UiCommandType::kResume, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kPreview && snapshot.start_available) {
    set_action(runtime_, "START", UiCommandType::kStart, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kHistory) {
    set_action(runtime_, "BACK", UiCommandType::kBack, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kHistoryDetail) {
    set_action(runtime_, "BACK", UiCommandType::kBack, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kCompletion) {
    set_action(runtime_, "HOME", UiCommandType::kBack, !snapshot.busy);
  } else if (snapshot.screen == UiScreen::kHome) {
    // Home navigation lives in the four-tile grid. The old primary-action
    // button duplicated DEVICES and made the screen look like two menus.
    lv_obj_add_flag(runtime_->objects.action, LV_OBJ_FLAG_HIDDEN);
  } else if (snapshot.end_save_available) {
    set_action(runtime_, "END & SAVE", UiCommandType::kEndAndSave, !snapshot.busy);
  } else {
    set_action(runtime_, "BACK", UiCommandType::kBack, false);
  }
  if (snapshot.screen == UiScreen::kStopConfirmation) {
    lv_obj_set_width(runtime_->objects.action, 168);
    lv_obj_remove_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
    runtime_->history_context.command = UiCommandType::kBack;
    lv_label_set_text(runtime_->objects.history_button_label, "CANCEL");
    lv_obj_set_state(runtime_->objects.history_button, LV_STATE_DISABLED, snapshot.busy);
  } else if (snapshot.screen == UiScreen::kPairing) {
    lv_obj_set_width(runtime_->objects.action, 168);
    lv_obj_remove_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
    runtime_->history_context.command = UiCommandType::kBack;
    lv_label_set_text(runtime_->objects.history_button_label, "BACK");
    lv_obj_set_state(runtime_->objects.history_button, LV_STATE_DISABLED, snapshot.busy);
  } else if (snapshot.screen == UiScreen::kPaused || snapshot.screen == UiScreen::kHistory) {
    lv_obj_set_width(runtime_->objects.action, 168);
    lv_obj_remove_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
    runtime_->history_context.command = snapshot.screen == UiScreen::kPaused
        ? UiCommandType::kOpenStopConfirmation : UiCommandType::kRetryUploads;
    lv_label_set_text(runtime_->objects.history_button_label,
                      snapshot.screen == UiScreen::kPaused ? "END & SAVE" : "RETRY UPLOAD");
    lv_obj_set_state(runtime_->objects.history_button, LV_STATE_DISABLED, snapshot.busy);
  } else if ((snapshot.screen == UiScreen::kHome || snapshot.screen == UiScreen::kPreview) &&
             snapshot.next_workout_available) {
    lv_obj_remove_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
    runtime_->history_context.command = UiCommandType::kNextWorkout;
    lv_label_set_text(runtime_->objects.history_button_label, "NEXT");
    lv_obj_set_state(runtime_->objects.history_button, LV_STATE_DISABLED, snapshot.busy);
  }
  runtime_->screen = snapshot.screen;
  xSemaphoreGive(runtime_->lock);
  return Status::ok();
#endif
}

std::optional<UiCommand> LvglPresenter::pop_command() {
  if (!initialized_ || runtime_ == nullptr) return std::nullopt;
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) != pdTRUE) return std::nullopt;
  const std::optional<UiCommand> command = commands_.pop();
  xSemaphoreGive(runtime_->lock);
  return command;
}

#else

Status LvglPresenter::initialize(void*, void*, board_v1::BoardV1*) {
  return Status::failure(ErrorCode::kUnsupported, "LVGL display requires ESP-IDF");
}

Status LvglPresenter::show_setup(const SetupCredentials&) {
  return Status::failure(ErrorCode::kUnsupported, "LVGL setup display requires ESP-IDF");
}

Status LvglPresenter::present(const UiSnapshot&) {
  return Status::failure(ErrorCode::kUnsupported, "LVGL display requires ESP-IDF");
}

std::optional<UiCommand> LvglPresenter::pop_command() { return std::nullopt; }

#endif

}  // namespace trainer_loop

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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#endif

namespace trainer_loop {

#if TRAINER_LOOP_LVGL_AVAILABLE && defined(ESP_PLATFORM)
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

struct UiObjects {
  lv_obj_t* extra[4]{};
  lv_obj_t* extra_labels[4]{};
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
  PairingButtonContext candidate_contexts[kPairingRows]{};
  HistoryButtonContext history_row_contexts[kHistoryRows]{};
  NavigationButtonContext history_context;
  NavigationButtonContext extra_contexts[4]{};
};

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
  lv_draw_sw_rgb565_swap(pixels, static_cast<std::uint32_t>(width * height));
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
  if (runtime == nullptr || runtime->board == nullptr) return;
  board_v1::TouchPoint point;
  const Status status = runtime->board->read_touch(&point);
  if (status && point.touched) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = static_cast<lv_coord_t>(point.x);
    data->point.y = static_cast<lv_coord_t>(point.y);
  }
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
                    const lv_font_t* font, lv_color_t color) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) return nullptr;
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  return label;
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
  lv_obj_set_style_border_color(button, lv_color_hex(0x2D6B59), selector(LV_STATE_DEFAULT));
  lv_obj_set_style_border_color(button, lv_color_hex(0x8AF5CF), selector(LV_STATE_PRESSED));
  lv_obj_set_style_radius(button, radius, LV_PART_MAIN);
}

const char* compact_link_state(const std::string& status) {
  if (status == "Settings") return "SETTINGS";
  if (status.find("reconnecting") != std::string::npos) return "RETRY";
  if (status.find("connecting") != std::string::npos) return "CONN";
  if (status.find("connected") != std::string::npos) return "OK";
  if (status.find("disconnected") != std::string::npos) return "OFF";
  if (status.find("fault") != std::string::npos) return "ERR";
  if (status.find("unavailable") != std::string::npos) return "--";
  if (status.find("offline") != std::string::npos) return "OFF";
  return "--";
}

const char* compact_upload_state(const std::string& status) {
  if (status == "UPLOADED") return "SENT";
  if (status == "PENDING" || status == "FIT_PENDING" || status == "UPLOADING") return "WAIT";
  if (status == "RETRY_WAIT") return "RETRY";
  if (status == "AUTH_BLOCKED") return "BLOCK";
  if (status == "PERMANENT_ERROR") return "ERR";
  return "--";
}

bool create_ui(LvglPresenter::Runtime* runtime) {
  lv_obj_t* root = lv_screen_active();
  if (root == nullptr) return false;
  lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

  const lv_color_t white = lv_color_hex(0xFFFFFF);
  const lv_color_t muted = lv_color_hex(0xA8B0BE);
  const lv_color_t accent = lv_color_hex(0x45D6A5);
  const lv_color_t warning = lv_color_hex(0xFFB547);

  // The enclosure's rounded bezel masks the outermost pixels. Keep the
  // connection summary inside a conservative inset and use compact states so
  // all three links remain visible at once.
  runtime->objects.status = make_label(root, 24, 6, 320, 24, &lv_font_montserrat_14, muted);
  runtime->objects.workout = make_label(root, 12, 32, 344, 34, &lv_font_montserrat_14, white);
  runtime->objects.target_caption = make_label(root, 12, 70, 160, 20, &lv_font_montserrat_14, muted);
  runtime->objects.target_value = make_label(root, 12, 88, 160, 58, &lv_font_montserrat_48, white);
  runtime->objects.actual_caption = make_label(root, 196, 70, 160, 20, &lv_font_montserrat_14, muted);
  runtime->objects.actual_value = make_label(root, 196, 88, 160, 58, &lv_font_montserrat_48, accent);
  runtime->objects.sensors = make_label(root, 12, 154, 344, 40, &lv_font_montserrat_24, white);
  runtime->objects.timing = make_label(root, 12, 202, 344, 38, &lv_font_montserrat_24, white);
  runtime->objects.terrain = make_label(root, 12, 248, 344, 68, &lv_font_montserrat_24, white);
  runtime->objects.safety = make_label(root, 12, 320, 344, 38, &lv_font_montserrat_14, warning);
  runtime->objects.setup_title = make_label(root, 12, 8, 344, 32, &lv_font_montserrat_24, white);
  runtime->objects.setup_instruction = make_label(root, 12, 42, 344, 22, &lv_font_montserrat_14, muted);
  runtime->objects.setup_ssid = make_label(root, 12, 76, 344, 34, &lv_font_montserrat_24, white);
  runtime->objects.setup_password = make_label(root, 12, 116, 344, 34, &lv_font_montserrat_24, accent);
  runtime->objects.setup_url = make_label(root, 12, 156, 344, 34, &lv_font_montserrat_24, white);
  runtime->objects.setup_note = make_label(root, 12, 208, 344, 70, &lv_font_montserrat_14, muted);
  if (runtime->objects.status == nullptr || runtime->objects.workout == nullptr ||
      runtime->objects.target_caption == nullptr || runtime->objects.target_value == nullptr ||
      runtime->objects.actual_caption == nullptr || runtime->objects.actual_value == nullptr ||
      runtime->objects.sensors == nullptr || runtime->objects.timing == nullptr ||
      runtime->objects.terrain == nullptr || runtime->objects.safety == nullptr ||
      runtime->objects.setup_title == nullptr || runtime->objects.setup_instruction == nullptr ||
      runtime->objects.setup_ssid == nullptr || runtime->objects.setup_password == nullptr ||
      runtime->objects.setup_url == nullptr || runtime->objects.setup_note == nullptr) {
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
    lv_obj_set_pos(button, 12 + static_cast<int>(i % 2) * 176, 202 + static_cast<int>(i / 2) * 56);
    lv_obj_set_size(button, 168, 48);
    style_button(button, lv_color_hex(0x183C32), lv_color_hex(0x45D6A5),
                 lv_color_hex(0x252A30), 12);
    runtime->extra_contexts[i].runtime = runtime;
    lv_obj_add_event_cb(button, navigation_callback, LV_EVENT_CLICKED, &runtime->extra_contexts[i]);
    runtime->objects.extra_labels[i] = lv_label_create(button);
    if (!runtime->objects.extra_labels[i]) return false;
    lv_obj_center(runtime->objects.extra_labels[i]);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  }

  runtime->objects.action = lv_button_create(root);
  if (runtime->objects.action == nullptr) return false;
  lv_obj_set_pos(runtime->objects.action, 12, 370);
  lv_obj_set_size(runtime->objects.action, 168, 64);
  style_button(runtime->objects.action, lv_color_hex(0x183C32), lv_color_hex(0x45D6A5),
               lv_color_hex(0x252A30));
  lv_obj_add_event_cb(runtime->objects.action, action_callback, LV_EVENT_CLICKED, runtime);
  runtime->objects.action_label = lv_label_create(runtime->objects.action);
  if (runtime->objects.action_label == nullptr) return false;
  lv_obj_set_style_text_font(runtime->objects.action_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(runtime->objects.action_label, white, LV_PART_MAIN);
  lv_obj_center(runtime->objects.action_label);

  runtime->objects.history_button = lv_button_create(root);
  if (runtime->objects.history_button == nullptr) return false;
  lv_obj_set_pos(runtime->objects.history_button, 188, 370);
  lv_obj_set_size(runtime->objects.history_button, 168, 64);
  style_button(runtime->objects.history_button, lv_color_hex(0x183C32), lv_color_hex(0x45D6A5),
               lv_color_hex(0x252A30));
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
    lv_obj_set_pos(runtime->objects.candidate_buttons[index], 12, 104 + static_cast<int>(index) * 52);
    lv_obj_set_size(runtime->objects.candidate_buttons[index], 344, 52);
    style_button(runtime->objects.candidate_buttons[index], lv_color_hex(0x183C32),
                 lv_color_hex(0x45D6A5), lv_color_hex(0x252A30), 12);
    lv_obj_add_flag(runtime->objects.candidate_buttons[index], LV_OBJ_FLAG_HIDDEN);
    runtime->candidate_contexts[index].runtime = runtime;
    runtime->candidate_contexts[index].index = index;
    lv_obj_add_event_cb(runtime->objects.candidate_buttons[index], candidate_callback,
                        LV_EVENT_CLICKED, &runtime->candidate_contexts[index]);
    runtime->objects.candidate_labels[index] = lv_label_create(runtime->objects.candidate_buttons[index]);
    if (runtime->objects.candidate_labels[index] == nullptr) return false;
    lv_obj_set_style_text_font(runtime->objects.candidate_labels[index], &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(runtime->objects.candidate_labels[index], white, LV_PART_MAIN);
    lv_label_set_long_mode(runtime->objects.candidate_labels[index], LV_LABEL_LONG_CLIP);
    lv_obj_set_width(runtime->objects.candidate_labels[index], 320);
    lv_obj_set_style_pad_left(runtime->objects.candidate_labels[index], 6, LV_PART_MAIN);
    lv_obj_center(runtime->objects.candidate_labels[index]);
  }

  for (std::size_t index = 0U; index < kHistoryRows; ++index) {
    runtime->objects.history_rows[index] = lv_button_create(root);
    if (runtime->objects.history_rows[index] == nullptr) return false;
    lv_obj_set_pos(runtime->objects.history_rows[index], 12, 88 + static_cast<int>(index) * 58);
    lv_obj_set_size(runtime->objects.history_rows[index], 344, 52);
    style_button(runtime->objects.history_rows[index], lv_color_hex(0x183C32), lv_color_hex(0x45D6A5),
                 lv_color_hex(0x252A30), 8);
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
  return true;
}

void set_action(LvglPresenter::Runtime* runtime, const char* text,
                UiCommandType command, bool enabled) {
  runtime->action_command = command;
  lv_label_set_text(runtime->objects.action_label, text);
  lv_obj_set_state(runtime->objects.action, LV_STATE_DISABLED, !enabled);
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
  if (display_panel == nullptr || display_io == nullptr || board == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "LVGL requires an initialized V1 display");
  }

  auto* runtime = new Runtime();
  runtime->panel = static_cast<esp_lcd_panel_handle_t>(display_panel);
  runtime->panel_io = static_cast<esp_lcd_panel_io_handle_t>(display_io);
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
  lv_obj_add_flag(runtime_->objects.action, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
  for (std::size_t index = 0U; index < kPairingRows; ++index) {
    lv_obj_add_flag(runtime_->objects.candidate_buttons[index], LV_OBJ_FLAG_HIDDEN);
  }
  for (std::size_t index = 0U; index < kHistoryRows; ++index) {
    lv_obj_add_flag(runtime_->objects.history_rows[index], LV_OBJ_FLAG_HIDDEN);
  }

  for (auto* button : runtime_->objects.extra) lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);

  char ssid[64];
  char password[80];
  char url[96];
  std::snprintf(ssid, sizeof(ssid), "SSID  %s", credentials.ssid.c_str());
  std::snprintf(password, sizeof(password), "PASS  %s", credentials.passphrase.c_str());
  std::snprintf(url, sizeof(url), "OPEN  %s", credentials.url.c_str());
  lv_label_set_text(runtime_->objects.setup_title, "Set up TrainerLoop");
  lv_label_set_text(runtime_->objects.setup_instruction, "Join this temporary Wi-Fi network");
  lv_label_set_text(runtime_->objects.setup_ssid, ssid);
  lv_label_set_text(runtime_->objects.setup_password, password);
  lv_label_set_text(runtime_->objects.setup_url, url);
  lv_label_set_text(runtime_->objects.setup_note,
                    "Open the address in your browser.\n"
                    "The protected setup network expires after 10 minutes.\n"
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

Status LvglPresenter::present(const UiSnapshot& snapshot) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "LVGL presenter is not initialized");
  }
  if (xSemaphoreTake(runtime_->lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
    return Status::failure(ErrorCode::kTimeout, "LVGL update lock timed out");
  }

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
                compact_link_state(snapshot.trainer_status),
                compact_link_state(snapshot.heart_rate_status),
                compact_link_state(snapshot.wifi_status));
  std::snprintf(workout, sizeof(workout), "%s%s%s",
                snapshot.workout_name.empty() ? "No workout selected" : snapshot.workout_name.c_str(),
                snapshot.interval_status.empty() ? "" : "  |  ",
                snapshot.interval_status.empty() ? "" : snapshot.interval_status.c_str());
  const char* safety_text = snapshot.safety_message.empty() ? snapshot.fault.c_str() : snapshot.safety_message.c_str();
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

  if (snapshot.screen == UiScreen::kHome || snapshot.screen == UiScreen::kPreview) {
    char duration[16];
    char ftp[24];
    char today[192];
    format_duration(duration, sizeof(duration), snapshot.workout_duration_ms);
    std::snprintf(ftp, sizeof(ftp), "%d W", snapshot.profile.ftp_watts);
    std::snprintf(today, sizeof(today), "%s%s",
                  snapshot.screen == UiScreen::kPreview ? "Preview  |  " : "Today  |  ",
                  snapshot.workout_name.empty() ? "No workout selected" : snapshot.workout_name.c_str());
    lv_label_set_text(runtime_->objects.workout, today);
    lv_label_set_text(runtime_->objects.target_caption, "DURATION");
    lv_label_set_text(runtime_->objects.target_value,
                      snapshot.workout_name.empty() ? "--:--" : duration);
    lv_label_set_text(runtime_->objects.actual_caption, "FTP");
    lv_label_set_text(runtime_->objects.actual_value, ftp);
    char readiness[96];
    std::snprintf(readiness, sizeof(readiness), "Trainer %s   HR %s",
                  compact_link_state(snapshot.trainer_status),
                  compact_link_state(snapshot.heart_rate_status));
    lv_label_set_text(runtime_->objects.sensors, readiness);
  }
  if (snapshot.screen == UiScreen::kPaused) {
    lv_label_set_text(runtime_->objects.status, "WORKOUT PAUSED");
    lv_label_set_text(runtime_->objects.workout, "Workout paused");
    lv_label_set_text(runtime_->objects.safety,
                      snapshot.safety_message.empty() ? "Trainer pause confirmed" : snapshot.safety_message.c_str());
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
                                                   : "Scan finished • choose a row or tap SCAN AGAIN")
                                            : snapshot.fault;
    lv_label_set_text(runtime_->objects.safety, pairing_message.c_str());
    for (std::size_t index = 0U; index < snapshot.device_candidates.size() && index < kPairingRows; ++index) {
      const DeviceIdentity& candidate = snapshot.device_candidates[index];
      char text[128];
      const char* name = candidate.name.empty() ? "Unnamed device" : candidate.name.c_str();
      const char* role = candidate.role == PeripheralRole::kTrainer ? "TRAINER" : "HR";
      std::snprintf(text, sizeof(text), "%s  [%02X:%02X]  %d dBm  %s", name,
                    static_cast<unsigned>(candidate.address[4]),
                    static_cast<unsigned>(candidate.address[5]),
                    static_cast<int>(candidate.rssi),
                    role);
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
      std::snprintf(text, sizeof(text), "%s  •  %s\n%s  •  %s",
                    entry.summary.workout_name.empty() ? "Unnamed workout" : entry.summary.workout_name.c_str(),
                    duration, completion,
                    entry.upload_status.empty() ? "Saved locally" : entry.upload_status.c_str());
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
    lv_label_set_text(runtime_->objects.safety,
                      detail == nullptr ? snapshot.fault.c_str() : detail->upload_status.c_str());
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
    const std::string upload_message = snapshot.completion_upload_status.empty()
                                           ? "Saved on device"
                                           : snapshot.completion_upload_status;
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
    set_action(runtime_, snapshot.pairing_scan_active ? "STOP SCAN" : "SCAN AGAIN",
               snapshot.pairing_scan_active ? UiCommandType::kBack : UiCommandType::kOpenDevices,
               !snapshot.busy);
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
  if (snapshot.screen == UiScreen::kPairing) {
    lv_obj_set_width(runtime_->objects.action, 168);
    lv_obj_remove_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
    runtime_->history_context.command = UiCommandType::kBack;
    lv_label_set_text(runtime_->objects.history_button_label, "BACK");
    lv_obj_set_state(runtime_->objects.history_button, LV_STATE_DISABLED, snapshot.busy);
  } else if (snapshot.screen == UiScreen::kPaused || snapshot.screen == UiScreen::kHistory) {
    lv_obj_set_width(runtime_->objects.action, 168);
    lv_obj_remove_flag(runtime_->objects.history_button, LV_OBJ_FLAG_HIDDEN);
    runtime_->history_context.command = snapshot.screen == UiScreen::kPaused
        ? UiCommandType::kEndAndSave : UiCommandType::kRetry;
    lv_label_set_text(runtime_->objects.history_button_label,
                      snapshot.screen == UiScreen::kPaused ? "END & SAVE" : "RETRY");
    lv_obj_set_state(runtime_->objects.history_button, LV_STATE_DISABLED, snapshot.busy);
  }
  runtime_->screen = snapshot.screen;
  xSemaphoreGive(runtime_->lock);
  return Status::ok();
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

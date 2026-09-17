#include "ble_nimble.h"

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

extern "C" void ble_store_config_init(void);

namespace trainer_loop {

constexpr char kLogTag[] = "trainer_ble";
constexpr std::size_t kMaxScanCandidates = 16U;
constexpr std::size_t kMaxServices = 6U;
constexpr std::size_t kMaxCharacteristics = 32U;
constexpr std::size_t kMaxSubscriptions = 4U;
constexpr std::size_t kNotificationQueueCapacity = 16U;
constexpr std::uint16_t kNoHandle = 0xffffU;
constexpr std::uint32_t kScanDurationMs = 15000U;
constexpr std::int32_t kConnectDurationMs = 30000;

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

std::size_t role_index(PeripheralRole role) {
  return role == PeripheralRole::kTrainer ? 0U : 1U;
}

bool is_uuid(const ble_uuid_t* actual, std::uint16_t expected) {
  if (actual == nullptr) return false;
  const ble_uuid16_t expected_uuid = BLE_UUID16_INIT(expected);
  return ble_uuid_cmp(actual, &expected_uuid.u) == 0;
}

bool has_advertised_uuid(const ble_hs_adv_fields& fields, std::uint16_t expected) {
  for (std::uint8_t index = 0U; index < fields.num_uuids16; ++index) {
    if (fields.uuids16[index].value == expected) return true;
  }
  return false;
}

bool same_ble_address(const DeviceIdentity& identity, const ble_addr_t& address) {
  return identity.address_type == address.type &&
         std::memcmp(identity.address.data(), address.val, identity.address.size()) == 0;
}

bool selected_peer_matches(const DeviceIdentity& identity, const ble_gap_conn_desc& descriptor) {
  // peer_ota_addr is the address used for this connection.  The identity
  // address is also checked because a bonded/resolvable peer may expose a
  // different OTA address after resolution.
  return same_ble_address(identity, descriptor.peer_ota_addr) ||
         same_ble_address(identity, descriptor.peer_id_addr);
}

enum class ServiceKind : std::uint8_t {
  kFitnessMachine,
  kHeartRate,
  kDeviceInformation,
  kBattery,
};

enum class CharacteristicKind : std::uint8_t {
  kIndoorBikeData,
  kFitnessMachineFeature,
  kSupportedPowerRange,
  kFitnessMachineControlPoint,
  kFitnessMachineStatus,
  kHeartRateMeasurement,
  kManufacturerName,
  kModelNumber,
  kFirmwareRevision,
  kBatteryLevel,
};

enum class ReadKind : std::uint8_t {
  kFeature,
  kPowerRange,
};

struct ServiceRange {
  ServiceKind kind = ServiceKind::kFitnessMachine;
  std::uint16_t start_handle = 0U;
  std::uint16_t end_handle = 0U;
};

struct CharacteristicInfo {
  CharacteristicKind kind = CharacteristicKind::kIndoorBikeData;
  ServiceKind service = ServiceKind::kFitnessMachine;
  std::uint16_t definition_handle = 0U;
  std::uint16_t value_handle = 0U;
  std::uint16_t descriptor_end_handle = 0U;
  std::uint16_t cccd_handle = kNoHandle;
  std::uint8_t properties = 0U;
};

struct ReadRequest {
  ReadKind kind = ReadKind::kFeature;
  std::uint16_t value_handle = 0U;
};

struct SubscriptionRequest {
  CharacteristicKind kind = CharacteristicKind::kIndoorBikeData;
  std::uint16_t cccd_handle = 0U;
  bool indication = false;
  bool required = true;
};

enum class DiscoveryStage : std::uint8_t {
  kIdle,
  kServices,
  kCharacteristics,
  kDescriptors,
  kReads,
  kSubscriptions,
  kReady,
  kFailed,
};

bool is_relevant_service(PeripheralRole role, ServiceKind kind) {
  if (role == PeripheralRole::kHeartRate) return kind == ServiceKind::kHeartRate;
  return true;
}

bool is_relevant_characteristic(PeripheralRole role, ServiceKind service,
                                CharacteristicKind kind) {
  if (role == PeripheralRole::kHeartRate) {
    return service == ServiceKind::kHeartRate &&
           kind == CharacteristicKind::kHeartRateMeasurement;
  }
  return true;
}

bool has_property(const CharacteristicInfo& characteristic, std::uint32_t property) {
  return (static_cast<std::uint32_t>(characteristic.properties) & property) != 0U;
}

struct NimbleRuntime;

struct NimbleRuntimeLink {
  NimbleRuntime* runtime = nullptr;
  BleConnectionManager* connections = nullptr;
  PeripheralRole role = PeripheralRole::kTrainer;
  bool connecting = false;
  bool ignore_disconnect = false;
  std::uint16_t conn_handle = kNoHandle;
  std::uint32_t generation = 0U;
  DiscoveryStage discovery = DiscoveryStage::kIdle;

  std::array<ServiceRange, kMaxServices> services{};
  std::size_t service_count = 0U;
  std::size_t next_characteristic_service = 0U;
  ServiceKind current_service = ServiceKind::kFitnessMachine;

  std::array<CharacteristicInfo, kMaxCharacteristics> characteristics{};
  std::size_t characteristic_count = 0U;
  std::size_t next_descriptor = 0U;

  std::array<ReadRequest, 2U> reads{};
  std::size_t read_count = 0U;
  std::size_t next_read = 0U;

  std::array<SubscriptionRequest, kMaxSubscriptions> subscriptions{};
  std::size_t subscription_count = 0U;
  std::size_t next_subscription = 0U;

  TrainerCapabilities capabilities{};
  ControlTransactionGate control_gate;
  std::array<std::uint8_t, kBleNotificationMaxBytes> read_buffer{};
  bool current_subscription_pending = false;
  SubscriptionRequest current_subscription{};

  void reset_profile() {
    discovery = DiscoveryStage::kIdle;
    service_count = 0U;
    next_characteristic_service = 0U;
    characteristic_count = 0U;
    next_descriptor = 0U;
    read_count = 0U;
    next_read = 0U;
    subscription_count = 0U;
    next_subscription = 0U;
    capabilities = TrainerCapabilities{};
    current_subscription_pending = false;
    current_subscription = SubscriptionRequest{};
    control_gate.invalidate();
  }
};

struct NimbleRuntime {
  NimbleCentral* owner = nullptr;
  BleConnectionManager* connections = nullptr;
  bool host_ready = false;
  bool scan_complete = false;
  std::array<DeviceIdentity, kMaxScanCandidates> candidates{};
  std::size_t candidate_count = 0U;

  std::array<BleNotification, kNotificationQueueCapacity> notifications{};
  std::size_t notification_head = 0U;
  std::size_t notification_tail = 0U;
  std::size_t notification_count = 0U;
  std::uint32_t dropped_notifications = 0U;
  portMUX_TYPE notification_lock = portMUX_INITIALIZER_UNLOCKED;

  std::array<NimbleRuntimeLink, 2U> links{};

  NimbleRuntime(NimbleCentral* central, BleConnectionManager* manager)
      : owner(central), connections(manager) {
    links[0].runtime = this;
    links[0].connections = manager;
    links[0].role = PeripheralRole::kTrainer;
    links[1].runtime = this;
    links[1].connections = manager;
    links[1].role = PeripheralRole::kHeartRate;
  }
};

NimbleRuntime* g_runtime = nullptr;

NimbleRuntimeLink& link_for(NimbleRuntime* runtime, PeripheralRole role) {
  return runtime->links[role_index(role)];
}

const NimbleRuntimeLink& link_for(const NimbleRuntime* runtime, PeripheralRole role) {
  return runtime->links[role_index(role)];
}

ServiceKind service_kind(const ble_uuid_any_t& uuid, bool* known) {
  if (is_uuid(&uuid.u, 0x1826U)) {
    *known = true;
    return ServiceKind::kFitnessMachine;
  }
  if (is_uuid(&uuid.u, 0x180dU)) {
    *known = true;
    return ServiceKind::kHeartRate;
  }
  if (is_uuid(&uuid.u, 0x180aU)) {
    *known = true;
    return ServiceKind::kDeviceInformation;
  }
  if (is_uuid(&uuid.u, 0x180fU)) {
    *known = true;
    return ServiceKind::kBattery;
  }
  *known = false;
  return ServiceKind::kFitnessMachine;
}

bool characteristic_kind(const ble_uuid_any_t& uuid, ServiceKind service,
                         CharacteristicKind* kind) {
  if (service == ServiceKind::kFitnessMachine) {
    if (is_uuid(&uuid.u, 0x2ad2U)) *kind = CharacteristicKind::kIndoorBikeData;
    else if (is_uuid(&uuid.u, 0x2accU)) *kind = CharacteristicKind::kFitnessMachineFeature;
    else if (is_uuid(&uuid.u, 0x2ad8U)) *kind = CharacteristicKind::kSupportedPowerRange;
    else if (is_uuid(&uuid.u, 0x2ad9U)) *kind = CharacteristicKind::kFitnessMachineControlPoint;
    else if (is_uuid(&uuid.u, 0x2adaU)) *kind = CharacteristicKind::kFitnessMachineStatus;
    else return false;
    return true;
  }
  if (service == ServiceKind::kHeartRate && is_uuid(&uuid.u, 0x2a37U)) {
    *kind = CharacteristicKind::kHeartRateMeasurement;
    return true;
  }
  if (service == ServiceKind::kDeviceInformation) {
    if (is_uuid(&uuid.u, 0x2a29U)) *kind = CharacteristicKind::kManufacturerName;
    else if (is_uuid(&uuid.u, 0x2a24U)) *kind = CharacteristicKind::kModelNumber;
    else if (is_uuid(&uuid.u, 0x2a26U)) *kind = CharacteristicKind::kFirmwareRevision;
    else return false;
    return true;
  }
  if (service == ServiceKind::kBattery && is_uuid(&uuid.u, 0x2a19U)) {
    *kind = CharacteristicKind::kBatteryLevel;
    return true;
  }
  return false;
}

CharacteristicInfo* find_characteristic(NimbleRuntimeLink* link, CharacteristicKind kind) {
  for (std::size_t index = 0U; index < link->characteristic_count; ++index) {
    if (link->characteristics[index].kind == kind) return &link->characteristics[index];
  }
  return nullptr;
}

const CharacteristicInfo* find_characteristic(const NimbleRuntimeLink* link,
                                              CharacteristicKind kind) {
  for (std::size_t index = 0U; index < link->characteristic_count; ++index) {
    if (link->characteristics[index].kind == kind) return &link->characteristics[index];
  }
  return nullptr;
}

void enqueue_notification(NimbleRuntimeLink* link, BleNotification notification) {
  NimbleRuntime* runtime = link->runtime;
  portENTER_CRITICAL(&runtime->notification_lock);
  if (runtime->notification_count == kNotificationQueueCapacity) {
    // Drop the newest packet rather than evicting an older control response.
    if (runtime->dropped_notifications < UINT32_MAX) ++runtime->dropped_notifications;
    portEXIT_CRITICAL(&runtime->notification_lock);
    return;
  }
  runtime->notifications[runtime->notification_tail] = notification;
  runtime->notification_tail = (runtime->notification_tail + 1U) % kNotificationQueueCapacity;
  ++runtime->notification_count;
  portEXIT_CRITICAL(&runtime->notification_lock);
}

void terminate_after_failure(NimbleRuntimeLink* link, const char* reason) {
  link->discovery = DiscoveryStage::kFailed;
  link->control_gate.invalidate();
  ESP_LOGW(kLogTag, "BLE profile rejected (%s)", reason);
  if (link->conn_handle != kNoHandle) {
    (void)ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  }
}

void invalidate_control_and_reconnect(NimbleRuntimeLink* link, std::uint64_t now_ms,
                                      const char* reason) {
  link->control_gate.invalidate();
  if (link->connections != nullptr &&
      link->connections->state(link->role).link == LinkState::kConnected) {
    (void)link->connections->mark_control_invalid(now_ms);
  }
  ESP_LOGW(kLogTag, "FTMS control invalidated (%s)", reason);
  if (link->conn_handle != kNoHandle) {
    (void)ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  }
}

int service_discovery_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                               const ble_gatt_svc* service, void* argument);
int characteristic_discovery_callback(std::uint16_t conn_handle,
                                      const ble_gatt_error* error,
                                      const ble_gatt_chr* characteristic, void* argument);
int descriptor_discovery_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                                 std::uint16_t characteristic_value_handle,
                                 const ble_gatt_dsc* descriptor, void* argument);
int read_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                  ble_gatt_attr* attribute, void* argument);
int subscription_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                          ble_gatt_attr* attribute, void* argument);
int control_write_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                           ble_gatt_attr* attribute, void* argument);
int gap_event_callback(ble_gap_event* event, void* argument);

bool add_scan_candidate(NimbleRuntime* runtime, const ble_gap_disc_desc& advertisement,
                        PeripheralRole role, const ble_hs_adv_fields& fields) {
  DeviceIdentity identity;
  std::memcpy(identity.address.data(), advertisement.addr.val, identity.address.size());
  identity.address_type = advertisement.addr.type;
  identity.rssi = advertisement.rssi;
  identity.role = role;
  if (fields.name != nullptr && fields.name_len != 0U) {
    const std::size_t name_length = std::min<std::size_t>(fields.name_len, 31U);
    identity.name.assign(reinterpret_cast<const char*>(fields.name), name_length);
  }

  for (std::size_t index = 0U; index < runtime->candidate_count; ++index) {
    if (!same_identity(runtime->candidates[index], identity)) continue;
    runtime->candidates[index].rssi = identity.rssi;
    if (!identity.name.empty()) runtime->candidates[index].name = std::move(identity.name);
    return true;
  }
  if (runtime->candidate_count >= kMaxScanCandidates) return false;
  runtime->candidates[runtime->candidate_count++] = std::move(identity);
  return true;
}

void finish_scan_from_callback(NimbleRuntime* runtime) {
  runtime->scan_complete = true;
}

int scan_callback(ble_gap_event* event, void* argument) {
  auto* runtime = static_cast<NimbleRuntime*>(argument);
  if (runtime == nullptr || event == nullptr) return 0;
  if (event->type == BLE_GAP_EVENT_DISC) {
    ble_hs_adv_fields fields{};
    if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
      return 0;
    }
    if (has_advertised_uuid(fields, 0x1826U)) {
      (void)add_scan_candidate(runtime, event->disc, PeripheralRole::kTrainer, fields);
    }
    if (has_advertised_uuid(fields, 0x180dU)) {
      (void)add_scan_candidate(runtime, event->disc, PeripheralRole::kHeartRate, fields);
    }
    return 0;
  }
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) finish_scan_from_callback(runtime);
  return 0;
}

void host_reset_callback(int reason) {
  if (g_runtime != nullptr) g_runtime->host_ready = false;
  ESP_LOGW(kLogTag, "NimBLE host reset (%d)", reason);
}

void host_sync_callback() {
  if (g_runtime != nullptr) g_runtime->host_ready = true;
}

void host_task(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void start_characteristic_discovery(NimbleRuntimeLink* link);
void start_descriptor_discovery(NimbleRuntimeLink* link);
void start_read_sequence(NimbleRuntimeLink* link);
void start_subscription_sequence(NimbleRuntimeLink* link);

void start_service_discovery(NimbleRuntimeLink* link) {
  link->reset_profile();
  link->discovery = DiscoveryStage::kServices;
  const int result = ble_gattc_disc_all_svcs(link->conn_handle, service_discovery_callback, link);
  if (result != 0) terminate_after_failure(link, "service discovery could not start");
}

void start_characteristic_discovery(NimbleRuntimeLink* link) {
  while (link->next_characteristic_service < link->service_count) {
    const ServiceRange& service = link->services[link->next_characteristic_service++];
    link->current_service = service.kind;
    link->discovery = DiscoveryStage::kCharacteristics;
    const int result = ble_gattc_disc_all_chrs(link->conn_handle, service.start_handle,
                                               service.end_handle,
                                               characteristic_discovery_callback, link);
    if (result != 0) terminate_after_failure(link, "characteristic discovery could not start");
    return;
  }
  start_descriptor_discovery(link);
}

void calculate_descriptor_end_handles(NimbleRuntimeLink* link) {
  for (std::size_t index = 0U; index < link->characteristic_count; ++index) {
    CharacteristicInfo& characteristic = link->characteristics[index];
    std::uint16_t service_end = characteristic.definition_handle;
    for (std::size_t service_index = 0U; service_index < link->service_count; ++service_index) {
      const ServiceRange& service = link->services[service_index];
      if (service.kind == characteristic.service && service.start_handle <= characteristic.definition_handle &&
          service.end_handle >= characteristic.definition_handle) {
        service_end = service.end_handle;
        break;
      }
    }
    characteristic.descriptor_end_handle = service_end;
    for (std::size_t other = 0U; other < link->characteristic_count; ++other) {
      const CharacteristicInfo& next = link->characteristics[other];
      if (next.service == characteristic.service && next.definition_handle > characteristic.definition_handle &&
          next.definition_handle <= characteristic.descriptor_end_handle) {
        characteristic.descriptor_end_handle = static_cast<std::uint16_t>(next.definition_handle - 1U);
      }
    }
  }
}

bool needs_descriptor(const NimbleRuntimeLink* link, const CharacteristicInfo& characteristic) {
  switch (characteristic.kind) {
    case CharacteristicKind::kIndoorBikeData:
      return link->role == PeripheralRole::kTrainer &&
             has_property(characteristic, BLE_GATT_CHR_F_NOTIFY);
    case CharacteristicKind::kFitnessMachineControlPoint:
      return link->role == PeripheralRole::kTrainer &&
             has_property(characteristic, BLE_GATT_CHR_F_INDICATE);
    case CharacteristicKind::kFitnessMachineStatus:
      return link->role == PeripheralRole::kTrainer &&
             has_property(characteristic, BLE_GATT_CHR_F_NOTIFY);
    case CharacteristicKind::kHeartRateMeasurement:
      return has_property(characteristic, BLE_GATT_CHR_F_NOTIFY);
    default:
      return false;
  }
}

void start_descriptor_discovery(NimbleRuntimeLink* link) {
  calculate_descriptor_end_handles(link);
  link->next_descriptor = 0U;
  while (link->next_descriptor < link->characteristic_count) {
    CharacteristicInfo& characteristic = link->characteristics[link->next_descriptor++];
    if (!needs_descriptor(link, characteristic)) continue;
    link->discovery = DiscoveryStage::kDescriptors;
    const int result = ble_gattc_disc_all_dscs(link->conn_handle, characteristic.value_handle,
                                               characteristic.descriptor_end_handle,
                                               descriptor_discovery_callback, link);
    if (result != 0) terminate_after_failure(link, "descriptor discovery could not start");
    return;
  }
  start_read_sequence(link);
}

bool add_read(NimbleRuntimeLink* link, ReadKind kind, const CharacteristicInfo* characteristic) {
  if (characteristic == nullptr || link->read_count >= link->reads.size()) return false;
  link->reads[link->read_count++] = ReadRequest{kind, characteristic->value_handle};
  return true;
}

void start_read_sequence(NimbleRuntimeLink* link) {
  if (link->role == PeripheralRole::kHeartRate) {
    start_subscription_sequence(link);
    return;
  }
  const CharacteristicInfo* feature = find_characteristic(link, CharacteristicKind::kFitnessMachineFeature);
  const CharacteristicInfo* range = find_characteristic(link, CharacteristicKind::kSupportedPowerRange);
  const CharacteristicInfo* data = find_characteristic(link, CharacteristicKind::kIndoorBikeData);
  const CharacteristicInfo* control = find_characteristic(link, CharacteristicKind::kFitnessMachineControlPoint);
  if (feature == nullptr || range == nullptr || data == nullptr || control == nullptr ||
      !has_property(*feature, BLE_GATT_CHR_F_READ) || !has_property(*range, BLE_GATT_CHR_F_READ) ||
      !has_property(*data, BLE_GATT_CHR_F_NOTIFY) || !has_property(*control, BLE_GATT_CHR_F_INDICATE) ||
      data->cccd_handle == kNoHandle || control->cccd_handle == kNoHandle) {
    terminate_after_failure(link, "required FTMS characteristic is unavailable");
    return;
  }
  link->capabilities.ftms_present = true;
  link->capabilities.control_point_present = true;
  link->capabilities.indoor_bike_data_present = true;
  link->capabilities.feature_present = false;
  link->capabilities.power_range_present = false;
  link->read_count = 0U;
  link->next_read = 0U;
  (void)add_read(link, ReadKind::kFeature, feature);
  (void)add_read(link, ReadKind::kPowerRange, range);
  link->discovery = DiscoveryStage::kReads;
  const ReadRequest& request = link->reads[link->next_read];
  const int result = ble_gattc_read(link->conn_handle, request.value_handle, read_callback, link);
  if (result != 0) terminate_after_failure(link, "FTMS capability read could not start");
}

bool add_subscription(NimbleRuntimeLink* link, CharacteristicKind kind,
                      const CharacteristicInfo* characteristic, bool indication, bool required) {
  if (characteristic == nullptr || characteristic->cccd_handle == kNoHandle ||
      link->subscription_count >= link->subscriptions.size()) {
    return false;
  }
  link->subscriptions[link->subscription_count++] =
      SubscriptionRequest{kind, characteristic->cccd_handle, indication, required};
  return true;
}

void start_subscription_sequence(NimbleRuntimeLink* link) {
  link->subscription_count = 0U;
  link->next_subscription = 0U;
  if (link->role == PeripheralRole::kHeartRate) {
    const CharacteristicInfo* heart_rate =
        find_characteristic(link, CharacteristicKind::kHeartRateMeasurement);
    if (heart_rate == nullptr || !has_property(*heart_rate, BLE_GATT_CHR_F_NOTIFY) ||
        !add_subscription(link, CharacteristicKind::kHeartRateMeasurement, heart_rate, false, true)) {
      terminate_after_failure(link, "heart-rate notification is unavailable");
      return;
    }
  } else {
    const CharacteristicInfo* data = find_characteristic(link, CharacteristicKind::kIndoorBikeData);
    const CharacteristicInfo* control =
        find_characteristic(link, CharacteristicKind::kFitnessMachineControlPoint);
    if (data == nullptr || control == nullptr ||
        !add_subscription(link, CharacteristicKind::kIndoorBikeData, data, false, true) ||
        !add_subscription(link, CharacteristicKind::kFitnessMachineControlPoint, control, true, true)) {
      terminate_after_failure(link, "required FTMS subscription is unavailable");
      return;
    }
    const CharacteristicInfo* status = find_characteristic(link, CharacteristicKind::kFitnessMachineStatus);
    if (status != nullptr && status->cccd_handle != kNoHandle &&
        has_property(*status, BLE_GATT_CHR_F_NOTIFY)) {
      (void)add_subscription(link, CharacteristicKind::kFitnessMachineStatus, status, false, false);
    }
    const CharacteristicInfo* heart_rate =
        find_characteristic(link, CharacteristicKind::kHeartRateMeasurement);
    if (heart_rate != nullptr && heart_rate->cccd_handle != kNoHandle &&
        has_property(*heart_rate, BLE_GATT_CHR_F_NOTIFY)) {
      (void)add_subscription(link, CharacteristicKind::kHeartRateMeasurement, heart_rate, false, false);
    }
  }
  link->discovery = DiscoveryStage::kSubscriptions;
  if (link->subscription_count == 0U) {
    terminate_after_failure(link, "no BLE subscriptions were created");
    return;
  }
  link->current_subscription = link->subscriptions[0];
  link->current_subscription_pending = true;
  const std::uint8_t value[2] = {
      static_cast<std::uint8_t>(link->current_subscription.indication ? 0x02U : 0x01U), 0x00U};
  const int result = ble_gattc_write_flat(link->conn_handle, link->current_subscription.cccd_handle,
                                           value, sizeof(value), subscription_callback, link);
  if (result != 0) terminate_after_failure(link, "CCCD subscription could not start");
}

void start_next_subscription(NimbleRuntimeLink* link) {
  if (link->next_subscription >= link->subscription_count) {
    link->current_subscription_pending = false;
    link->discovery = DiscoveryStage::kReady;
    const Status ready = link->connections->services_ready(link->role);
    if (!ready) terminate_after_failure(link, "BLE link state rejected ready profile");
    return;
  }
  link->current_subscription = link->subscriptions[link->next_subscription];
  link->current_subscription_pending = true;
  const std::uint8_t value[2] = {
      static_cast<std::uint8_t>(link->current_subscription.indication ? 0x02U : 0x01U), 0x00U};
  const int result = ble_gattc_write_flat(link->conn_handle, link->current_subscription.cccd_handle,
                                           value, sizeof(value), subscription_callback, link);
  if (result != 0) terminate_after_failure(link, "CCCD subscription could not start");
}

int service_discovery_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                               const ble_gatt_svc* service, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || error == nullptr || link->conn_handle != conn_handle) return 0;
  if (error->status == BLE_HS_EDONE) {
    if (link->service_count == 0U) {
      terminate_after_failure(link, "peer exposes none of the supported services");
    } else {
      start_characteristic_discovery(link);
    }
    return 0;
  }
  if (error->status != 0 || service == nullptr) {
    terminate_after_failure(link, "service discovery failed");
    return 0;
  }
  bool known = false;
  const ServiceKind kind = service_kind(service->uuid, &known);
  if (!known || !is_relevant_service(link->role, kind) || link->service_count >= link->services.size()) {
    return 0;
  }
  for (std::size_t index = 0U; index < link->service_count; ++index) {
    if (link->services[index].kind == kind) return 0;
  }
  link->services[link->service_count++] = ServiceRange{kind, service->start_handle, service->end_handle};
  return 0;
}

int characteristic_discovery_callback(std::uint16_t conn_handle,
                                      const ble_gatt_error* error,
                                      const ble_gatt_chr* characteristic, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || error == nullptr || link->conn_handle != conn_handle) return 0;
  if (error->status == BLE_HS_EDONE) {
    start_characteristic_discovery(link);
    return 0;
  }
  if (error->status != 0 || characteristic == nullptr) {
    terminate_after_failure(link, "characteristic discovery failed");
    return 0;
  }
  CharacteristicKind kind;
  if (!characteristic_kind(characteristic->uuid, link->current_service, &kind) ||
      !is_relevant_characteristic(link->role, link->current_service, kind)) {
    return 0;
  }
  if (link->characteristic_count >= link->characteristics.size()) {
    terminate_after_failure(link, "characteristic table is full");
    return 0;
  }
  for (std::size_t index = 0U; index < link->characteristic_count; ++index) {
    if (link->characteristics[index].value_handle == characteristic->val_handle) return 0;
  }
  link->characteristics[link->characteristic_count++] = CharacteristicInfo{
      kind, link->current_service, characteristic->def_handle, characteristic->val_handle,
      characteristic->def_handle, kNoHandle, characteristic->properties};
  return 0;
}

int descriptor_discovery_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                                 std::uint16_t characteristic_value_handle,
                                 const ble_gatt_dsc* descriptor, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || error == nullptr || link->conn_handle != conn_handle) return 0;
  if (error->status == BLE_HS_EDONE) {
    start_descriptor_discovery(link);
    return 0;
  }
  if (error->status != 0 || descriptor == nullptr) {
    terminate_after_failure(link, "descriptor discovery failed");
    return 0;
  }
  if (is_uuid(&descriptor->uuid.u, BLE_GATT_DSC_CLT_CFG_UUID16)) {
    for (std::size_t index = 0U; index < link->characteristic_count; ++index) {
      CharacteristicInfo& characteristic = link->characteristics[index];
      if (characteristic.value_handle == characteristic_value_handle) {
        characteristic.cccd_handle = descriptor->handle;
        break;
      }
    }
  }
  return 0;
}

int copy_attribute(const ble_gatt_attr* attribute, std::array<std::uint8_t, kBleNotificationMaxBytes>* buffer,
                   std::size_t* length) {
  if (attribute == nullptr || attribute->om == nullptr || buffer == nullptr || length == nullptr) return -1;
  const std::size_t packet_length = static_cast<std::size_t>(OS_MBUF_PKTLEN(attribute->om));
  if (packet_length > buffer->size()) return -1;
  if (os_mbuf_copydata(attribute->om, 0, static_cast<int>(packet_length), buffer->data()) != 0) return -1;
  *length = packet_length;
  return 0;
}

int read_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                  ble_gatt_attr* attribute, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || error == nullptr || link->conn_handle != conn_handle) return 0;
  if (error->status != 0 || link->next_read >= link->read_count) {
    terminate_after_failure(link, "FTMS capability read failed");
    return 0;
  }
  std::size_t length = 0U;
  if (copy_attribute(attribute, &link->read_buffer, &length) != 0) {
    terminate_after_failure(link, "FTMS capability payload is invalid");
    return 0;
  }
  const ReadKind kind = link->reads[link->next_read].kind;
  if (kind == ReadKind::kFeature) {
    if (length < 4U) {
      terminate_after_failure(link, "FTMS feature payload is truncated");
      return 0;
    }
    link->capabilities.feature_present = true;
  } else {
    const auto parsed = parse_supported_power_range(link->read_buffer.data(), length);
    if (!parsed) {
      terminate_after_failure(link, "FTMS power range is invalid");
      return 0;
    }
    link->capabilities = *parsed.value;
    link->capabilities.ftms_present = true;
    link->capabilities.control_point_present = true;
    link->capabilities.indoor_bike_data_present = true;
    link->capabilities.feature_present = true;
  }
  ++link->next_read;
  if (link->next_read < link->read_count) {
    const ReadRequest& request = link->reads[link->next_read];
    const int result = ble_gattc_read(link->conn_handle, request.value_handle, read_callback, link);
    if (result != 0) terminate_after_failure(link, "next FTMS capability read could not start");
  } else {
    start_subscription_sequence(link);
  }
  return 0;
}

int subscription_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                          ble_gatt_attr*, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || error == nullptr || link->conn_handle != conn_handle) return 0;
  if (error->status != 0) {
    if (link->current_subscription.required) {
      terminate_after_failure(link, "required CCCD write failed");
      return 0;
    }
    ++link->next_subscription;
    link->current_subscription_pending = false;
    start_next_subscription(link);
    return 0;
  }
  ++link->next_subscription;
  link->current_subscription_pending = false;
  start_next_subscription(link);
  return 0;
}

bool notification_kind_for_handle(const NimbleRuntimeLink* link, std::uint16_t handle,
                                  BleNotificationKind* kind) {
  const CharacteristicInfo* characteristic = nullptr;
  for (std::size_t index = 0U; index < link->characteristic_count; ++index) {
    if (link->characteristics[index].value_handle == handle) {
      characteristic = &link->characteristics[index];
      break;
    }
  }
  if (characteristic == nullptr) return false;
  switch (characteristic->kind) {
    case CharacteristicKind::kIndoorBikeData:
      *kind = BleNotificationKind::kIndoorBikeData;
      return true;
    case CharacteristicKind::kFitnessMachineStatus:
      *kind = BleNotificationKind::kFitnessMachineStatus;
      return true;
    case CharacteristicKind::kFitnessMachineControlPoint:
      *kind = BleNotificationKind::kFitnessMachineControlPoint;
      return true;
    case CharacteristicKind::kHeartRateMeasurement:
      *kind = BleNotificationKind::kHeartRateMeasurement;
      return true;
    default:
      return false;
  }
}

int notify_received(ble_gap_event* event, NimbleRuntimeLink* link) {
  if (event == nullptr || link == nullptr || event->notify_rx.om == nullptr) return 0;
  BleNotificationKind kind;
  if (!notification_kind_for_handle(link, event->notify_rx.attr_handle, &kind)) return 0;
  BleNotification notification;
  notification.role = link->role;
  notification.kind = kind;
  notification.generation = link->generation;
  notification.attribute_handle = event->notify_rx.attr_handle;
  notification.indication = event->notify_rx.indication != 0U;
  notification.timestamp_ms = monotonic_ms();
  std::size_t length = 0U;
  ble_gatt_attr attribute{};
  attribute.om = event->notify_rx.om;
  if (copy_attribute(&attribute, &notification.payload, &length) != 0) return 0;
  notification.length = static_cast<std::uint8_t>(length);
  enqueue_notification(link, notification);

  if (kind == BleNotificationKind::kFitnessMachineControlPoint) {
    const bool was_pending = link->control_gate.pending();
    const auto acknowledged = link->control_gate.acknowledge(
        notification.payload.data(), length, link->generation, notification.timestamp_ms);
    // A valid negative FTMS response consumes the pending transaction and
    // invalidates control.  A malformed, stale, or wrong-opcode response does
    // not complete a newer command and is intentionally left for diagnostics.
    if (was_pending && !acknowledged && !link->control_gate.pending()) {
      invalidate_control_and_reconnect(link, notification.timestamp_ms, "trainer rejected command");
    }
  }
  return 0;
}

int control_write_callback(std::uint16_t conn_handle, const ble_gatt_error* error,
                           ble_gatt_attr*, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || error == nullptr || link->conn_handle != conn_handle) return 0;
  if (error->status != 0) {
    invalidate_control_and_reconnect(link, monotonic_ms(), "ATT write failed");
  }
  // ATT success is deliberately not forwarded to ControlTransactionGate.
  // The FTMS indication is the only command acknowledgement.
  return 0;
}

int gap_event_callback(ble_gap_event* event, void* argument) {
  auto* link = static_cast<NimbleRuntimeLink*>(argument);
  if (link == nullptr || event == nullptr || link->runtime == nullptr) return 0;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      link->connecting = false;
      if (event->connect.status != 0) {
        if (!link->ignore_disconnect && link->connections != nullptr &&
            link->connections->selected(link->role).has_value()) {
          (void)link->connections->disconnected(link->role, monotonic_ms());
        }
        return 0;
      }
      ble_gap_conn_desc descriptor{};
      if (ble_gap_conn_find(event->connect.conn_handle, &descriptor) != 0 ||
          !link->connections->selected(link->role).has_value() ||
          !selected_peer_matches(*link->connections->selected(link->role), descriptor)) {
        (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
      }
      link->conn_handle = event->connect.conn_handle;
      const Status connected = link->connections->connected(link->role, monotonic_ms());
      if (!connected) {
        (void)ble_gap_terminate(link->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
      }
      link->generation = link->connections->state(link->role).generation;
      start_service_discovery(link);
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
      const bool ignored = link->ignore_disconnect;
      link->ignore_disconnect = false;
      link->connecting = false;
      link->conn_handle = kNoHandle;
      link->reset_profile();
      if (!ignored && link->connections != nullptr &&
          link->connections->selected(link->role).has_value()) {
        (void)link->connections->disconnected(link->role, monotonic_ms());
      }
      return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
      return notify_received(event, link);
    default:
      return 0;
  }
}

NimbleCentral::NimbleCentral() = default;

NimbleCentral::~NimbleCentral() = default;

Status NimbleCentral::initialize() {
  if (initialized_) return Status::ok();
  runtime_ = std::make_unique<NimbleRuntime>(this, &connections_);
  g_runtime = runtime_.get();
  if (nimble_port_init() != ESP_OK) {
    g_runtime = nullptr;
    runtime_.reset();
    return Status::failure(ErrorCode::kTransport, "NimBLE controller init failed");
  }
  ble_hs_cfg.reset_cb = host_reset_callback;
  ble_hs_cfg.sync_cb = host_sync_callback;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_store_config_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  initialized_ = true;
  nimble_port_freertos_init(host_task);
  return Status::ok();
}

Status NimbleCentral::start_foreground_scan(std::uint64_t now_ms) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "NimBLE is not initialized");
  if (!runtime_->host_ready) return Status::failure(ErrorCode::kConflict, "NimBLE host is not ready");
  if (scan_active_) return Status::failure(ErrorCode::kConflict, "BLE scan is already active");
  const Status manager_status = connections_.scan_started(now_ms);
  if (!manager_status) return manager_status;
  runtime_->candidate_count = 0U;
  runtime_->scan_complete = false;
  std::uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
  if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
    (void)connections_.scan_finished();
    return Status::failure(ErrorCode::kTransport, "could not determine BLE identity address type");
  }
  ble_gap_disc_params parameters{};
  parameters.passive = 1U;
  parameters.filter_duplicates = 1U;
  const int result = ble_gap_disc(own_addr_type, static_cast<std::int32_t>(kScanDurationMs),
                                  &parameters, scan_callback, runtime_.get());
  if (result != 0) {
    (void)connections_.scan_finished();
    return Status::failure(ErrorCode::kTransport, "BLE scan could not start");
  }
  scan_active_ = true;
  scan_deadline_ms_ = now_ms > std::numeric_limits<std::uint64_t>::max() - kScanDurationMs
                          ? std::numeric_limits<std::uint64_t>::max()
                          : now_ms + kScanDurationMs;
  return Status::ok();
}

Status NimbleCentral::stop_scan() {
  if (!scan_active_) return connections_.scan_finished();
  int result = 0;
  if (!runtime_->scan_complete) result = ble_gap_disc_cancel();
  scan_active_ = false;
  scan_deadline_ms_ = 0U;
  runtime_->scan_complete = false;
  const Status manager_status = connections_.scan_finished();
  if (!manager_status) return manager_status;
  return result == 0 ? Status::ok()
                     : Status::failure(ErrorCode::kTransport, "BLE scan cancel failed");
}

Status NimbleCentral::poll(std::uint64_t now_ms) {
  if (!initialized_) return Status::failure(ErrorCode::kConflict, "NimBLE is not initialized");
  if (scan_active_ && (runtime_->scan_complete || now_ms >= scan_deadline_ms_)) {
    const Status stopped = stop_scan();
    if (!stopped) return stopped;
  }
  NimbleRuntimeLink& trainer = link_for(runtime_.get(), PeripheralRole::kTrainer);
  if (trainer.control_gate.pending()) {
    const Status timeout = trainer.control_gate.poll_timeout(now_ms);
    if (!timeout) {
      invalidate_control_and_reconnect(&trainer, now_ms, "FTMS acknowledgement timed out");
    }
  }
  if (!scan_active_) {
    for (const PeripheralRole role : {PeripheralRole::kTrainer, PeripheralRole::kHeartRate}) {
      NimbleRuntimeLink& link = link_for(runtime_.get(), role);
      const ReconnectState& state = connections_.state(role);
      const bool restored_selection_due = connections_.selected(role).has_value() &&
                                           state.link == LinkState::kDisconnected;
      if (!link.connecting && link.conn_handle == kNoHandle &&
          (connections_.reconnect_due(role, now_ms) || restored_selection_due)) {
        const Status reconnect = connect_selected(role);
        if (!reconnect) return reconnect;
      }
    }
  }
  return Status::ok();
}

Status NimbleCentral::poll_scan(std::uint64_t now_ms) { return poll(now_ms); }

std::size_t NimbleCentral::scan_result_count() const {
  return runtime_ == nullptr ? 0U : runtime_->candidate_count;
}

bool NimbleCentral::scan_result(std::size_t index, DeviceIdentity* result) const {
  if (runtime_ == nullptr || result == nullptr || index >= runtime_->candidate_count) return false;
  *result = runtime_->candidates[index];
  return true;
}

Status NimbleCentral::select_device(const DeviceIdentity& device) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "NimBLE is not initialized");
  }
  NimbleRuntimeLink& link = link_for(runtime_.get(), device.role);
  if (link.connecting) return Status::failure(ErrorCode::kConflict, "BLE connection attempt is active");
  if (link.conn_handle != kNoHandle) {
    link.ignore_disconnect = true;
    const int result = ble_gap_terminate(link.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (result != 0) {
      link.ignore_disconnect = false;
      return Status::failure(ErrorCode::kTransport, "old BLE connection could not be closed");
    }
  }
  const Status selected = connections_.select(device);
  if (!selected) return selected;
  link.conn_handle = kNoHandle;
  link.generation = connections_.state(device.role).generation;
  link.reset_profile();
  return Status::ok();
}

Status NimbleCentral::forget_device(PeripheralRole role) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "NimBLE is not initialized");
  }
  NimbleRuntimeLink& link = link_for(runtime_.get(), role);
  if (link.connecting) return Status::failure(ErrorCode::kConflict, "BLE connection attempt is active");
  if (link.conn_handle != kNoHandle) {
    link.ignore_disconnect = true;
    const int result = ble_gap_terminate(link.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (result != 0) {
      link.ignore_disconnect = false;
      return Status::failure(ErrorCode::kTransport, "BLE connection could not be closed");
    }
  }
  const Status forgotten = connections_.forget(role);
  link.conn_handle = kNoHandle;
  link.reset_profile();
  return forgotten;
}

Status NimbleCentral::connect_selected(PeripheralRole role) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "NimBLE is not initialized");
  }
  if (!runtime_->host_ready) return Status::failure(ErrorCode::kConflict, "NimBLE host is not ready");
  NimbleRuntimeLink& link = link_for(runtime_.get(), role);
  const auto& selected = connections_.selected(role);
  if (!selected.has_value()) return Status::failure(ErrorCode::kConflict, "no BLE device is selected");
  if (link.connecting || link.conn_handle != kNoHandle) {
    return Status::failure(ErrorCode::kConflict, "BLE link is already active");
  }
  if (scan_active_) {
    const Status stopped = stop_scan();
    if (!stopped) return stopped;
  }
  std::uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
  if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
    return Status::failure(ErrorCode::kTransport, "could not determine BLE identity address type");
  }
  ble_addr_t peer{};
  peer.type = selected->address_type;
  std::memcpy(peer.val, selected->address.data(), selected->address.size());
  link.connecting = true;
  const int result = ble_gap_connect(own_addr_type, &peer, kConnectDurationMs, nullptr,
                                     gap_event_callback, &link);
  if (result != 0) {
    link.connecting = false;
    (void)connections_.disconnected(role, monotonic_ms());
    return Status::failure(ErrorCode::kTransport, "BLE connection could not start");
  }
  return Status::ok();
}

Status NimbleCentral::write_ftms_command(FtmsOpcode opcode, const std::uint8_t* payload,
                                         std::size_t length, std::uint64_t now_ms) {
  if (!initialized_ || runtime_ == nullptr) {
    return Status::failure(ErrorCode::kConflict, "NimBLE is not initialized");
  }
  if (payload == nullptr || length == 0U || length > kBleNotificationMaxBytes ||
      payload[0] != static_cast<std::uint8_t>(opcode)) {
    return Status::failure(ErrorCode::kInvalidArgument, "FTMS command payload does not match opcode");
  }
  NimbleRuntimeLink& link = link_for(runtime_.get(), PeripheralRole::kTrainer);
  const CharacteristicInfo* control =
      find_characteristic(&link, CharacteristicKind::kFitnessMachineControlPoint);
  const bool requesting_control = opcode == FtmsOpcode::kRequestControl;
  if (link.conn_handle == kNoHandle || link.discovery != DiscoveryStage::kReady ||
      control == nullptr || (!requesting_control && !link.control_gate.control_valid())) {
    return Status::failure(ErrorCode::kConflict, "FTMS control is not acknowledged and ready");
  }
  const Status begun = link.control_gate.begin(opcode, link.generation, now_ms);
  if (!begun) return begun;
  const int result = ble_gattc_write_flat(link.conn_handle, control->value_handle, payload,
                                           static_cast<std::uint16_t>(length), control_write_callback,
                                           &link);
  if (result != 0) {
    link.control_gate.invalidate();
    return Status::failure(ErrorCode::kTransport, "FTMS ATT write could not start");
  }
  return Status::ok();
}

Status NimbleCentral::send_and_wait(FtmsOpcode opcode, const std::uint8_t* payload,
                                    std::size_t length) {
  const Status write = write_ftms_command(opcode, payload, length, monotonic_ms());
  if (!write) return write;
  const std::uint64_t deadline = monotonic_ms() + 3000U;
  while (monotonic_ms() <= deadline) {
    if (!trainer_control_pending()) {
      return trainer_control_valid()
                 ? Status::ok()
                 : Status::failure(ErrorCode::kTransport, "FTMS indication rejected command");
    }
    const Status polled = poll(monotonic_ms());
    if (!polled) return polled;
    vTaskDelay(pdMS_TO_TICKS(10U));
  }
  return Status::failure(ErrorCode::kTimeout, "FTMS indication was not received");
}

Status NimbleCentral::acquire_control() {
  const std::vector<std::uint8_t> command = encode_request_control();
  return send_and_wait(FtmsOpcode::kRequestControl, command.data(), command.size());
}

Status NimbleCentral::set_target_power(int watts) {
  const auto command = encode_target_power(watts, trainer_capabilities());
  if (!command) return Status::failure(command.error->code, command.error->message);
  return send_and_wait(FtmsOpcode::kSetTargetPower, command.value->data(), command.value->size());
}

Status NimbleCentral::start_or_resume() {
  const std::vector<std::uint8_t> command = encode_start_resume();
  return send_and_wait(FtmsOpcode::kStartResume, command.data(), command.size());
}

Status NimbleCentral::pause() {
  const std::vector<std::uint8_t> command = encode_stop_pause(FtmsStopParameter::kPause);
  return send_and_wait(FtmsOpcode::kStopPause, command.data(), command.size());
}

Status NimbleCentral::stop() {
  const std::vector<std::uint8_t> command = encode_stop_pause(FtmsStopParameter::kStop);
  return send_and_wait(FtmsOpcode::kStopPause, command.data(), command.size());
}

bool NimbleCentral::pop_notification(BleNotification* notification) {
  if (runtime_ == nullptr || notification == nullptr) return false;
  portENTER_CRITICAL(&runtime_->notification_lock);
  if (runtime_->notification_count == 0U) {
    portEXIT_CRITICAL(&runtime_->notification_lock);
    return false;
  }
  *notification = runtime_->notifications[runtime_->notification_head];
  runtime_->notification_head = (runtime_->notification_head + 1U) % kNotificationQueueCapacity;
  --runtime_->notification_count;
  portEXIT_CRITICAL(&runtime_->notification_lock);
  return true;
}

std::uint32_t NimbleCentral::dropped_notification_count() const {
  return runtime_ == nullptr ? 0U : runtime_->dropped_notifications;
}

bool NimbleCentral::host_ready() const { return runtime_ != nullptr && runtime_->host_ready; }

bool NimbleCentral::trainer_services_ready() const {
  return runtime_ != nullptr && connections_.state(PeripheralRole::kTrainer).services_ready;
}

bool NimbleCentral::heart_rate_services_ready() const {
  return runtime_ != nullptr && connections_.state(PeripheralRole::kHeartRate).services_ready;
}

bool NimbleCentral::trainer_control_valid() const {
  return runtime_ != nullptr && link_for(runtime_.get(), PeripheralRole::kTrainer).control_gate.control_valid();
}

bool NimbleCentral::trainer_control_pending() const {
  return runtime_ != nullptr && link_for(runtime_.get(), PeripheralRole::kTrainer).control_gate.pending();
}

TrainerCapabilities NimbleCentral::trainer_capabilities() const {
  if (runtime_ == nullptr) return TrainerCapabilities{};
  return link_for(runtime_.get(), PeripheralRole::kTrainer).capabilities;
}

}  // namespace trainer_loop

#else

namespace trainer_loop {

struct NimbleRuntime {};

NimbleCentral::NimbleCentral() = default;

NimbleCentral::~NimbleCentral() = default;

Status NimbleCentral::initialize() {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::start_foreground_scan(std::uint64_t) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::poll(std::uint64_t) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::poll_scan(std::uint64_t now_ms) { return poll(now_ms); }

Status NimbleCentral::stop_scan() {
  scan_active_ = false;
  return connections_.scan_finished();
}

std::size_t NimbleCentral::scan_result_count() const { return 0U; }

bool NimbleCentral::scan_result(std::size_t, DeviceIdentity*) const { return false; }

Status NimbleCentral::select_device(const DeviceIdentity&) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::forget_device(PeripheralRole) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::connect_selected(PeripheralRole) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::write_ftms_command(FtmsOpcode, const std::uint8_t*, std::size_t,
                                         std::uint64_t) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::send_and_wait(FtmsOpcode, const std::uint8_t*, std::size_t) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::acquire_control() {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::set_target_power(int) {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::start_or_resume() {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::pause() {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

Status NimbleCentral::stop() {
  return Status::failure(ErrorCode::kUnsupported, "NimBLE requires ESP-IDF");
}

bool NimbleCentral::pop_notification(BleNotification*) { return false; }

std::uint32_t NimbleCentral::dropped_notification_count() const { return 0U; }

bool NimbleCentral::host_ready() const { return false; }

bool NimbleCentral::trainer_services_ready() const { return false; }

bool NimbleCentral::heart_rate_services_ready() const { return false; }

bool NimbleCentral::trainer_control_valid() const { return false; }

bool NimbleCentral::trainer_control_pending() const { return false; }

TrainerCapabilities NimbleCentral::trainer_capabilities() const { return TrainerCapabilities{}; }

}  // namespace trainer_loop

#endif

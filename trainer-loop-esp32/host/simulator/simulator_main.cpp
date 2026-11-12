#include "firmware_app.h"
#include "platform_services.h"
#include "ui_fixtures.h"
#ifdef TRAINER_LOOP_HAS_SDL
#include "lvgl_sdl.h"
#endif
#include "trainer_loop/storage_io.h"
#include "cJSON.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

using namespace trainer_loop;
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string str(const cJSON* j, const char* key, std::string fallback = {}) {
  auto* v = cJSON_GetObjectItemCaseSensitive(j,key);
  if (!v) return fallback;
  if (!cJSON_IsString(v)) throw std::runtime_error(std::string(key)+" must be a string");
  return v->valuestring;
}
double number(const cJSON* j,const char* key,double fallback=0) {
  auto* v=cJSON_GetObjectItemCaseSensitive(j,key);
  if (!v) return fallback;
  if (!cJSON_IsNumber(v)) throw std::runtime_error(std::string(key)+" must be numeric");
  if(!std::isfinite(v->valuedouble))throw std::runtime_error(std::string(key)+" must be finite");
  return v->valuedouble;
}
std::uint64_t integer(const cJSON* j,const char* key,std::uint64_t fallback=0,std::uint64_t maximum=86400000) {
  const auto value=number(j,key,static_cast<double>(fallback));
  if(value<0||value>static_cast<double>(maximum)||std::floor(value)!=value)
    throw std::runtime_error(std::string(key)+" must be an integer in [0,"+std::to_string(maximum)+"]");
  return static_cast<std::uint64_t>(value);
}
bool boolean(const cJSON* j,const char* key,bool fallback=false) {
  auto* v=cJSON_GetObjectItemCaseSensitive(j,key);
  if (!v) return fallback;
  if (!cJSON_IsBool(v)) throw std::runtime_error(std::string(key)+" must be boolean");
  return cJSON_IsTrue(v);
}
int signed_integer(const cJSON* j,const char* key,int fallback,int minimum,int maximum) {
  const auto value=number(j,key,fallback);
  if(value<minimum||value>maximum||std::floor(value)!=value)throw std::runtime_error(std::string(key)+" is outside its integer range");
  return static_cast<int>(value);
}
void string_field(cJSON* j,const char* key,const std::string& value) {cJSON_AddStringToObject(j,key,value.c_str());}
void require(Status s) {if(!s) throw std::runtime_error(s.error->message);}
std::filesystem::path shortest_storage_path(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute=std::filesystem::weakly_canonical(path,error);
  if(error)return path;
  const auto current=std::filesystem::current_path(error);
  if(error)return absolute;
  const auto cwd=std::filesystem::weakly_canonical(current,error);
  if(error)return absolute;
  const auto relative=absolute.lexically_relative(cwd);
  if(relative.empty())return absolute;
  return relative.native().size()<absolute.native().size()?relative:absolute;
}
class Simulator {
 public:
  explicit Simulator(std::filesystem::path root):root_(std::move(root)) { restart(true); }
  void restart(bool retained) {
    app_.reset();
    if (!retained) { // Only the dedicated run subdirectory is ever reset.
      std::filesystem::remove_all(root_/"storage");
      services().clear_http_fixtures();services().clear_events();services().set_wifi_available(true);services().set_http_available(true);services().set_time_sync_available(true);
      services().clear_scheduled_events();
      services().ftms_behavior()=HostFtmsBehavior{};storage_test_write_budget.reset();
      services().set_trainer_capabilities({true,true,true,true,true,50,1000,1});
      services().set_trainer_available(true);services().set_heart_rate_available(true);
      services().set_trainer_telemetry_enabled(true);services().set_heart_rate_telemetry_enabled(true);
    }
    services().configure(root_/"storage");
    const auto retained_utc=services().has_utc()?services().utc_ms():1790762400000LL;
    services().clear_scheduled_events();
    services().set_monotonic_ms(0);
    services().set_boot_pressed(false);
    fixture_.reset();
    services().set_utc_ms(retained?retained_utc:1790762400000LL);
    // Durable metadata stores bounded path strings. A clean export may run
    // under a deeply nested artifact directory, so give production stores
    // the shorter lexical path while retaining root_ for simulator artifacts.
    app_=std::make_unique<FirmwareApp>(shortest_storage_path(root_/"storage").string());
    app_->start();
  }
  HostPlatformServices& services() {return HostPlatformServices::instance();}
  void advance(std::uint64_t delta) {
    if(delta>86400000) throw std::runtime_error("advance limited to 24 hours per command");
    const auto end=services().monotonic_ms()+delta;
    while(services().monotonic_ms()<end) {
      auto step=std::min<std::uint64_t>(100,end-services().monotonic_ms());
      const auto event=services().next_scheduled_event_ms();
      if(event && *event>services().monotonic_ms())step=std::min(step,*event-services().monotonic_ms());
      services().advance_monotonic_ms(step);
      app_->display().host_pump(static_cast<std::uint32_t>(step));
      require(app_->runtime_step(services().monotonic_ms()));
    }
  }
  cJSON* state() {
    auto* j=cJSON_CreateObject(); const auto s=fixture_.value_or(app_->snapshot()); const auto c=app_->coordinator_snapshot();
    cJSON_AddNumberToObject(j,"seed",0);
    string_field(j,"screen",ui_screen_name(s.screen)); string_field(j,"ride_state",ride_state_name(c.state));
    cJSON_AddNumberToObject(j,"now_ms",static_cast<double>(services().monotonic_ms()));
    cJSON_AddNumberToObject(j,"utc_ms",static_cast<double>(services().utc_ms()));
    string_field(j,"workout",s.workout_name); string_field(j,"fault",s.fault);
    string_field(j,"trainer_status",s.trainer_status); string_field(j,"heart_rate_status",s.heart_rate_status);
    string_field(j,"wifi_status",s.wifi_status);
    if(s.actual_power_w)cJSON_AddNumberToObject(j,"actual_power_w",*s.actual_power_w);
    if(s.heart_rate_bpm)cJSON_AddNumberToObject(j,"heart_rate_bpm",*s.heart_rate_bpm);
    string_field(j,"safety_message",s.safety_message); string_field(j,"pause_reason",pause_reason_name(c.pause_reason));
    cJSON_AddBoolToObject(j,"resistance_unknown",c.resistance_unknown);
    cJSON_AddNumberToObject(j,"elapsed_active_ms",s.elapsed_active_ms);
    cJSON_AddNumberToObject(j,"elapsed_wall_ms",s.elapsed_wall_ms);
    cJSON_AddNumberToObject(j,"interval_index",static_cast<double>(s.interval_index));
    cJSON_AddNumberToObject(j,"target_power_w",s.target_power_w);
    auto* profile=cJSON_AddObjectToObject(j,"profile");
    cJSON_AddNumberToObject(profile,"ftp_watts",s.profile.ftp_watts);
    cJSON_AddNumberToObject(profile,"rider_kg",s.profile.rider_kg);
    cJSON_AddNumberToObject(profile,"bike_kg",s.profile.bike_kg);
    cJSON_AddBoolToObject(profile,"ftp_local_override",s.profile.ftp_local_override);
    cJSON_AddBoolToObject(j,"start_available",s.start_available);
    cJSON_AddBoolToObject(j,"resume_available",s.resume_available);
    const auto capabilities=services().trainer_capabilities();
    auto* caps=cJSON_AddObjectToObject(j,"trainer_capabilities");
    cJSON_AddBoolToObject(caps,"power_range_present",capabilities.power_range_present);
    cJSON_AddNumberToObject(caps,"min_watts",capabilities.power_min_watts);
    cJSON_AddNumberToObject(caps,"max_watts",capabilities.power_max_watts);
    cJSON_AddNumberToObject(caps,"increment_watts",capabilities.power_increment_watts);
    cJSON_AddNumberToObject(caps,"increment_watts",capabilities.power_increment_watts);
    const auto& coordinator=app_->coordinator();
    string_field(j,"session_uuid",coordinator.session_uuid());
    auto* samples=cJSON_AddArrayToObject(j,"samples");
    std::vector<TelemetrySample> recorded;
    if(!coordinator.session_uuid().empty()) {
      SessionJournal journal((root_/"storage"/"journal").string());
      const auto session=journal.has_current_session()?journal.recover_current():journal.read_session(coordinator.session_uuid());
      if(session && session.value->manifest.uuid==coordinator.session_uuid())recorded=session.value->samples;
    }
    recorded.insert(recorded.end(),coordinator.buffered_samples().begin(),coordinator.buffered_samples().end());
    cJSON_AddNumberToObject(j,"recorded_sample_count",coordinator.session_uuid().empty()?0:coordinator.recorded_sample_count());
    for(std::size_t i=recorded.size()>20?recorded.size()-20:0;i<recorded.size();++i) {
      const auto& sample=recorded[i];auto* o=cJSON_CreateObject();
      cJSON_AddNumberToObject(o,"sequence",sample.sequence);cJSON_AddNumberToObject(o,"wall_ms",sample.wall_ms);cJSON_AddNumberToObject(o,"active_ms",sample.active_ms);cJSON_AddNumberToObject(o,"power_w",sample.power_w);cJSON_AddItemToArray(samples,o);
    }
    auto* controls=cJSON_AddArrayToObject(j,"controls");
    for(const auto& v:app_->display().host_controls()) {
      auto* o=cJSON_CreateObject(); string_field(o,"id",v.id); string_field(o,"text",v.text);
      cJSON_AddNumberToObject(o,"x",v.x);cJSON_AddNumberToObject(o,"y",v.y);
      cJSON_AddNumberToObject(o,"width",v.width);cJSON_AddNumberToObject(o,"height",v.height);
      cJSON_AddBoolToObject(o,"enabled",v.enabled);cJSON_AddItemToArray(controls,o);
    }
    auto* history=cJSON_AddArrayToObject(j,"history");
    for(const auto& h:s.history_entries) {auto* o=cJSON_CreateObject();string_field(o,"uuid",h.summary.uuid);string_field(o,"upload_status",h.upload_status);cJSON_AddItemToArray(history,o);}
    auto* events=cJSON_AddArrayToObject(j,"trace");
    const auto& trace=app_->event_trace();
    for(std::size_t i=trace.size()>100?trace.size()-100:0;i<trace.size();++i)cJSON_AddItemToArray(events,cJSON_CreateString(trace[i].c_str()));
    auto* transport=cJSON_AddArrayToObject(j,"platform_trace");
    const auto& platform_events=services().events();
    for(std::size_t i=platform_events.size()>100?platform_events.size()-100:0;i<platform_events.size();++i)cJSON_AddItemToArray(transport,cJSON_CreateString(platform_events[i].c_str()));
    auto* devices=cJSON_AddArrayToObject(j,"devices");
    for(auto role:{PeripheralRole::kTrainer,PeripheralRole::kHeartRate}) {const auto& d=app_->ble().connection_manager().state(role);auto* o=cJSON_CreateObject();string_field(o,"role",role==PeripheralRole::kTrainer?"trainer":"heart_rate");cJSON_AddNumberToObject(o,"generation",d.generation);cJSON_AddBoolToObject(o,"connected",d.link==LinkState::kConnected);cJSON_AddItemToArray(devices,o);}
    return j;
  }
  cJSON* execute(const cJSON* command) {
    const auto op=str(command,"op");
    if(op=="fixture") {
      const auto name=str(command,"name");
      if(name=="setup") {require(app_->display().show_setup(make_setup_credentials(0x51a7c0deU,1)));UiSnapshot s;s.screen=UiScreen::kSetup;fixture_=s;}
      else {fixture_=ui_fixture(name);require(app_->display().present(*fixture_));}
      app_->display().host_pump(500);return state();
    }
    if(op=="fixtures") {auto* result=state();auto* names=cJSON_AddArrayToObject(result,"fixtures");cJSON_AddItemToArray(names,cJSON_CreateString("setup"));for(const auto& name:ui_fixture_names())cJSON_AddItemToArray(names,cJSON_CreateString(name.c_str()));return result;}
    if(op=="advance") {
      advance(integer(command,"ms"));
    } else if(op=="touch") {
      int x=signed_integer(command,"x",0,0,367), y=signed_integer(command,"y",0,0,447);
      if(x<0||x>=368||y<0||y>=448)throw std::runtime_error("touch outside 368x448 display");
      app_->display().host_pointer(x,y,boolean(command,"pressed"));app_->display().host_pump(20);require(app_->runtime_step(services().monotonic_ms()));
    } else if(op=="pointer_click") {
      int x=signed_integer(command,"x",0,0,367),y=signed_integer(command,"y",0,0,447);
      if(x<0||x>=368||y<0||y>=448)throw std::runtime_error("touch outside 368x448 display");
      app_->display().host_pointer(x,y,true);
      app_->display().host_pointer(x,y,false);
      require(app_->runtime_step(services().monotonic_ms()));
    } else if(op=="click") {
      const auto wanted=str(command,"text");const auto id=str(command,"control_id");bool found=false;
      if(wanted.empty()&&id.empty())throw std::runtime_error("click requires text or control_id");
      for(const auto& v:app_->display().host_controls()) if((!id.empty()&&v.id==id)||(id.empty()&&v.text==wanted)) {
        if(!v.enabled)throw std::runtime_error("control disabled: "+wanted);
        int x=v.x+v.width/2,y=v.y+v.height/2;
        app_->display().host_pointer(x,y,true);
        app_->display().host_pointer(x,y,false);
        require(app_->runtime_step(services().monotonic_ms()));found=true;break;
      }
      if(!found)throw std::runtime_error("visible control not found: "+wanted);
    } else if(op=="boot") {services().set_boot_pressed(boolean(command,"pressed"));}
    else if(op=="clock") {require(app_->set_simulated_utc(static_cast<std::int64_t>(integer(command,"utc_ms",1790762400000LL,4102444800000LL)),boolean(command,"trusted",true)));}
    else if(op=="restart") {restart(true);}
    else if(op=="reset") {restart(false);}
    else if(op=="setup") {
      SetupCandidate c;c.secrets.wifi_ssid=str(command,"ssid","Simulator");c.secrets.wifi_password=str(command,"password","synthetic-password");
      c.secrets.athlete_id=str(command,"athlete_id","sim-athlete");c.secrets.api_key=str(command,"api_key","synthetic-key");
      require(app_->setup(c));
    } else if(op=="screenshot") {require(app_->display().host_screenshot_png(str(command,"path")));}
    else if(op=="network") {services().set_wifi_available(boolean(command,"wifi",true));services().set_http_available(boolean(command,"http",true));services().set_time_sync_available(boolean(command,"time_sync",true));}
    else if(op=="device") {
      const auto role=str(command,"role","trainer");if(role!="trainer"&&role!="heart_rate")throw std::runtime_error("role must be trainer or heart_rate");
      if(role=="trainer") {services().set_trainer_available(boolean(command,"available",true));services().set_trainer_telemetry_enabled(boolean(command,"telemetry",true));}
      else {services().set_heart_rate_available(boolean(command,"available",true));services().set_heart_rate_telemetry_enabled(boolean(command,"telemetry",true));}
    }
    else if(op=="schedule") {
      const auto at=integer(command,"at_ms",services().monotonic_ms(),9007199254740991ULL);
      const auto kind=str(command,"kind","device");
      bool accepted=false;
      if(kind=="device") {
        const auto role=str(command,"role","trainer");
        if(role!="trainer"&&role!="heart_rate")throw std::runtime_error("role must be trainer or heart_rate");
        accepted=services().schedule_device_event(at,role=="trainer"?PeripheralRole::kTrainer:PeripheralRole::kHeartRate,boolean(command,"available",true),boolean(command,"telemetry",true));
      } else if(kind=="network") {
        accepted=services().schedule_network_event(at,boolean(command,"wifi",true),boolean(command,"http",true));
      }
      if(!accepted)throw std::runtime_error("schedule queue full or kind invalid");
    }
    else if(op=="notification") {
      BleNotification n;const auto role=str(command,"role","trainer");
      if(role!="trainer"&&role!="heart_rate")throw std::runtime_error("role must be trainer or heart_rate");
      n.role=role=="trainer"?PeripheralRole::kTrainer:PeripheralRole::kHeartRate;
      const auto kind=str(command,"kind","bike");
      if(kind=="bike")n.kind=BleNotificationKind::kIndoorBikeData;else if(kind=="heart_rate")n.kind=BleNotificationKind::kHeartRateMeasurement;else if(kind=="control")n.kind=BleNotificationKind::kFitnessMachineControlPoint;else if(kind=="status")n.kind=BleNotificationKind::kFitnessMachineStatus;else throw std::runtime_error("unknown notification kind");
      n.generation=static_cast<std::uint32_t>(integer(command,"generation",0,UINT32_MAX));n.timestamp_ms=integer(command,"timestamp_ms",services().monotonic_ms(),9007199254740991ULL);
      const auto hex=str(command,"hex");if(hex.size()%2||hex.size()>128)throw std::runtime_error("hex must contain at most 64 bytes");
      for(std::size_t i=0;i<hex.size();i+=2) {auto part=hex.substr(i,2);if(part.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)throw std::runtime_error("invalid hex");n.payload[n.length++]=static_cast<std::uint8_t>(std::stoul(part,nullptr,16));}
      require(app_->ble().host_inject_notification(n));
    }
    else if(op=="capabilities") {auto c=services().trainer_capabilities();c.ftms_present=boolean(command,"ftms",c.ftms_present);c.control_point_present=boolean(command,"control_point",c.control_point_present);c.indoor_bike_data_present=boolean(command,"indoor_bike_data",c.indoor_bike_data_present);c.feature_present=boolean(command,"feature",c.feature_present);c.power_range_present=boolean(command,"power_range",c.power_range_present);c.power_min_watts=signed_integer(command,"min_watts",c.power_min_watts,-32768,32767);c.power_max_watts=signed_integer(command,"max_watts",c.power_max_watts,-32768,32767);c.power_increment_watts=signed_integer(command,"increment_watts",c.power_increment_watts,0,65535);services().set_trainer_capabilities(c);}
    else if(op=="http") {services().set_http_fixture(str(command,"method","GET"),str(command,"path"),{signed_integer(command,"status",200,100,599),str(command,"body"),static_cast<std::uint32_t>(integer(command,"retry_after_s",0,86400))});}
    else if(op=="ftms") {
      auto& b=services().ftms_behavior();b.acknowledgement_delay_ms=static_cast<std::uint32_t>(integer(command,"delay_ms",40,60000));
      b.reject_next_command=boolean(command,"reject");b.timeout_next_command=boolean(command,"timeout");
      b.disconnect_before_next_ack=boolean(command,"disconnect");b.emit_stale_generation_next=boolean(command,"stale_generation");b.auto_reconnect=boolean(command,"auto_reconnect",true);
    } else if(op=="storage") {
      if(boolean(command,"clear")) storage_test_write_budget.reset();
      else {storage_test_write_budget=static_cast<std::size_t>(integer(command,"write_budget",0,1073741824));}
    } else if(op!="state") throw std::runtime_error("unknown op: "+op);
    return state();
  }
  LvglPresenter& display() {return app_->display();}
  bool setup_pending() const {return app_->snapshot().screen==UiScreen::kSetup;}
 private:
  std::filesystem::path root_;std::unique_ptr<FirmwareApp> app_;std::optional<UiSnapshot> fixture_;
};
}
int main(int argc,char** argv) {
 try {
  std::filesystem::path root=std::filesystem::temp_directory_path()/("trainer-simulator-"+std::to_string(getpid()));
  bool window=false, provisioned=false;
  unsigned window_frames=0;
  for(int i=1;i<argc;++i) {std::string arg=argv[i];if(arg=="--storage"&&i+1<argc) root=argv[++i];else if(arg=="--window")window=true;else if(arg=="--provisioned")provisioned=true;else if(arg=="--window-frames"&&i+1<argc)window_frames=static_cast<unsigned>(std::stoul(argv[++i]));else throw std::runtime_error("usage: trainer_simulator [--storage RUN_DIRECTORY] [--window] [--provisioned] [--window-frames N]");}
  Simulator sim(root);std::ofstream trace(root/"commands.jsonl",std::ios::app);
  if(provisioned && sim.setup_pending()) {Json command(cJSON_Parse("{\"op\":\"setup\"}"),cJSON_Delete);Json result(sim.execute(command.get()),cJSON_Delete);sim.advance(100);}
  if(window) {
#ifdef TRAINER_LOOP_HAS_SDL
    return run_lvgl_sdl(sim.display(),[&sim](std::uint32_t ms){sim.advance(ms);},[&sim](bool pressed){sim.services().set_boot_pressed(pressed);},2,window_frames);
#else
    throw std::runtime_error("SDL2 unavailable at build time; install SDL2 and rebuild for --window");
#endif
  }
  std::string line;while(std::getline(std::cin,line)) {
    Json response(cJSON_CreateObject(),cJSON_Delete);Json input(cJSON_Parse(line.c_str()),cJSON_Delete);
    try {if(!input||!cJSON_IsObject(input.get()))throw std::runtime_error("command must be a JSON object");
      auto* id=cJSON_GetObjectItemCaseSensitive(input.get(),"id");if(id)cJSON_AddItemToObject(response.get(),"id",cJSON_Duplicate(id,1));
      cJSON_AddItemToObject(response.get(),"state",sim.execute(input.get()));cJSON_AddBoolToObject(response.get(),"ok",true);
    } catch(const std::exception& e) {cJSON_AddBoolToObject(response.get(),"ok",false);string_field(response.get(),"error",e.what());cJSON_AddItemToObject(response.get(),"state",sim.state());}
    char* output=cJSON_PrintUnformatted(response.get());std::cout<<output<<std::endl;trace<<line<<'\n'<<output<<'\n';trace.flush();cJSON_free(output);
  }
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}

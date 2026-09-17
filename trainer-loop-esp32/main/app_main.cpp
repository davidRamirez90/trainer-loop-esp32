#include "firmware_app.h"

extern "C" void app_main() {
  static trainer_loop::FirmwareApp app;
  app.start();
}

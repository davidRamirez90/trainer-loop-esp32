#pragma once

#include "trainer_loop/models.h"
#include "trainer_loop/result.h"
#include "trainer_loop/workout.h"

#include <cstdint>
#include <vector>

namespace trainer_loop {

struct PhysicsParams {
  double rider_kg = 75.0;
  double bike_kg = 8.0;
  double crr = 0.005;
  double cda = 0.32;
};

Status validate_physics(const PhysicsParams& params);
double power_at_speed(double speed_mps, double grade_percent, const PhysicsParams& params);
double virtual_speed_mps(int power_watts, double grade_percent, const PhysicsParams& params);

struct RouteProfile {
  std::vector<double> grade_percent;
  std::vector<double> expected_altitude_m;

  double grade_at(std::uint32_t second) const;
};

Result<RouteProfile> generate_route(const Workout& workout, int ftp_watts,
                                    const PhysicsParams& params);

struct VirtualPoint {
  double speed_mps = 0.0;
  double distance_m = 0.0;
  double altitude_m = 0.0;
  double ascent_m = 0.0;
  double grade_percent = 0.0;
  bool speed_valid = false;
};

class VirtualRideTracker {
 public:
  VirtualRideTracker(RouteProfile route, PhysicsParams params);
  VirtualPoint tick(std::uint32_t plan_ms, int power_watts, bool power_fresh);
  void reset();

  const VirtualPoint& last() const { return last_; }

 private:
  RouteProfile route_;
  PhysicsParams params_;
  bool has_tick_ = false;
  std::uint32_t last_plan_ms_ = 0U;
  VirtualPoint last_;
};

}  // namespace trainer_loop

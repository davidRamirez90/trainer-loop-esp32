#include "trainer_loop/physics.h"

#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"
#include "trainer_loop/settings.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trainer_loop {
namespace {

constexpr double kGravity = 9.81;
constexpr double kAirDensity = 1.226;
constexpr std::uint32_t kNoiseBucketSeconds = 30U;
constexpr double kNoiseAmplitude = 0.8;
constexpr double kSmoothingAlpha = 0.12;

class PortableRandom {
 public:
  explicit PortableRandom(std::uint32_t seed) : state_(seed == 0U ? 0x9e3779b9U : seed) {}

  double symmetric(double amplitude) {
    // xorshift32 is deliberately specified here instead of using a C++ or
    // Kotlin library RNG, so route fixtures remain portable across targets.
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    const double unit = static_cast<double>(state_) / 4294967295.0;
    return ((unit * 2.0) - 1.0) * amplitude;
  }

 private:
  std::uint32_t state_;
};

double midpoint(const TargetRange& range) { return (range.low + range.high) / 2.0; }

}  // namespace

Status validate_physics(const PhysicsParams& params) {
  ProfileSettings profile;
  profile.rider_kg = params.rider_kg;
  profile.bike_kg = params.bike_kg;
  profile.crr = params.crr;
  profile.cda = params.cda;
  profile.ftp_watts = 250;
  return validate_profile(profile);
}

double power_at_speed(double speed_mps, double grade_percent, const PhysicsParams& params) {
  const double mass = params.rider_kg + params.bike_kg;
  const double theta = std::atan(grade_percent / 100.0);
  return (params.crr * mass * kGravity * std::cos(theta) + mass * kGravity * std::sin(theta)) * speed_mps +
         0.5 * kAirDensity * params.cda * speed_mps * speed_mps * speed_mps;
}

double virtual_speed_mps(int power_watts, double grade_percent, const PhysicsParams& params) {
  const double power = static_cast<double>(std::clamp(power_watts, 0, static_cast<int>(kPowerMaxWatts)));
  const double grade = std::clamp(grade_percent, -20.0, 20.0);
  constexpr double kLowSpeed = 1e-3;
  constexpr double kMaxSpeed = 40.0;
  // This also gives a positive terminal coasting speed on a sufficiently
  // steep descent when power is zero: power_at(low) is then negative and the
  // positive crossing is found by bisection.
  if (power_at_speed(kLowSpeed, grade, params) >= power) return 0.0;
  double low = kLowSpeed;
  double high = kMaxSpeed;
  for (unsigned i = 0U; i < 50U; ++i) {
    const double middle = (low + high) / 2.0;
    if (power_at_speed(middle, grade, params) < power) low = middle;
    else high = middle;
  }
  return (low + high) / 2.0;
}

double RouteProfile::grade_at(std::uint32_t second) const {
  if (grade_percent.empty()) return 0.0;
  const std::size_t index = std::min<std::size_t>(second, grade_percent.size() - 1U);
  return grade_percent[index];
}

Result<RouteProfile> generate_route(const Workout& workout, int ftp_watts,
                                    const PhysicsParams& params) {
  const Status workout_status = validate_workout(workout, ftp_watts);
  if (!workout_status) return Result<RouteProfile>::failure(workout_status.error->code, workout_status.error->message);
  const Status physics_status = validate_physics(params);
  if (!physics_status) return Result<RouteProfile>::failure(physics_status.error->code, physics_status.error->message);
  const std::uint32_t total_seconds = (workout.duration_ms() + 999U) / 1000U;
  if (total_seconds == 0U) return Result<RouteProfile>::success(RouteProfile{});

  PortableRandom random(fnv1a32(workout.id));
  const std::size_t noise_count = static_cast<std::size_t>(total_seconds / kNoiseBucketSeconds) + 2U;
  std::vector<double> noise(noise_count);
  for (double& value : noise) value = random.symmetric(kNoiseAmplitude);

  RouteProfile route;
  route.grade_percent.resize(total_seconds);
  route.expected_altitude_m.resize(total_seconds);
  double previous = 0.0;
  double expected_altitude = 0.0;
  for (std::uint32_t second = 0U; second < total_seconds; ++second) {
    const TargetRange range = resolved_target_range_at(workout, second * 1000U, ftp_watts);
    const double target = midpoint(range);
    const double percent_ftp = target * 100.0 / static_cast<double>(ftp_watts);
    const double base = std::clamp((percent_ftp - 65.0) / 8.0, -3.0, 8.0);
    const std::size_t bucket = second / kNoiseBucketSeconds;
    const double fraction = static_cast<double>(second % kNoiseBucketSeconds) /
                            static_cast<double>(kNoiseBucketSeconds);
    const double raw = base + noise[bucket] * (1.0 - fraction) + noise[bucket + 1U] * fraction;
    const double grade = second == 0U ? raw : previous + (raw - previous) * kSmoothingAlpha;
    route.grade_percent[second] = grade;
    previous = grade;
    expected_altitude += virtual_speed_mps(static_cast<int>(std::lround(target)), grade, params) * grade / 100.0;
    route.expected_altitude_m[second] = expected_altitude;
  }
  return Result<RouteProfile>::success(std::move(route));
}

VirtualRideTracker::VirtualRideTracker(RouteProfile route, PhysicsParams params)
    : route_(std::move(route)), params_(params) {}

VirtualPoint VirtualRideTracker::tick(std::uint32_t plan_ms, int power_watts, bool power_fresh) {
  const std::uint32_t second = plan_ms / 1000U;
  const double grade = route_.grade_at(second);
  const std::uint32_t dt_ms = !has_tick_ || plan_ms <= last_plan_ms_
                                  ? 0U
                                  : std::min<std::uint32_t>(1000U, plan_ms - last_plan_ms_);
  const bool valid = power_fresh;
  const double speed = valid ? virtual_speed_mps(power_watts, grade, params_) : 0.0;
  const double dt = static_cast<double>(dt_ms) / 1000.0;
  if (valid && dt > 0.0) {
    last_.distance_m += speed * dt;
    const double delta_altitude = speed * dt * grade / 100.0;
    last_.altitude_m += delta_altitude;
    if (delta_altitude > 0.0) last_.ascent_m += delta_altitude;
  }
  last_.speed_mps = speed;
  last_.grade_percent = grade;
  last_.speed_valid = valid;
  last_plan_ms_ = plan_ms;
  has_tick_ = true;
  return last_;
}

void VirtualRideTracker::reset() {
  has_tick_ = false;
  last_plan_ms_ = 0U;
  last_ = VirtualPoint{};
}

}  // namespace trainer_loop

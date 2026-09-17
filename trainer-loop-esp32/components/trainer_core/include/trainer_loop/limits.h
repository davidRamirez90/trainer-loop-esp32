#pragma once

#include <cstddef>
#include <cstdint>

namespace trainer_loop {

constexpr std::size_t kMaxWorkoutSourceBytes = 256U * 1024U;
constexpr std::size_t kMaxWorkoutSegments = 2000U;
constexpr std::uint32_t kMaxWorkoutDurationMs = 4U * 60U * 60U * 1000U;
constexpr std::size_t kMaxWorkoutNameBytes = 96U;
constexpr std::size_t kMaxWorkoutTextBytes = 256U;
constexpr std::size_t kMaxAthleteIdBytes = 64U;
constexpr std::size_t kMaxApiKeyBytes = 192U;
constexpr std::size_t kMaxWifiSsidBytes = 32U;
constexpr std::size_t kMaxWifiPasswordBytes = 64U;
constexpr std::size_t kMaxTimezoneBytes = 48U;
constexpr std::size_t kMaxEventReasonBytes = 64U;

constexpr std::uint32_t kPowerMinWatts = 0U;
constexpr std::uint32_t kPowerMaxWatts = 2000U;
constexpr int kFtpMinWatts = 50;
constexpr int kFtpMaxWatts = 600;
constexpr double kRiderMinKg = 30.0;
constexpr double kRiderMaxKg = 200.0;
constexpr double kBikeMinKg = 5.0;
constexpr double kBikeMaxKg = 15.0;
constexpr double kCrrMin = 0.002;
constexpr double kCrrMax = 0.010;
constexpr double kCdAMin = 0.15;
constexpr double kCdAMax = 0.60;

constexpr std::size_t kTelemetrySampleBytes = 48U;
constexpr std::size_t kMaxSessionSamples = static_cast<std::size_t>(kMaxWorkoutDurationMs / 1000U) + 1U;
constexpr std::size_t kMaxFitArtifactBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxJournalChunkBytes = 16U * 1024U;
constexpr std::uint32_t kPowerStaleAfterMs = 3000U;
constexpr std::uint32_t kHeartRateStaleAfterMs = 5000U;

}  // namespace trainer_loop

#pragma once

#include "trainer_loop/intervals.h"
#include "trainer_loop/result.h"

#include <cstdint>
#include <string>

namespace trainer_loop {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::uint32_t retry_after_s = 0U;
};

class IntervalsHttpClient {
 public:
  Result<HttpResponse> execute(const HttpRequest& request) const;
};

}  // namespace trainer_loop

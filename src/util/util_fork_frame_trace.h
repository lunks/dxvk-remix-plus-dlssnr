#pragma once

// [nfs2se 0042] DIAGNOSTIC: per-frame stage timers (CPU wall clock), one "[nfs2se-fs]" log line per event, so a one-frame hitch
// can be NAMED (the 0015 GPU timers print one averaged line per 600 frames). Header-only, no state but the clock origin.
//   nowMs()   milliseconds on a steady clock since the first call (any thread)
//   line(...) printf-style, one Logger::info line prefixed "[nfs2se-fs] t=<nowMs> "

#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "log/log.h"

namespace dxvk {
  namespace nfs2seft {

    inline double nowMs() {
      static const std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_t0).count();
    }

    inline void line(const char* fmt, ...) {
      char body[480];
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(body, sizeof(body), fmt, ap);
      va_end(ap);
      char out[540];
      snprintf(out, sizeof(out), "[nfs2se-fs] t=%.1f %s", nowMs(), body);
      Logger::info(out);
    }

  }
}

#pragma once

namespace cte::runtime_compat {

// Records the host runtime and graphics-module paths without taking a hard
// dependency on Wine or Proton. Call immediately after the first log line and
// before graphics bootstrap so silent loader failures can be separated from
// swapchain-discovery failures.
void log_environment();

} // namespace cte::runtime_compat

#pragma once

#include "wdk.hpp"

namespace nv::kmdod {

NTSTATUS InitializeDriverRuntime() noexcept;
void ShutdownDriverRuntime() noexcept;

} // namespace nv::kmdod

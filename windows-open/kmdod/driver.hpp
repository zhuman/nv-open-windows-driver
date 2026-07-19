#pragma once

#include "wdk.hpp"

namespace nv::kmdod {

NTSTATUS InitializeDriverRuntime(
	_In_ PDRIVER_OBJECT driverObject,
	_In_ const UNICODE_STRING* registryPath) noexcept;
void ShutdownDriverRuntime() noexcept;

} // namespace nv::kmdod

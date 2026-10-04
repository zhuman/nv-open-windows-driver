#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "nvkms_windows.h"

namespace nv::iddcx::display_modes {

void FillSignalInfo(
	DISPLAYCONFIG_VIDEO_SIGNAL_INFO* signalInfo,
	const NV_KMS_WINDOWS_MODE& mode,
	bool monitorMode) noexcept;
IDDCX_MONITOR_MODE CreateMonitorMode(
	const NV_KMS_WINDOWS_MODE& mode,
	IDDCX_MONITOR_MODE_ORIGIN origin) noexcept;
IDDCX_MONITOR_MODE2 CreateMonitorMode2(
	const NV_KMS_WINDOWS_MODE& mode,
	IDDCX_MONITOR_MODE_ORIGIN origin) noexcept;
IDDCX_TARGET_MODE CreateTargetMode(const NV_KMS_WINDOWS_MODE& mode) noexcept;
IDDCX_TARGET_MODE2 CreateTargetMode2(const NV_KMS_WINDOWS_MODE& mode) noexcept;
DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY GetOutputTechnology(
	NvU32 connectorType) noexcept;
NTSTATUS GetDisplayState(NvU32 index, NV_KMS_WINDOWS_DISPLAY_STATE* state) noexcept;
NTSTATUS GetDisplayState(
	const IDDCX_MONITOR_DESCRIPTION& description,
	NV_KMS_WINDOWS_DISPLAY_STATE* state) noexcept;
NTSTATUS FillMonitorModeBuffer(
	const NV_KMS_WINDOWS_DISPLAY_STATE& state,
	UINT inputCount,
	IDDCX_MONITOR_MODE* modes,
	IDDCX_MONITOR_MODE_ORIGIN origin,
	UINT* outputCount,
	UINT* preferredIndex) noexcept;
NTSTATUS FillMonitorMode2Buffer(
	const NV_KMS_WINDOWS_DISPLAY_STATE& state,
	UINT inputCount,
	IDDCX_MONITOR_MODE2* modes,
	UINT* outputCount,
	UINT* preferredIndex) noexcept;
const NV_KMS_WINDOWS_MODE* FindDisplayMode(
	const NV_KMS_WINDOWS_DISPLAY_STATE& state,
	const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signalInfo) noexcept;
const NV_KMS_WINDOWS_MODE* FindDisplayMode(
	const NV_KMS_WINDOWS_DISPLAY_STATE& state,
	NvU32 modeIndex) noexcept;

} // namespace nv::iddcx::display_modes

namespace nv::iddcx {
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION ParseMonitorDescription;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 ParseMonitorDescription2;
} // namespace nv::iddcx

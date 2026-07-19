#include "adapter.hpp"

namespace {

constexpr wchar_t EndpointFriendlyName[] = L"NVIDIA Indirect Display Adapter";
constexpr wchar_t EndpointManufacturerName[] = L"NVIDIA";
constexpr wchar_t EndpointModelName[] = L"Open GPU Kernel Modules";

} // namespace

namespace nv::iddcx {

Adapter::Adapter() noexcept
    : handle_(nullptr),
      initializationStatus_(STATUS_NOT_FOUND),
      state_(State::Stopped),
      powered_(false)
{
}

NTSTATUS Adapter::EnterD0(_In_ WDFDEVICE device) noexcept
{
    powered_ = true;
    if (handle_ != nullptr)
    {
        return state_ == State::Failed
            ? initializationStatus_
            : STATUS_SUCCESS;
    }

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;

    IDDCX_ADAPTER_CAPS capabilities = {};
    capabilities.Size = sizeof(capabilities);
    capabilities.Flags = IDDCX_ADAPTER_FLAGS_NONE;
    capabilities.MaxMonitorsSupported = 1;
    capabilities.EndPointDiagnostics.Size =
        sizeof(capabilities.EndPointDiagnostics);
    capabilities.EndPointDiagnostics.TransmissionType =
        IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    capabilities.EndPointDiagnostics.pEndPointFriendlyName = EndpointFriendlyName;
    capabilities.EndPointDiagnostics.pEndPointManufacturerName =
        EndpointManufacturerName;
    capabilities.EndPointDiagnostics.pEndPointModelName = EndpointModelName;
    capabilities.EndPointDiagnostics.pHardwareVersion = &version;
    capabilities.EndPointDiagnostics.pFirmwareVersion = &version;
    capabilities.EndPointDiagnostics.GammaSupport =
        IDDCX_FEATURE_IMPLEMENTATION_NONE;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AdapterObjectContext);
    attributes.EvtCleanupCallback = Cleanup;

    IDARG_IN_ADAPTER_INIT input = {};
    input.WdfDevice = device;
    input.pCaps = &capabilities;
    input.ObjectAttributes = &attributes;

    IDARG_OUT_ADAPTER_INIT output = {};
    const NTSTATUS status = IddCxAdapterInitAsync(&input, &output);
    if (!NT_SUCCESS(status))
    {
        powered_ = false;
        initializationStatus_ = status;
        state_ = State::Failed;
        return status;
    }

    handle_ = output.AdapterObject;
    GetAdapterObjectContext(handle_)->owner = this;
    initializationStatus_ = STATUS_PENDING;
    state_ = State::Initializing;
    return STATUS_SUCCESS;
}

void Adapter::ExitD0() noexcept
{
    powered_ = false;
}

void Adapter::Reset() noexcept
{
    powered_ = false;
    if (handle_ != nullptr)
    {
        AdapterObjectContext* context = GetAdapterObjectContext(handle_);
        context->owner = nullptr;
        WdfObjectDelete(handle_);
        handle_ = nullptr;
    }

    initializationStatus_ = STATUS_NOT_FOUND;
    state_ = State::Stopped;
}

NTSTATUS Adapter::CompleteInitialization(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* arguments) noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Initializing)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    initializationStatus_ = arguments->AdapterInitStatus;
    state_ = NT_SUCCESS(initializationStatus_) ? State::Ready : State::Failed;
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::CommitModes(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_COMMITMODES* arguments) const noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    return powered_ ? STATUS_SUCCESS : STATUS_DEVICE_POWER_FAILURE;
}

_Use_decl_annotations_
void Adapter::Cleanup(WDFOBJECT adapterObject)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapterObject);
    if (context->owner != nullptr)
    {
        context->owner->handle_ = nullptr;
        context->owner->initializationStatus_ = STATUS_NOT_FOUND;
        context->owner->state_ = State::Stopped;
        context->owner->powered_ = false;
        context->owner = nullptr;
    }
}

_Use_decl_annotations_
NTSTATUS AdapterInitFinished(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_ADAPTER_INIT_FINISHED* arguments)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CompleteInitialization(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterCommitModes(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES* arguments)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CommitModes(adapter, arguments);
}

} // namespace nv::iddcx
#pragma once

// Let ETW verbosity be independent of the in-flight recorder's default filter.
#define ENABLE_WPP_TRACE_FILTERING_WITH_WPP_RECORDER 1

#define WPP_CONTROL_GUIDS \
    WPP_DEFINE_CONTROL_GUID( \
        NvIddCxTraceGuid, \
        (9d829d2e, 9292, 4b43, 9a8b, 6a6c32f53479), \
        WPP_DEFINE_BIT(TRACE_DRIVER) \
        WPP_DEFINE_BIT(TRACE_PCI))

#define WPP_LEVEL_FLAGS_LOGGER(level, flags) \
    WPP_LEVEL_LOGGER(flags)

#define WPP_LEVEL_FLAGS_ENABLED(level, flags) \
    (WPP_LEVEL_ENABLED(flags) && \
     WPP_CONTROL(WPP_BIT_ ## flags).Level >= (level))

// begin_wpp config
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
// end_wpp
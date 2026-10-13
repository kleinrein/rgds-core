#pragma once
#include <cstdint>

namespace rgds {
// Opt-in host benchmark instrumentation; production builds add no counters.
struct BridgeMetrics {
    uint64_t file_stats=0,file_reads=0,block_checks=0,state_stats=0;
    uint64_t keyboard_copies=0,capture_checks=0,sdl_lookups=0,gl_lookups=0;
    uint64_t frame_allocations=0,frame_copies=0,composites=0,texture_uploads=0,draws=0;
};
#ifdef RGDS_BRIDGE_PROFILE
inline BridgeMetrics& bridge_metrics(){static BridgeMetrics counters;return counters;}
#define RGDS_PROFILE(field,amount) (rgds::bridge_metrics().field+=(amount))
#else
#define RGDS_PROFILE(field,amount) ((void)0)
#endif
}

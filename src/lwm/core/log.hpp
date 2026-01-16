#pragma once

// Zero-cost debug logging for LWM
//
// In debug builds: logs to stderr with file:line info
// In release builds (NDEBUG defined): compiles to nothing
//
// Usage:
//   LWM_DEBUG("message");
//   LWM_DEBUG("value = " << value);

#ifndef NDEBUG

#include <iostream>

#define LWM_DEBUG(msg) \
    std::cerr << "[LWM] " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl

#define LWM_DEBUG_KEY(state, keysym) \
    std::cerr << "[LWM] Key: state=0x" << std::hex << state << " keysym=0x" << keysym << std::dec << std::endl

#else

// Release mode: these expand to nothing (zero cost)
#define LWM_DEBUG(msg) ((void)0)
#define LWM_DEBUG_KEY(state, keysym) ((void)0)

#endif

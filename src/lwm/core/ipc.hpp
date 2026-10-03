#pragma once

#include <filesystem>

namespace lwm::ipc {

inline constexpr size_t max_reply_bytes = 8 * 1024 * 1024;
inline constexpr size_t max_event_bytes = 1024 * 1024;

// $XDG_RUNTIME_DIR/lwm (or /tmp/lwm-UID) holds one socket per sanitized display name.
std::filesystem::path default_socket_path();

} // namespace lwm::ipc

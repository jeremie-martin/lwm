#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xcb/xcb.h>

// Test-only interposition. The driver reads counters after a completed IPC reply.
static uint64_t volatile* counts;
__attribute__((constructor)) static void initialize(void)
{
    char const* path = getenv("LWM_TRANSITION_COUNTS");
    if (!path)
        return;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        abort();
    void* mapping = mmap(NULL, 6 * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        abort();
    counts = mapping;
}

#define REAL(name)                 \
    static __typeof__(&name) real; \
    if (!real)                     \
    real = dlsym(RTLD_NEXT, #name)
#define COUNT(index)         \
    do {                     \
        if (counts)          \
            ++counts[index]; \
    } while (0)

xcb_get_property_cookie_t xcb_get_property(
    xcb_connection_t* connection,
    uint8_t remove,
    xcb_window_t window,
    xcb_atom_t property,
    xcb_atom_t type,
    uint32_t offset,
    uint32_t length
)
{
    REAL(xcb_get_property);
    COUNT(0);
    return real(connection, remove, window, property, type, offset, length);
}
xcb_void_cookie_t
xcb_configure_window(xcb_connection_t* connection, xcb_window_t window, uint16_t mask, void const* values)
{
    REAL(xcb_configure_window);
    if (mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))
        COUNT(1);
    return real(connection, window, mask, values);
}
xcb_get_input_focus_cookie_t xcb_get_input_focus(xcb_connection_t* connection)
{
    REAL(xcb_get_input_focus);
    COUNT(2);
    return real(connection);
}
xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t* connection, xcb_window_t window)
{
    REAL(xcb_query_tree);
    COUNT(3);
    return real(connection, window);
}

int xcb_flush(xcb_connection_t* connection)
{
    REAL(xcb_flush);
    COUNT(4);
    return real(connection);
}

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t* connection, xcb_drawable_t drawable)
{
    REAL(xcb_get_geometry);
    COUNT(5);
    return real(connection, drawable);
}

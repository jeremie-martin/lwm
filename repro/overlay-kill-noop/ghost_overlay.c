// ghost_overlay.c — a minimal non-activating, on-top overlay client.
//
// Stands in for the ABILITY companion's always-on-top / click-through overlay:
// a window lwm keeps mapped and on top but never makes the active (focused)
// window. We advertise _NET_WM_WINDOW_TYPE_UTILITY so lwm's overlay-layer rule
// lifts it to the overlay layer (sticky, root-sized, never activated) — the
// same path the existing overlay integration test exercises.
//
// It deliberately never advertises WM_DELETE_WINDOW and never exits on its own,
// so the *only* way to get rid of it is the WM force-killing the client. That
// is exactly the situation the user hit: a stuck fullscreen window that the
// keyboard could not dismiss.
//
// Build:  cc ghost_overlay.c -o ghost_overlay -lxcb
// (driven by repro.sh; you normally don't run it by hand)
//
// Arg:  --normal   skip the utility type, so lwm treats it as an ordinary
//                  activating window. Used by repro.sh as a control: super+q
//                  *should* kill it, proving key injection + the kill path work.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

int main(int argc, char **argv)
{
    int normal = (argc > 1 && strcmp(argv[1], "--normal") == 0);

    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (!c || xcb_connection_has_error(c)) {
        fprintf(stderr, "ghost_overlay: cannot connect to X\n");
        return 1;
    }
    xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

    xcb_window_t w = xcb_generate_id(c);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = { s->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root,
                      0, 0, 400, 300, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
                      mask, values);

    if (!normal) {
        xcb_atom_t wtype = intern(c, "_NET_WM_WINDOW_TYPE");
        xcb_atom_t util = intern(c, "_NET_WM_WINDOW_TYPE_UTILITY");
        if (wtype != XCB_NONE && util != XCB_NONE)
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, wtype, XCB_ATOM_ATOM, 32, 1, &util);
    }

    const char *title = normal ? "ghost-normal" : "ghost-overlay";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);

    xcb_map_window(c, w);
    xcb_flush(c);

    printf("ghost_overlay: mapped window 0x%x (no WM_DELETE_WINDOW; will not self-exit)\n", w);
    fflush(stdout);

    // Idle forever. xcb_wait_for_event returns NULL only when the connection
    // breaks — i.e. when the WM force-kills us (xcb_kill_client). That is the
    // success signal for the *fixed* WM.
    xcb_generic_event_t *ev;
    while ((ev = xcb_wait_for_event(c)) != NULL)
        free(ev);

    xcb_disconnect(c);
    return 0;
}

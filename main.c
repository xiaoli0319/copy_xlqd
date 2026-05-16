#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xfixes.h>

static Display *dpy;
static Window win;
static int screen;
static GC gc;
static Atom wm_delete_window;

/* clipboard atoms */
static Atom clip_atom;
static Atom utf8_string;
static Atom prop_atom;
static int fixes_event_base;

/* clipboard history (circular buffer) */
#define MAX_HISTORY 20
#define MAX_TEXT_LEN 4096

typedef struct {
    char text[MAX_TEXT_LEN];
    int len;
} ClipItem;

static ClipItem history[MAX_HISTORY];
static int history_count = 0;
static int pending_selection = 0;

/* popup state */
static int popup_visible = 0;
static int popup_x, popup_y;

/* hotkey keycodes */
static KeyCode kc_shift_l, kc_shift_r, kc_alt_l, kc_alt_r, kc_space;
static int hotkey_was_down = 0;

static void render(void);
static void popup_show(void);
static void popup_hide(void);

static int create_window(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return -1;
    }

    screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    /* create popup window with override_redirect (no WM decorations) */
    XSetWindowAttributes attr;
    attr.override_redirect = True;
    attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                      FocusChangeMask | StructureNotifyMask;
    attr.background_pixel = WhitePixel(dpy, screen);
    attr.border_pixel = BlackPixel(dpy, screen);

    win = XCreateWindow(dpy, root, -2000, -2000, 600, 400, 1,
                        DefaultDepth(dpy, screen), InputOutput,
                        DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWEventMask |
                        CWBackPixel | CWBorderPixel,
                        &attr);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));

    /* map window off-screen */
    XMapWindow(dpy, win);

    /* clipboard atoms */
    clip_atom = XInternAtom(dpy, "CLIPBOARD", False);
    utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    prop_atom = XInternAtom(dpy, "COPY_XLQD_PROP", False);

    /* XFixes for clipboard monitoring */
    int fixes_error_base;
    if (!XFixesQueryExtension(dpy, &fixes_event_base, &fixes_error_base)) {
        fprintf(stderr, "XFixes extension not available\n");
        return -1;
    }

    XFixesSelectSelectionInput(dpy, win, clip_atom,
                               XFixesSetSelectionOwnerNotifyMask);

    /* cache hotkey keycodes */
    kc_shift_l = XKeysymToKeycode(dpy, XK_Shift_L);
    kc_shift_r = XKeysymToKeycode(dpy, XK_Shift_R);
    kc_alt_l   = XKeysymToKeycode(dpy, XK_Alt_L);
    kc_alt_r   = XKeysymToKeycode(dpy, XK_Alt_R);
    kc_space   = XKeysymToKeycode(dpy, XK_space);

    fprintf(stderr,
        "debug: Shift_L=%d Shift_R=%d Alt_L=%d Alt_R=%d Space=%d\n",
        kc_shift_l, kc_shift_r, kc_alt_l, kc_alt_r, kc_space);

    /* save center position for popup */
    popup_x = (DisplayWidth(dpy, screen) - 600) / 2;
    popup_y = (DisplayHeight(dpy, screen) - 400) / 3;

    return 0;
}

/* ---- clipboard ---- */

static void request_clipboard(void)
{
    if (pending_selection)
        return;

    Window owner = XGetSelectionOwner(dpy, clip_atom);
    if (owner == None || owner == win)
        return;

    XConvertSelection(dpy, clip_atom, utf8_string, prop_atom, win, CurrentTime);
    pending_selection = 1;
}

static void read_clipboard(void)
{
    pending_selection = 0;

    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int rc = XGetWindowProperty(dpy, win, prop_atom,
                                0, MAX_TEXT_LEN / 4, True,
                                AnyPropertyType,
                                &type, &format, &nitems, &bytes_after, &data);
    if (rc != Success || !data || nitems == 0) {
        if (data) XFree(data);
        return;
    }

    /* skip if same as last entry */
    if (history_count > 0) {
        int last = (history_count - 1) % MAX_HISTORY;
        if ((int)nitems == history[last].len &&
            memcmp(data, history[last].text, nitems) == 0) {
            XFree(data);
            return;
        }
    }

    int idx = history_count % MAX_HISTORY;
    ClipItem *item = &history[idx];
    int len = nitems;
    if (len >= MAX_TEXT_LEN)
        len = MAX_TEXT_LEN - 1;
    memcpy(item->text, data, len);
    item->text[len] = '\0';
    item->len = len;
    history_count++;

    printf("clipboard[%d]: %.*s\n", history_count - 1,
           len < 80 ? len : 80, item->text);

    XFree(data);
}

/* ---- hotkey detection ---- */

/* check key state using XQueryKeymap */
static int key_is_down(KeyCode kc)
{
    if (!kc) return 0;
    char keys[32];
    XQueryKeymap(dpy, keys);
    return (keys[kc / 8] & (1 << (kc % 8))) != 0;
}

static void check_hotkey(void)
{
    if (popup_visible)
        return;

    char keys[32];
    XQueryKeymap(dpy, keys);

    int s = (kc_shift_l && (keys[kc_shift_l/8] & (1 << (kc_shift_l%8))))
          || (kc_shift_r && (keys[kc_shift_r/8] & (1 << (kc_shift_r%8))));
    int a = (kc_alt_l   && (keys[kc_alt_l/8]   & (1 << (kc_alt_l%8))))
          || (kc_alt_r   && (keys[kc_alt_r/8]   & (1 << (kc_alt_r%8))));
    int sp = kc_space && (keys[kc_space/8] & (1 << (kc_space%8)));

    /* debug: print when shift+alt are held but space toggles */
    if (s && a) {
        static int last_sp = -1;
        if (sp != last_sp) {
            fprintf(stderr, "debug: hold Shift+Alt, Space=%d  (keymap byte[8]=0x%02x)\n",
                    sp, (unsigned char)keys[8]);
            last_sp = sp;
        }
    }

    if (s && a && sp) {
        if (!hotkey_was_down) {
            fprintf(stderr, "debug: HOTKEY DETECTED\n");
            popup_show();
        }
        hotkey_was_down = 1;
    } else {
        hotkey_was_down = 0;
    }
}

/* ---- popup ---- */

static void popup_show(void)
{
    if (popup_visible)
        return;

    XMoveWindow(dpy, win, popup_x, popup_y);
    XRaiseWindow(dpy, win);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    XFlush(dpy);
    popup_visible = 1;

    render();

    XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}

static void popup_hide(void)
{
    if (!popup_visible)
        return;

    XUngrabKeyboard(dpy, CurrentTime);
    XMoveWindow(dpy, win, -2000, -2000);
    XFlush(dpy);
    popup_visible = 0;
}

/* ---- render ---- */

static void render(void)
{
    char buf[512];
    int y = 20;

    XClearWindow(dpy, win);

    XDrawString(dpy, win, gc, 20, y,
                "copy_xlqd  \342\200\224  select [1-9], ESC to dismiss", 46);
    y += 25;

    XDrawLine(dpy, win, gc, 20, y, 580, y);
    y += 10;

    if (history_count == 0) {
        XDrawString(dpy, win, gc, 20, y, "(empty)", 7);
        return;
    }

    int n = history_count < MAX_HISTORY ? history_count : MAX_HISTORY;
    int end = n;
    int start = n > 9 ? n - 9 : 0;

    for (int i = end - 1; i >= start; i--) {
        int idx = i % MAX_HISTORY;
        ClipItem *item = &history[idx];
        int slen = item->len < 55 ? item->len : 55;

        int num = end - 1 - i + 1;
        snprintf(buf, sizeof(buf), "[%d] %.*s", num, slen, item->text);
        for (char *p = buf; *p; p++)
            if (*p == '\n') *p = ' ';
        XDrawString(dpy, win, gc, 20, y, buf, strlen(buf));
        y += 18;
    }
}

/* ---- main ---- */

int main(void)
{
    if (create_window() < 0)
        return 1;

    request_clipboard();
    fprintf(stderr, "debug: entering main loop (hotkey: Shift+Alt+Space)\n");

    XEvent ev;
    int running = 1;

    while (running) {
        /* handle pending X events */
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            if (ev.type == fixes_event_base + XFixesSelectionNotify) {
                request_clipboard();
                continue;
            }

            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0 && popup_visible)
                    render();
                break;

            case SelectionNotify:
                read_clipboard();
                if (popup_visible)
                    render();
                break;

            case KeyPress:
            case KeyRelease: {
                if (!popup_visible)
                    break;

                if (ev.type == KeyRelease)
                    break;

                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                char buf[8] = {0};
                XLookupString(&ev.xkey, buf, sizeof(buf), NULL, NULL);

                if (ks == XK_Escape) {
                    printf("dismiss\n");
                    popup_hide();
                    break;
                }

                if (buf[0] >= '1' && buf[0] <= '9') {
                    int num = buf[0] - '0';
                    int n = history_count < MAX_HISTORY ? history_count : MAX_HISTORY;
                    if (num <= n) {
                        int idx = (history_count - num) % MAX_HISTORY;
                        printf("selected [%d]: %.*s\n", num,
                               history[idx].len < 80 ? history[idx].len : 80,
                               history[idx].text);
                    }
                    popup_hide();
                    break;
                }

                printf("key: keysym=0x%lx char='%s'\n", (unsigned long)ks, buf);
                break;
            }

            case FocusOut:
                if (popup_visible)
                    popup_hide();
                break;

            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete_window)
                    running = 0;
                break;

            case DestroyNotify:
                running = 0;
                break;
            }
        }

        /* poll hotkey */
        if (!popup_visible)
            check_hotkey();

        /* sleep to avoid busy-loop */
        usleep(30000); /* 30ms ≈ 33Hz polling */
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

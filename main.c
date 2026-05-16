#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xft/Xft.h>          /* UTF-8 / CJK rendering */

/* ------------------------------------------------------------------ */
/*  IPC / single-instance                                              */
/* ------------------------------------------------------------------ */

#define PID_FILE "/tmp/copy_xlqd.pid"

static int sig_pipe[2] = {-1, -1};

static void handle_sigusr1(int sig)
{
    (void)sig;
    char b = 1;
    (void)!write(sig_pipe[1], &b, 1);
}

static int pidfile_write(void)
{
    int fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open " PID_FILE); return -1; }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, n) < 0) { perror("write " PID_FILE); close(fd); return -1; }
    close(fd);
    return 0;
}

static void pidfile_remove(void) { unlink(PID_FILE); }

static pid_t pidfile_read(void)
{
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return -1;
    pid_t pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);
    if (pid <= 0) return -1;
    if (kill(pid, 0) < 0) {
        if (errno == ESRCH) { fprintf(stderr, "info: stale PID file removed\n"); unlink(PID_FILE); }
        return -1;
    }
    return pid;
}

static void do_toggle_and_exit(void)
{
    pid_t pid = pidfile_read();
    if (pid < 0) { fprintf(stderr, "copy_xlqd: no running instance found (%s)\n", PID_FILE); exit(1); }
    if (kill(pid, SIGUSR1) < 0) { perror("kill SIGUSR1"); exit(1); }
    printf("copy_xlqd: sent SIGUSR1 to pid %d\n", (int)pid);
    exit(0);
}

/* ------------------------------------------------------------------ */
/*  X11 globals                                                        */
/* ------------------------------------------------------------------ */

static Display  *dpy;
static Window    win;
static int       screen;
static GC        gc;
static Atom      wm_delete_window;

/* clipboard atoms */
static Atom  clip_atom;
static Atom  utf8_string;
static Atom  prop_atom;
static Atom  targets_atom;
static int   fixes_event_base;

/* Xft (UTF-8 text rendering) */
static XftFont  *xft_font;
static XftDraw  *xft_draw;
static XftColor  xft_fg;
static int       line_h;    /* pixels per row */

/* clipboard history */
#define MAX_HISTORY  20
#define MAX_TEXT_LEN 4096

typedef struct { char text[MAX_TEXT_LEN]; int len; } ClipItem;

static ClipItem history[MAX_HISTORY];
static int      history_count     = 0;
static int      pending_selection = 0;

/* clipboard write-back */
static char  owned_text[MAX_TEXT_LEN];
static int   owned_len = 0;

/* popup */
static int popup_visible = 0;
static int popup_x, popup_y;

/* ------------------------------------------------------------------ */
/*  create_window                                                      */
/* ------------------------------------------------------------------ */

static void render(void);
static void popup_show(void);
static void popup_hide(void);
static void popup_toggle(void);

static int create_window(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Cannot open display\n"); return -1; }

    screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XSetWindowAttributes attr;
    attr.override_redirect = True;
    /* NOTE: SelectionNotify / SelectionRequest arrive regardless of mask,
     * but we must NOT include them here — they are not maskable per ICCCM. */
    attr.event_mask       = ExposureMask | KeyPressMask | KeyReleaseMask |
                            FocusChangeMask | StructureNotifyMask;
    attr.background_pixel = WhitePixel(dpy, screen);
    attr.border_pixel     = BlackPixel(dpy, screen);

    win = XCreateWindow(dpy, root, -2000, -2000, 620, 420, 1,
                        DefaultDepth(dpy, screen), InputOutput,
                        DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWEventMask |
                        CWBackPixel | CWBorderPixel, &attr);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));

    XMapWindow(dpy, win);

    /* atoms */
    clip_atom    = XInternAtom(dpy, "CLIPBOARD",      False);
    utf8_string  = XInternAtom(dpy, "UTF8_STRING",    False);
    prop_atom    = XInternAtom(dpy, "COPY_XLQD_PROP", False);
    targets_atom = XInternAtom(dpy, "TARGETS",        False);

    /* XFixes */
    int fixes_error_base;
    if (!XFixesQueryExtension(dpy, &fixes_event_base, &fixes_error_base)) {
        fprintf(stderr, "XFixes extension not available\n"); return -1;
    }
    XFixesSelectSelectionInput(dpy, win, clip_atom,
                               XFixesSetSelectionOwnerNotifyMask);

    /* Xft font — prefer monospace, fall back to any fixed */
    xft_font = XftFontOpenName(dpy, screen, "monospace:size=12");
    if (!xft_font)
        xft_font = XftFontOpenName(dpy, screen, "fixed:size=12");
    if (!xft_font) { fprintf(stderr, "Cannot open Xft font\n"); return -1; }

    line_h = xft_font->ascent + xft_font->descent + 4;

    xft_draw = XftDrawCreate(dpy, win,
                             DefaultVisual(dpy, screen),
                             DefaultColormap(dpy, screen));
    if (!xft_draw) { fprintf(stderr, "XftDrawCreate failed\n"); return -1; }

    XRenderColor rc = { 0, 0, 0, 0xffff };   /* opaque black */
    XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                       DefaultColormap(dpy, screen), &rc, &xft_fg);

    popup_x = (DisplayWidth(dpy,  screen) - 620) / 2;
    popup_y = (DisplayHeight(dpy, screen) - 420) / 3;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  clipboard read                                                     */
/* ------------------------------------------------------------------ */

static void request_clipboard(void)
{
    if (pending_selection) return;
    Window owner = XGetSelectionOwner(dpy, clip_atom);
    if (owner == None || owner == win) return;
    XConvertSelection(dpy, clip_atom, utf8_string, prop_atom, win, CurrentTime);
    pending_selection = 1;
}

static void read_clipboard(void)
{
    pending_selection = 0;

    Atom type; int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int rc = XGetWindowProperty(dpy, win, prop_atom,
                                0, MAX_TEXT_LEN / 4, True,
                                AnyPropertyType,
                                &type, &format, &nitems, &bytes_after, &data);
    if (rc != Success || !data || nitems == 0) { if (data) XFree(data); return; }

    /* skip duplicates */
    if (history_count > 0) {
        int last = (history_count - 1) % MAX_HISTORY;
        if ((int)nitems == history[last].len &&
            memcmp(data, history[last].text, nitems) == 0) {
            XFree(data); return;
        }
    }

    int idx = history_count % MAX_HISTORY;
    int len = (int)nitems < MAX_TEXT_LEN - 1 ? (int)nitems : MAX_TEXT_LEN - 1;
    memcpy(history[idx].text, data, len);
    history[idx].text[len] = '\0';
    history[idx].len = len;
    history_count++;

    fprintf(stderr, "clipboard[%d]: %.*s\n", history_count - 1,
            len < 80 ? len : 80, history[idx].text);
    XFree(data);
}

/* ------------------------------------------------------------------ */
/*  clipboard write-back (selection owner protocol)                   */
/* ------------------------------------------------------------------ */

/*
 * Claim CLIPBOARD ownership.  Must be called with a valid event timestamp
 * (not CurrentTime) to avoid the server rejecting the request.
 */
static void clipboard_claim(const char *text, int len, Time t)
{
    if (len >= MAX_TEXT_LEN) len = MAX_TEXT_LEN - 1;
    memcpy(owned_text, text, len);
    owned_text[len] = '\0';
    owned_len = len;

    XSetSelectionOwner(dpy, clip_atom, win, t);
    XFlush(dpy);

    /* Verify immediately with a round-trip */
    if (XGetSelectionOwner(dpy, clip_atom) == win)
        fprintf(stderr, "info: CLIPBOARD owned, len=%d\n", owned_len);
    else
        fprintf(stderr, "warn: XSetSelectionOwner failed\n");
}

/*
 * Respond to a SelectionRequest from another application.
 * Called from the event loop when ev.type == SelectionRequest.
 */
static void handle_selection_request(XEvent *ev)
{
    XSelectionRequestEvent *req = &ev->xselectionrequest;

    fprintf(stderr, "debug: SelectionRequest target=%lu prop=%lu owned_len=%d\n",
            (unsigned long)req->target,
            (unsigned long)req->property,
            owned_len);

    /* ICCCM §2.2: if property is None, use target atom as property name */
    Atom reply_prop = (req->property != None) ? req->property : req->target;

    XEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.xselection.type      = SelectionNotify;
    reply.xselection.display   = dpy;
    reply.xselection.requestor = req->requestor;
    reply.xselection.selection = req->selection;
    reply.xselection.target    = req->target;
    reply.xselection.time      = req->time;
    reply.xselection.property  = None;   /* assume failure */

    if (req->target == targets_atom) {
        /* Advertise what we can convert to */
        Atom supported[] = { targets_atom, utf8_string, XA_STRING };
        XChangeProperty(dpy, req->requestor, reply_prop,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported,
                        (int)(sizeof(supported) / sizeof(Atom)));
        reply.xselection.property = reply_prop;

    } else if (req->target == utf8_string) {
        XChangeProperty(dpy, req->requestor, reply_prop,
                        utf8_string, 8, PropModeReplace,
                        (unsigned char *)owned_text, owned_len);
        reply.xselection.property = reply_prop;

    } else if (req->target == XA_STRING) {
        /* XA_STRING expects Latin-1; send as-is (best-effort) */
        XChangeProperty(dpy, req->requestor, reply_prop,
                        XA_STRING, 8, PropModeReplace,
                        (unsigned char *)owned_text, owned_len);
        reply.xselection.property = reply_prop;
    }

    XSendEvent(dpy, req->requestor, False, 0, &reply);
    XFlush(dpy);

    fprintf(stderr, "debug: SelectionNotify sent, property=%lu\n",
            (unsigned long)reply.xselection.property);
}

/* ------------------------------------------------------------------ */
/*  popup                                                              */
/* ------------------------------------------------------------------ */

static void popup_show(void)
{
    if (popup_visible) return;

    XMoveWindow(dpy, win, popup_x, popup_y);
    XRaiseWindow(dpy, win);
    XSync(dpy, False);

    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

    popup_visible = 1;
    render();
    XFlush(dpy);

    int grab = XGrabKeyboard(dpy, win, True,
                             GrabModeAsync, GrabModeAsync, CurrentTime);
    if (grab != GrabSuccess) {
        usleep(20000);
        grab = XGrabKeyboard(dpy, win, True,
                             GrabModeAsync, GrabModeAsync, CurrentTime);
    }
    if (grab != GrabSuccess)
        fprintf(stderr, "warn: XGrabKeyboard failed (%d)\n", grab);

    fprintf(stderr, "info: popup shown\n");
}

static void popup_hide(void)
{
    if (!popup_visible) return;
    XUngrabKeyboard(dpy, CurrentTime);
    XMoveWindow(dpy, win, -2000, -2000);
    XFlush(dpy);
    popup_visible = 0;
    fprintf(stderr, "info: popup hidden\n");
}

static void popup_toggle(void)
{
    if (popup_visible) popup_hide();
    else               popup_show();
}

/* ------------------------------------------------------------------ */
/*  render  (Xft — handles UTF-8 / CJK)                               */
/* ------------------------------------------------------------------ */

/* Draw a UTF-8 string via Xft */
static void xft_draw_utf8(int x, int y, const char *s, int len)
{
    if (len <= 0) return;
    XftDrawStringUtf8(xft_draw, &xft_fg, xft_font,
                      x, y + xft_font->ascent,
                      (const FcChar8 *)s, len);
}

static void render(void)
{
    char buf[640];
    int  y = 10;

    XClearWindow(dpy, win);

    /* title */
    const char *title = "copy_xlqd  —  [1-9] select  ESC dismiss";
    xft_draw_utf8(20, y, title, strlen(title));
    y += line_h;
    XDrawLine(dpy, win, gc, 20, y + xft_font->descent, 600, y + xft_font->descent);
    y += 6;

    if (history_count == 0) {
        xft_draw_utf8(20, y, "(empty)", 7);
        return;
    }

    int n     = history_count < MAX_HISTORY ? history_count : MAX_HISTORY;
    int start = n > 9 ? n - 9 : 0;

    for (int i = n - 1; i >= start; i--) {
        int      idx  = i % MAX_HISTORY;
        ClipItem *item = &history[idx];
        int      num  = n - 1 - i + 1;

        /* Build "[N] " prefix */
        int prefix_len = snprintf(buf, sizeof(buf), "[%d] ", num);

        /* Append up to ~56 bytes of text, replacing newlines with spaces */
        int copy_len = item->len;
        if (copy_len > (int)(sizeof(buf) - prefix_len - 1))
            copy_len = (int)(sizeof(buf) - prefix_len - 1);
        memcpy(buf + prefix_len, item->text, copy_len);
        buf[prefix_len + copy_len] = '\0';
        for (int k = prefix_len; k < prefix_len + copy_len; k++)
            if ((unsigned char)buf[k] < 0x20 && buf[k] != '\0') buf[k] = ' ';

        xft_draw_utf8(20, y, buf, strlen(buf));
        y += line_h;
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--toggle") == 0)
        do_toggle_and_exit();

    pid_t existing = pidfile_read();
    if (existing > 0) {
        fprintf(stderr,
                "copy_xlqd: already running (pid %d).\n"
                "  toggle: copy_xlqd --toggle  |  kill -USR1 %d\n",
                (int)existing, (int)existing);
        return 1;
    }

    if (pipe(sig_pipe) < 0) { perror("pipe"); return 1; }
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    if (pidfile_write() < 0) return 1;
    atexit(pidfile_remove);

    if (create_window() < 0) return 1;

    request_clipboard();
    fprintf(stderr, "info: copy_xlqd running (pid %d)\n", (int)getpid());

    int x11_fd  = ConnectionNumber(dpy);
    XEvent ev;
    int running = 1;

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd,      &rfds);
        FD_SET(sig_pipe[0], &rfds);
        int maxfd = x11_fd > sig_pipe[0] ? x11_fd : sig_pipe[0];

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        /* SIGUSR1 → toggle popup */
        if (FD_ISSET(sig_pipe[0], &rfds)) {
            char drain[16];
            { ssize_t _r = read(sig_pipe[0], drain, sizeof(drain)); (void)_r; }
            fprintf(stderr, "info: SIGUSR1 → toggle\n");
            popup_toggle();
        }

        /* X11 events */
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);

            if (ev.type == fixes_event_base + XFixesSelectionNotify) {
                request_clipboard();
                continue;
            }

            switch (ev.type) {

            case Expose:
                if (ev.xexpose.count == 0 && popup_visible) render();
                break;

            case SelectionNotify:
                read_clipboard();
                if (popup_visible) render();
                break;

            case SelectionRequest:
                handle_selection_request(&ev);
                break;

            case SelectionClear:
                owned_len = 0;
                owned_text[0] = '\0';
                fprintf(stderr, "info: lost CLIPBOARD ownership\n");
                break;

            case KeyPress: {
                if (!popup_visible) break;

                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                char   kbuf[8] = {0};
                XLookupString(&ev.xkey, kbuf, sizeof(kbuf), NULL, NULL);

                if (ks == XK_Escape) {
                    popup_hide();
                    break;
                }

                if (kbuf[0] >= '1' && kbuf[0] <= '9') {
                    int num = kbuf[0] - '0';
                    int n   = history_count < MAX_HISTORY
                              ? history_count : MAX_HISTORY;
                    if (num <= n) {
                        int idx = (history_count - num) % MAX_HISTORY;
                        /* Claim clipboard BEFORE hiding the popup so we
                         * still hold the keyboard grab at claim time */
                        clipboard_claim(history[idx].text,
                                        history[idx].len,
                                        ev.xkey.time);
                    }
                    popup_hide();
                    break;
                }
                break;
            }

            case FocusOut:
                if (popup_visible &&
                    ev.xfocus.mode != NotifyGrab &&
                    ev.xfocus.mode != NotifyUngrab)
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
    }

    XftDrawDestroy(xft_draw);
    XftColorFree(dpy, DefaultVisual(dpy, screen),
                 DefaultColormap(dpy, screen), &xft_fg);
    XftFontClose(dpy, xft_font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
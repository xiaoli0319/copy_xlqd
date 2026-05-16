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

/* ------------------------------------------------------------------ */
/*  IPC / single-instance                                              */
/* ------------------------------------------------------------------ */

#define PID_FILE "/tmp/copy_xlqd.pid"

/* self-pipe: signal handler writes here, select() reads here */
static int sig_pipe[2] = {-1, -1};

static void handle_sigusr1(int sig)
{
    (void)sig;
    char b = 1;
    (void)!write(sig_pipe[1], &b, 1);   /* async-signal-safe; error unrecoverable here */
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

static void pidfile_remove(void)
{
    unlink(PID_FILE);
}

/*
 * Read PID from PID_FILE.
 * Returns pid on success, -1 if missing or stale (stale file removed).
 */
static pid_t pidfile_read(void)
{
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return -1;

    pid_t pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);

    if (pid <= 0) return -1;

    if (kill(pid, 0) < 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "info: stale PID file removed\n");
            unlink(PID_FILE);
        }
        return -1;
    }
    return pid;
}

/* Send SIGUSR1 to the running instance and exit. */
static void do_toggle_and_exit(void)
{
    pid_t pid = pidfile_read();
    if (pid < 0) {
        fprintf(stderr, "copy_xlqd: no running instance found (%s)\n", PID_FILE);
        exit(1);
    }
    if (kill(pid, SIGUSR1) < 0) { perror("kill SIGUSR1"); exit(1); }
    printf("copy_xlqd: sent SIGUSR1 to pid %d\n", (int)pid);
    exit(0);
}

/* ------------------------------------------------------------------ */
/*  X11 / clipboard / popup                                            */
/* ------------------------------------------------------------------ */

static Display *dpy;
static Window   win;
static int      screen;
static GC       gc;
static Atom     wm_delete_window;

static Atom clip_atom;
static Atom utf8_string;
static Atom prop_atom;
static int  fixes_event_base;

#define MAX_HISTORY  20
#define MAX_TEXT_LEN 4096

typedef struct {
    char text[MAX_TEXT_LEN];
    int  len;
} ClipItem;

static ClipItem history[MAX_HISTORY];
static int      history_count     = 0;
static int      pending_selection = 0;

static int popup_visible = 0;
static int popup_x, popup_y;

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
    attr.event_mask        = ExposureMask | KeyPressMask | KeyReleaseMask |
                             FocusChangeMask | StructureNotifyMask;
    attr.background_pixel  = WhitePixel(dpy, screen);
    attr.border_pixel      = BlackPixel(dpy, screen);

    win = XCreateWindow(dpy, root, -2000, -2000, 600, 400, 1,
                        DefaultDepth(dpy, screen), InputOutput,
                        DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWEventMask |
                        CWBackPixel | CWBorderPixel, &attr);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));

    XMapWindow(dpy, win);

    clip_atom   = XInternAtom(dpy, "CLIPBOARD",      False);
    utf8_string = XInternAtom(dpy, "UTF8_STRING",    False);
    prop_atom   = XInternAtom(dpy, "COPY_XLQD_PROP", False);

    int fixes_error_base;
    if (!XFixesQueryExtension(dpy, &fixes_event_base, &fixes_error_base)) {
        fprintf(stderr, "XFixes extension not available\n");
        return -1;
    }
    XFixesSelectSelectionInput(dpy, win, clip_atom,
                               XFixesSetSelectionOwnerNotifyMask);

    popup_x = (DisplayWidth(dpy,  screen) - 600) / 2;
    popup_y = (DisplayHeight(dpy, screen) - 400) / 3;
    return 0;
}

/* ---- clipboard ---- */

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
    if (rc != Success || !data || nitems == 0) {
        if (data) XFree(data);
        return;
    }

    if (history_count > 0) {
        int last = (history_count - 1) % MAX_HISTORY;
        if ((int)nitems == history[last].len &&
            memcmp(data, history[last].text, nitems) == 0) {
            XFree(data); return;
        }
    }

    int idx = history_count % MAX_HISTORY;
    int len = (int)nitems;
    if (len >= MAX_TEXT_LEN) len = MAX_TEXT_LEN - 1;

    memcpy(history[idx].text, data, len);
    history[idx].text[len] = '\0';
    history[idx].len = len;
    history_count++;

    printf("clipboard[%d]: %.*s\n", history_count - 1,
           len < 80 ? len : 80, history[idx].text);
    XFree(data);
}

/* ---- popup ---- */

static void popup_show(void)
{
    if (popup_visible) return;

    /* 1. Move and raise, then sync so the server has processed the
     *    geometry change before we request focus or a keyboard grab. */
    XMoveWindow(dpy, win, popup_x, popup_y);
    XRaiseWindow(dpy, win);
    XSync(dpy, False);

    /* 2. Request focus (may produce a harmless BadMatch on some WMs). */
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

    popup_visible = 1;

    /* 3. Draw and flush so pixels actually reach the screen. */
    render();
    XFlush(dpy);

    /* 4. Grab keyboard; retry once after a short wait if it fails
     *    (AlreadyGrabbed / GrabNotViewable can occur transiently). */
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

/* ---- render ---- */

static void render(void)
{
    char buf[512];
    int  y = 20;

    XClearWindow(dpy, win);
    XDrawString(dpy, win, gc, 20, y,
                "copy_xlqd  \342\200\224  [1-9] select, ESC dismiss", 41);
    y += 25;
    XDrawLine(dpy, win, gc, 20, y, 580, y);
    y += 10;

    if (history_count == 0) {
        XDrawString(dpy, win, gc, 20, y, "(empty)", 7);
        return;
    }

    int n     = history_count < MAX_HISTORY ? history_count : MAX_HISTORY;
    int start = n > 9 ? n - 9 : 0;

    for (int i = n - 1; i >= start; i--) {
        int      idx  = i % MAX_HISTORY;
        ClipItem *item = &history[idx];
        int      slen = item->len < 55 ? item->len : 55;
        int      num  = n - 1 - i + 1;

        snprintf(buf, sizeof(buf), "[%d] %.*s", num, slen, item->text);
        for (char *p = buf; *p; p++)
            if (*p == '\n') *p = ' ';
        XDrawString(dpy, win, gc, 20, y, buf, strlen(buf));
        y += 18;
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* ── --toggle: forward to running instance ── */
    if (argc >= 2 && strcmp(argv[1], "--toggle") == 0)
        do_toggle_and_exit();       /* never returns */

    /* ── single-instance guard ── */
    pid_t existing = pidfile_read();
    if (existing > 0) {
        fprintf(stderr,
                "copy_xlqd: already running (pid %d).\n"
                "  Toggle popup: copy_xlqd --toggle\n"
                "            or: kill -USR1 %d\n",
                (int)existing, (int)existing);
        return 1;
    }

    /* ── self-pipe (signal → select wakeup) ── */
    if (pipe(sig_pipe) < 0) { perror("pipe"); return 1; }
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);   /* non-blocking write end */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    /* ── PID file ── */
    if (pidfile_write() < 0) return 1;
    atexit(pidfile_remove);

    /* ── X11 ── */
    if (create_window() < 0) return 1;

    request_clipboard();
    fprintf(stderr,
            "info: copy_xlqd running (pid %d)\n"
            "  toggle popup: copy_xlqd --toggle  |  kill -USR1 %d\n",
            (int)getpid(), (int)getpid());

    int x11_fd = ConnectionNumber(dpy);
    XEvent ev;
    int running = 1;

    while (running) {
        /*
         * select() on two fds:
         *   x11_fd      — X server has events pending
         *   sig_pipe[0] — SIGUSR1 arrived (written by handle_sigusr1)
         */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd,      &rfds);
        FD_SET(sig_pipe[0], &rfds);
        int maxfd = x11_fd > sig_pipe[0] ? x11_fd : sig_pipe[0];

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        /* ── SIGUSR1 via self-pipe → toggle ── */
        if (FD_ISSET(sig_pipe[0], &rfds)) {
            char drain[16];
            { ssize_t _r = read(sig_pipe[0], drain, sizeof(drain)); (void)_r; }
            fprintf(stderr, "info: SIGUSR1 → toggle\n");
            popup_toggle();
        }

        /* ── X11 events ── */
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

            case KeyPress: {
                if (!popup_visible) break;

                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                char   kbuf[8] = {0};
                XLookupString(&ev.xkey, kbuf, sizeof(kbuf), NULL, NULL);

                if (ks == XK_Escape) {
                    printf("dismiss\n");
                    popup_hide();
                    break;
                }
                if (kbuf[0] >= '1' && kbuf[0] <= '9') {
                    int num = kbuf[0] - '0';
                    int n   = history_count < MAX_HISTORY
                              ? history_count : MAX_HISTORY;
                    if (num <= n) {
                        int idx = (history_count - num) % MAX_HISTORY;
                        printf("selected [%d]: %.*s\n", num,
                               history[idx].len < 80 ? history[idx].len : 80,
                               history[idx].text);
                    }
                    popup_hide();
                    break;
                }
                break;
            }

            case FocusOut:
                /* XGrabKeyboard/XUngrabKeyboard each fire a synthetic
                 * FocusOut (mode=NotifyGrab / NotifyUngrab) to the grab
                 * window itself.  Hiding on those events would close the
                 * popup the instant it opens.  Only react to genuine focus
                 * losses (mode=NotifyNormal, NotifyWhileGrabbed). */
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

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
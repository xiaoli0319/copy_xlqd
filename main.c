#include "common.h"

/* ------------------------------------------------------------------ */
/*  global variable definitions                                        */
/* ------------------------------------------------------------------ */

/* IPC */
int sig_pipe[2] = {-1, -1};

/* X11 */
Display *dpy;
Window   win;
int      screen;
GC       gc;
Atom     wm_delete_window;
Atom     clip_atom;
Atom     utf8_string;
Atom     prop_atom;
Atom     targets_atom;
int      fixes_event_base;

/* Xft */
XftFont  *xft_font;
XftDraw  *xft_draw;
XftColor  xft_fg;
XftColor  xft_fg_white;
XftColor  xft_fg_hl;
int       line_h;

/* XIM */
XIM xim;
XIC xic;

/* clipboard history */
ClipItem history[MAX_HISTORY];
int      history_count = 0;
int      pending_selection = 0;

/* clipboard write-back */
char owned_text[MAX_TEXT_LEN];
int  owned_len = 0;

/* search */
char search_text[MAX_TEXT_LEN];
int  search_len = 0;
int  filtered_idx[MAX_HISTORY];
int  filtered_num[MAX_HISTORY];
int  filtered_count = 0;

/* popup */
int      popup_visible = 0;
int      popup_x;
int      popup_y;
int      scroll_offset = 0;
int      selected_idx = 0;
Window   prev_focus_win = None;

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--toggle") == 0)
        do_toggle_and_exit();

    pid_t existing = pidfile_read();
    if (existing > 0)
    {
        fprintf(stderr,
                "copy_xlqd: already running (pid %d).\n"
                "  toggle: copy_xlqd --toggle  |  kill -USR1 %d\n",
                (int)existing, (int)existing);
        return 1;
    }

    if (pipe(sig_pipe) < 0)
    {
        perror("pipe");
        return 1;
    }
    fcntl(sig_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    if (pidfile_write() < 0)
        return 1;
    atexit(pidfile_remove);

    if (create_window() < 0)
        return 1;

    load_history_from_file();

    request_clipboard();
    fprintf(stderr, "info: copy_xlqd running (pid %d)\n", (int)getpid());

    int x11_fd = ConnectionNumber(dpy);
    XEvent ev;
    int running = 1;

    while (running)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd, &rfds);
        FD_SET(sig_pipe[0], &rfds);
        int maxfd = x11_fd > sig_pipe[0] ? x11_fd : sig_pipe[0];

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0)
        {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* SIGUSR1 → toggle popup */
        if (FD_ISSET(sig_pipe[0], &rfds))
        {
            char drain[16];
            {
                ssize_t _r = read(sig_pipe[0], drain, sizeof(drain));
                (void)_r;
            }
            fprintf(stderr, "info: SIGUSR1 \xe2\x86\x92 toggle\n");
            popup_toggle();
        }

        /* X11 events */
        while (XPending(dpy))
        {
            XNextEvent(dpy, &ev);

            /* let input method filter compose events first */
            if (xic && XFilterEvent(&ev, win))
                continue;

            if (ev.type == fixes_event_base + XFixesSelectionNotify)
            {
                request_clipboard();
                continue;
            }

            switch (ev.type)
            {

            case Expose:
                if (ev.xexpose.count == 0 && popup_visible)
                    render();
                break;

            case SelectionNotify:
                read_clipboard();
                if (popup_visible)
                {
                    search_len    = 0;
                    search_text[0] = '\0';
                    filter_history();
                    scroll_offset = 0;
                    selected_idx  = 0;
                    render();
                }
                break;

            case SelectionRequest:
                handle_selection_request(&ev);
                break;

            case SelectionClear:
                owned_len    = 0;
                owned_text[0] = '\0';
                fprintf(stderr, "info: lost CLIPBOARD ownership\n");
                break;

            case KeyPress:
            {
                if (!popup_visible)
                    break;

                KeySym ks = XLookupKeysym(&ev.xkey, 0);

                if (ks == XK_Escape)
                {
                    if (search_len > 0)
                    {
                        search_len    = 0;
                        search_text[0] = '\0';
                        filter_history();
                        render();
                    }
                    else
                    {
                        popup_hide();
                    }
                    break;
                }

                if (ks == XK_BackSpace)
                {
                    if (search_len > 0)
                    {
                        do {
                            search_len--;
                        } while (search_len > 0 &&
                                 ((unsigned char)search_text[search_len]
                                  & 0xC0) == 0x80);
                        search_text[search_len] = '\0';
                        filter_history();
                        render();
                    }
                    break;
                }

                if (ks == XK_Down)
                {
                    if (filtered_count == 0) break;
                    int max_scroll = (filtered_count > DISPLAY_ROWS)
                                     ? (filtered_count - DISPLAY_ROWS) : 0;
                    int max_selected =
                        (filtered_count - scroll_offset > DISPLAY_ROWS)
                        ? (DISPLAY_ROWS - 1)
                        : (filtered_count - scroll_offset - 1);

                    if (selected_idx < max_selected)
                        selected_idx++;
                    else if (scroll_offset < max_scroll)
                        scroll_offset++;
                    render();
                    break;
                }

                if (ks == XK_Up)
                {
                    if (filtered_count == 0) break;
                    if (selected_idx > 0)
                        selected_idx--;
                    else if (scroll_offset > 0)
                        scroll_offset--;
                    render();
                    break;
                }

                if (ks == XK_Return)
                {
                    if (filtered_count == 0) break;
                    int sel = scroll_offset + selected_idx;
                    if (sel < filtered_count)
                    {
                        int idx = filtered_idx[sel];
                        clipboard_claim(history[idx].text,
                                        history[idx].len,
                                        ev.xkey.time);
                    }
                    popup_hide();
                    break;
                }

                /* Regular character input → fuzzy search */
                {
                    char buf[16] = {0};
                    int  len;

                    if (xic)
                    {
                        Status st;
                        KeySym kx;
                        len = Xutf8LookupString(xic, &ev.xkey,
                                                buf, sizeof(buf) - 1,
                                                &kx, &st);
                        if (st == XBufferOverflow) break;
                        if (st == XLookupKeySym || len <= 0) break;
                    }
                    else
                    {
                        KeySym dummy;
                        len = XLookupString(&ev.xkey,
                                            buf, sizeof(buf),
                                            &dummy, NULL);
                    }

                    if (len > 0 && search_len + len < MAX_TEXT_LEN)
                    {
                        memcpy(search_text + search_len, buf, len);
                        search_len += len;
                        search_text[search_len] = '\0';
                        filter_history();
                        render();
                    }
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
    XftColorFree(dpy, DefaultVisual(dpy, screen),
                 DefaultColormap(dpy, screen), &xft_fg_white);
    XftColorFree(dpy, DefaultVisual(dpy, screen),
                 DefaultColormap(dpy, screen), &xft_fg_hl);
    XftFontClose(dpy, xft_font);
    XFreeGC(dpy, gc);
    if (xic) XDestroyIC(xic);
    if (xim) XCloseIM(xim);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

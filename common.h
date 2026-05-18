#ifndef COPY_XLQD_COMMON_H
#define COPY_XLQD_COMMON_H

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
#include <X11/Xft/Xft.h>

/* ------------------------------------------------------------------ */
/*  constants                                                          */
/* ------------------------------------------------------------------ */

#define MAX_HISTORY  500
#define MAX_TEXT_LEN 4096
#define DISPLAY_ROWS 9

/* ------------------------------------------------------------------ */
/*  types                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    char text[MAX_TEXT_LEN];
    int  len;
} ClipItem;

/* ------------------------------------------------------------------ */
/*  globals (defined in main.c)                                        */
/* ------------------------------------------------------------------ */

/* IPC */
extern int sig_pipe[2];

/* X11 */
extern Display *dpy;
extern Window   win;
extern int      screen;
extern GC       gc;
extern Atom     wm_delete_window;
extern Atom     clip_atom;
extern Atom     utf8_string;
extern Atom     prop_atom;
extern Atom     targets_atom;
extern int      fixes_event_base;

/* Xft */
extern XftFont  *xft_font;
extern XftDraw  *xft_draw;
extern XftColor  xft_fg;
extern XftColor  xft_fg_white;
extern XftColor  xft_fg_hl;
extern int       line_h;

/* XIM */
extern XIM xim;
extern XIC xic;

/* clipboard history */
extern ClipItem history[MAX_HISTORY];
extern int      history_count;
extern int      pending_selection;

/* clipboard write-back */
extern char owned_text[MAX_TEXT_LEN];
extern int  owned_len;

/* search */
extern char search_text[MAX_TEXT_LEN];
extern int  search_len;
extern int  filtered_idx[MAX_HISTORY];
extern int  filtered_num[MAX_HISTORY];
extern int  filtered_count;

/* popup */
extern int      popup_visible;
extern int      popup_x;
extern int      popup_y;
extern int      scroll_offset;
extern int      selected_idx;
extern Window   prev_focus_win;

/* ------------------------------------------------------------------ */
/*  function declarations                                             */
/* ------------------------------------------------------------------ */

/* ipc.c */
void handle_sigusr1(int sig);
int  pidfile_write(void);
void pidfile_remove(void);
pid_t pidfile_read(void);
void do_toggle_and_exit(void);

/* clipboard.c */
void request_clipboard(void);
void read_clipboard(void);
void save_history_to_file(void);
void load_history_from_file(void);
void clipboard_claim(const char *text, int len, Time t);
void handle_selection_request(XEvent *ev);

/* popup.c */
int  create_window(void);
void popup_show(void);
void popup_hide(void);
void popup_toggle(void);
void render(void);
void filter_history(void);

#endif /* COPY_XLQD_COMMON_H */

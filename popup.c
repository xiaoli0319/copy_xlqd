#include "common.h"

/* ------------------------------------------------------------------ */
/*  create_window — X11 / Xft / XIM setup                             */
/* ------------------------------------------------------------------ */

int create_window(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy)
    {
        fprintf(stderr, "Cannot open display\n");
        return -1;
    }

    screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XSetWindowAttributes attr;
    attr.override_redirect = True;
    attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                      FocusChangeMask | StructureNotifyMask;
    attr.background_pixel = WhitePixel(dpy, screen);
    attr.border_pixel = BlackPixel(dpy, screen);

    win = XCreateWindow(dpy, root, -2000, -2000, 620, 280, 1,
                        DefaultDepth(dpy, screen), InputOutput,
                        DefaultVisual(dpy, screen),
                        CWOverrideRedirect | CWEventMask |
                            CWBackPixel | CWBorderPixel,
                        &attr);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));

    XMapWindow(dpy, win);

    /* atoms */
    clip_atom    = XInternAtom(dpy, "CLIPBOARD", False);
    utf8_string  = XInternAtom(dpy, "UTF8_STRING", False);
    prop_atom    = XInternAtom(dpy, "COPY_XLQD_PROP", False);
    targets_atom = XInternAtom(dpy, "TARGETS", False);

    /* XFixes */
    int fixes_error_base;
    if (!XFixesQueryExtension(dpy, &fixes_event_base, &fixes_error_base))
    {
        fprintf(stderr, "XFixes extension not available\n");
        return -1;
    }
    XFixesSelectSelectionInput(dpy, win, clip_atom,
                               XFixesSetSelectionOwnerNotifyMask);

    /* Xft font */
    if (!FcInit())
    {
        fprintf(stderr, "FcInit failed\n");
        return -1;
    }

    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, (FcChar8 *)"Noto Sans Mono CJK SC");
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);
    FcPatternAddString(pattern, FC_LANG, (FcChar8 *)"zh:en");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);

    if (match)
    {
        FcChar8 *family = NULL;
        FcPatternGetString(match, FC_FAMILY, 0, &family);
        fprintf(stderr, "debug: Using font: %s\n",
                family ? (char *)family : "unknown");
        xft_font = XftFontOpenPattern(dpy, match);
        FcPatternDestroy(match);
    }
    else
    {
        fprintf(stderr, "FcFontMatch failed\n");
        FcPatternDestroy(pattern);
        return -1;
    }
    FcPatternDestroy(pattern);

    if (!xft_font)
    {
        fprintf(stderr, "Cannot open Xft font\n");
        return -1;
    }

    line_h = xft_font->ascent + xft_font->descent + 4;

    xft_draw = XftDrawCreate(dpy, win,
                              DefaultVisual(dpy, screen),
                              DefaultColormap(dpy, screen));
    if (!xft_draw)
    {
        fprintf(stderr, "XftDrawCreate failed\n");
        return -1;
    }

    XRenderColor rc = {0, 0, 0, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                       DefaultColormap(dpy, screen), &rc, &xft_fg);

    XRenderColor rc_white = {0xffff, 0xffff, 0xffff, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                       DefaultColormap(dpy, screen), &rc_white, &xft_fg_white);

    XRenderColor rc_hl = {0xffff, 0x4444, 0x4444, 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
                       DefaultColormap(dpy, screen), &rc_hl, &xft_fg_hl);

    popup_x = (DisplayWidth(dpy, screen) - 620) / 2;
    popup_y = (DisplayHeight(dpy, screen) - 280) / 3;

    /* XIM for CJK input */
    xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim)
    {
        xic = XCreateIC(xim,
                        XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, win,
                        XNFocusWindow, win,
                        NULL);
        if (!xic)
            fprintf(stderr, "warn: XCreateIC failed\n");
    }
    else
    {
        fprintf(stderr, "debug: XOpenIM failed, fallback to XLookupString\n");
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  popup                                                              */
/* ------------------------------------------------------------------ */

void popup_show(void)
{
    if (popup_visible)
        return;

    scroll_offset = 0;
    selected_idx  = 0;
    search_len    = 0;
    search_text[0] = '\0';
    filter_history();

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
    if (grab != GrabSuccess)
    {
        usleep(20000);
        grab = XGrabKeyboard(dpy, win, True,
                             GrabModeAsync, GrabModeAsync, CurrentTime);
    }
    if (grab != GrabSuccess)
        fprintf(stderr, "warn: XGrabKeyboard failed (%d)\n", grab);

    fprintf(stderr, "info: popup shown\n");
    if (xic) XSetICFocus(xic);
}

void popup_hide(void)
{
    if (!popup_visible)
        return;
    if (xic) XUnsetICFocus(xic);
    XUngrabKeyboard(dpy, CurrentTime);
    XSync(dpy, False);
    if (prev_focus_win != None && prev_focus_win != win)
        XSetInputFocus(dpy, prev_focus_win, RevertToPointerRoot,
                       CurrentTime);
    XMoveWindow(dpy, win, -2000, -2000);
    XSync(dpy, False);
    popup_visible = 0;
    fprintf(stderr, "info: popup hidden\n");
}

void popup_toggle(void)
{
    if (popup_visible)
        popup_hide();
    else
        popup_show();
}

/* ------------------------------------------------------------------ */
/*  fuzzy search                                                       */
/* ------------------------------------------------------------------ */

/* Case-insensitive exact substring match */
static int matches_search(const char *text, int text_len)
{
    if (search_len == 0)
        return 1;
    if (text_len < search_len)
        return 0;
    for (int i = 0; i <= text_len - search_len; i++)
    {
        int match = 1;
        for (int j = 0; j < search_len; j++)
        {
            char tc = text[i + j];
            char sc = search_text[j];
            if (tc >= 'A' && tc <= 'Z') tc += 32;
            if (sc >= 'A' && sc <= 'Z') sc += 32;
            if (tc != sc) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

void filter_history(void)
{
    filtered_count = 0;
    int n = history_count < MAX_HISTORY ? history_count : MAX_HISTORY;
    int num = n;
    for (int i = 0; i < n; i++)
    {
        int idx = (history_count - 1 - i) % MAX_HISTORY;
        if (matches_search(history[idx].text, history[idx].len))
        {
            filtered_idx[filtered_count] = idx;
            filtered_num[filtered_count] = num;
            filtered_count++;
        }
        num--;
    }
    if (scroll_offset >= filtered_count && filtered_count > 0)
        scroll_offset = filtered_count - 1;
    if (scroll_offset < 0) scroll_offset = 0;
    if (selected_idx >= filtered_count)
        selected_idx = filtered_count > 0 ? filtered_count - 1 : 0;
    if (selected_idx < 0) selected_idx = 0;
}

/* ------------------------------------------------------------------ */
/*  render  (Xft — handles UTF-8 / CJK)                               */
/* ------------------------------------------------------------------ */

static void xft_draw_utf8(int x, int y, const char *s, int len)
{
    if (len <= 0) return;
    XftDrawStringUtf8(xft_draw, &xft_fg, xft_font,
                      x, y + xft_font->ascent,
                      (const FcChar8 *)s, len);
}

static void xft_draw_utf8_white(int x, int y, const char *s, int len)
{
    if (len <= 0) return;
    XftDrawStringUtf8(xft_draw, &xft_fg_white, xft_font,
                      x, y + xft_font->ascent,
                      (const FcChar8 *)s, len);
}

void render(void)
{
    char buf[640];
    int y = 10;

    XClearWindow(dpy, win);

    /* title */
    const char *title = "copy_xlqd  \xe2\x80\x94  \xe2\x86\x91\xe2\x86\x93 scroll  "
                        "Enter select  ESC dismiss";
    xft_draw_utf8(20, y, title, strlen(title));
    y += line_h;
    XDrawLine(dpy, win, gc, 20, y + xft_font->descent,
              600, y + xft_font->descent);
    y += 6;

    /* Search bar */
    {
        XSetForeground(dpy, gc, BlackPixel(dpy, screen));
        XDrawRectangle(dpy, win, gc, 10, y - 2, 580, line_h + 4);

        char sdisp[640];
        int sn = snprintf(sdisp, sizeof(sdisp), "Search: %s", search_text);
        xft_draw_utf8(20, y, sdisp, sn);

        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy, xft_font, (FcChar8 *)search_text,
                           search_len, &ext);
        int cx = 20 + 7 * 8 + ext.xOff;
        XDrawLine(dpy, win, gc, cx, y + 2, cx, y + line_h - 2);

        y += line_h + 8;
    }

    if (filtered_count == 0)
    {
        xft_draw_utf8(20, y,
                      search_len > 0 ? "(no match)" : "(empty)",
                      search_len > 0 ? 10 : 7);
        return;
    }

    /* Draw visible rows */
    for (int i = 0; i < DISPLAY_ROWS && scroll_offset + i < filtered_count; i++)
    {
        int fi = scroll_offset + i;
        int idx = filtered_idx[fi];
        ClipItem *item = &history[idx];

        int prefix_len = snprintf(buf, sizeof(buf), "[%d] ", filtered_num[fi]);
        int content_off = 0;
        int match_off = -1;

        if (search_len > 0 && item->len >= search_len)
        {
            int match_pos = -1;
            for (int p = 0; p <= item->len - search_len; p++)
            {
                int ok = 1;
                for (int q = 0; q < search_len; q++)
                {
                    char tc = item->text[p + q];
                    char sc = search_text[q];
                    if (tc >= 'A' && tc <= 'Z') tc += 32;
                    if (sc >= 'A' && sc <= 'Z') sc += 32;
                    if (tc != sc) { ok = 0; break; }
                }
                if (ok) { match_pos = p; break; }
            }

            if (match_pos >= 0)
            {
                int ctx = 30;
                int s = match_pos - ctx; if (s < 0) s = 0;
                int e = match_pos + search_len + ctx;
                if (e > item->len) e = item->len;

                int remain = (int)sizeof(buf) - prefix_len - 1;

                if (s > 0 && content_off + 2 < remain)
                    buf[prefix_len + content_off++] = '.',
                    buf[prefix_len + content_off++] = '.';

                int before_len = match_pos - s;
                if (before_len > 0 && content_off + before_len <= remain)
                {
                    memcpy(buf + prefix_len + content_off,
                           item->text + s, before_len);
                    content_off += before_len;
                }

                match_off = content_off;

                int mlen = search_len;
                if (content_off + mlen > remain)
                    mlen = remain - content_off;
                if (mlen > 0)
                {
                    memcpy(buf + prefix_len + content_off,
                           item->text + match_pos, mlen);
                    content_off += mlen;
                }

                int after_start = match_pos + search_len;
                int after_len = e - after_start;
                if (after_len > 0 && content_off + after_len <= remain)
                {
                    memcpy(buf + prefix_len + content_off,
                           item->text + after_start, after_len);
                    content_off += after_len;
                }

                if (e < item->len && content_off + 2 < remain)
                    buf[prefix_len + content_off++] = '.',
                    buf[prefix_len + content_off++] = '.';
            }
        }

        if (content_off == 0)
        {
            int copy_len = item->len;
            int remain = (int)sizeof(buf) - prefix_len - 1;
            if (copy_len > remain) copy_len = remain;
            memcpy(buf + prefix_len, item->text, copy_len);
            content_off = copy_len;
        }

        buf[prefix_len + content_off] = '\0';
        for (int k = prefix_len; k < prefix_len + content_off; k++)
            if ((unsigned char)buf[k] < 0x20)
                buf[k] = ' ';

        if (i == selected_idx)
        {
            XSetForeground(dpy, gc, BlackPixel(dpy, screen));
            XFillRectangle(dpy, win, gc, 10, y - 2, 600, line_h);
            XSetForeground(dpy, gc, BlackPixel(dpy, screen));
            xft_draw_utf8_white(20, y, buf + prefix_len, content_off);
        }
        else
        {
            xft_draw_utf8(20, y, buf + prefix_len, content_off);
        }

        if (match_off >= 0)
        {
            XGlyphInfo gi;
            XftTextExtentsUtf8(dpy, xft_font,
                               (FcChar8 *)(buf + prefix_len),
                               match_off, &gi);
            int mx = 20 + gi.xOff;
            int hl_len = search_len;
            if (match_off + hl_len > content_off)
                hl_len = content_off - match_off;
            if (hl_len > 0)
                XftDrawStringUtf8(xft_draw, &xft_fg_hl, xft_font,
                                  mx, y + xft_font->ascent,
                                  (FcChar8 *)(buf + prefix_len + match_off),
                                  hl_len);
        }

        y += line_h;
    }
}

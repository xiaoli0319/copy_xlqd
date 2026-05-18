#include "common.h"

/* ------------------------------------------------------------------ */
/*  clipboard read                                                     */
/* ------------------------------------------------------------------ */

void request_clipboard(void)
{
    if (pending_selection)
        return;
    Window owner = XGetSelectionOwner(dpy, clip_atom);
    if (owner == None || owner == win)
        return;
    XConvertSelection(dpy, clip_atom, utf8_string, prop_atom, win,
                      CurrentTime);
    pending_selection = 1;
}

void read_clipboard(void)
{
    pending_selection = 0;

    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int rc = XGetWindowProperty(dpy, win, prop_atom,
                                0, MAX_TEXT_LEN / 4, True,
                                AnyPropertyType,
                                &type, &format, &nitems, &bytes_after,
                                &data);
    if (rc != Success || !data || nitems == 0)
    {
        if (data) XFree(data);
        return;
    }

    /* skip duplicates */
    if (history_count > 0)
    {
        int last = (history_count - 1) % MAX_HISTORY;
        if ((int)nitems == history[last].len &&
            memcmp(data, history[last].text, nitems) == 0)
        {
            XFree(data);
            return;
        }
    }

    int idx = history_count % MAX_HISTORY;
    int len = (int)nitems < MAX_TEXT_LEN - 1 ? (int)nitems
                                             : MAX_TEXT_LEN - 1;
    memcpy(history[idx].text, data, len);
    history[idx].text[len] = '\0';
    history[idx].len = len;
    history_count++;

    fprintf(stderr, "clipboard[%d]: %.*s\n",
            history_count - 1,
            len < 80 ? len : 80, history[idx].text);

    save_history_to_file();
    XFree(data);
}

/* ------------------------------------------------------------------ */
/*  history file I/O                                                   */
/* ------------------------------------------------------------------ */

static void history_path(char *buf, size_t sz)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sz, "%s/.local/share/copy_xlqd/history.dat", home);
}

void save_history_to_file(void)
{
    char filepath[512];
    history_path(filepath, sizeof(filepath));

    /* ensure directory exists */
    char dir[256];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(dir, sizeof(dir), "%s/.local/share/copy_xlqd", home);
    mkdir(dir, 0755);

    FILE *f = fopen(filepath, "wb");
    if (!f)
    {
        perror("fopen history");
        return;
    }

    int n = history_count < MAX_HISTORY ? history_count : MAX_HISTORY;
    if (fwrite(&n, sizeof(int), 1, f) != 1)
    {
        perror("fwrite count");
        fclose(f);
        return;
    }

    for (int i = 0; i < n; i++)
    {
        int idx = (history_count - n + i) % MAX_HISTORY;
        ClipItem *item = &history[idx];
        if (fwrite(&item->len, sizeof(int), 1, f) != 1 ||
            fwrite(item->text, 1, item->len, f) != (size_t)item->len)
        {
            perror("fwrite item");
            fclose(f);
            return;
        }
    }
    fclose(f);
}

void load_history_from_file(void)
{
    char filepath[512];
    history_path(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    int n = 0;
    if (fread(&n, sizeof(int), 1, f) != 1)
    {
        fclose(f);
        return;
    }

    for (int i = 0; i < n && i < MAX_HISTORY; i++)
    {
        int len = 0;
        if (fread(&len, sizeof(int), 1, f) != 1)
            break;
        if (len > 0 && len < MAX_TEXT_LEN)
        {
            int idx = history_count % MAX_HISTORY;
            if (fread(history[idx].text, 1, len, f) == (size_t)len)
            {
                history[idx].text[len] = '\0';
                history[idx].len = len;
                history_count++;
            }
        }
    }
    fclose(f);
    fprintf(stderr, "info: loaded %d items from history file\n", n);
}

/* ------------------------------------------------------------------ */
/*  clipboard write-back (selection owner protocol)                    */
/* ------------------------------------------------------------------ */

void clipboard_claim(const char *text, int len, Time t)
{
    if (len >= MAX_TEXT_LEN)
        len = MAX_TEXT_LEN - 1;
    memcpy(owned_text, text, len);
    owned_text[len] = '\0';
    owned_len = len;

    XSetSelectionOwner(dpy, clip_atom, win, t);
    XFlush(dpy);

    if (XGetSelectionOwner(dpy, clip_atom) == win)
        fprintf(stderr, "info: CLIPBOARD owned, len=%d\n", owned_len);
    else
        fprintf(stderr, "warn: XSetSelectionOwner failed\n");
}

void handle_selection_request(XEvent *ev)
{
    XSelectionRequestEvent *req = &ev->xselectionrequest;

    fprintf(stderr,
            "debug: SelectionRequest target=%lu prop=%lu owned_len=%d\n",
            (unsigned long)req->target,
            (unsigned long)req->property,
            owned_len);

    Atom reply_prop = (req->property != None) ? req->property : req->target;

    XEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.xselection.type          = SelectionNotify;
    reply.xselection.display       = dpy;
    reply.xselection.requestor     = req->requestor;
    reply.xselection.selection     = req->selection;
    reply.xselection.target        = req->target;
    reply.xselection.time          = req->time;
    reply.xselection.property      = None;

    if (req->target == targets_atom)
    {
        Atom supported[] = {targets_atom, utf8_string, XA_STRING};
        XChangeProperty(dpy, req->requestor, reply_prop,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)supported,
                        (int)(sizeof(supported) / sizeof(Atom)));
        reply.xselection.property = reply_prop;
    }
    else if (req->target == utf8_string)
    {
        XChangeProperty(dpy, req->requestor, reply_prop,
                        utf8_string, 8, PropModeReplace,
                        (unsigned char *)owned_text, owned_len);
        reply.xselection.property = reply_prop;
    }
    else if (req->target == XA_STRING)
    {
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

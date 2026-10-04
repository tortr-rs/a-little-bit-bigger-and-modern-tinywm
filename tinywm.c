#define _POSIX_C_SOURCE 200809L

/* TinyWM is written by Nick Welch <nick@incise.org> in 2005 & 2011.
 *
 * This software is in the public domain
 * and is provided AS IS, with NO WARRANTY. */

#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_CLIENTS 512
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum { WM_DELETE, NET_SUPPORTED, NET_CLIENT_LIST, NET_ACTIVE_WINDOW,
       NET_CURRENT_DESKTOP, NET_NUMBER_OF_DESKTOPS, NET_WM_NAME,
       NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_WINDOW_TYPE,
       NET_WM_WINDOW_TYPE_DOCK, NET_WM_WINDOW_TYPE_DIALOG,
       NET_WM_WINDOW_TYPE_SPLASH, NET_WM_STRUT_PARTIAL, NET_WORKAREA,
       ATOM_COUNT };

typedef struct {
    Window win;
    int desktop;
    int ignore_unmap;
    int mapped;
    int floating;
    int fullscreen;
    int maximized;
    int x, y, w, h;
} Client;

static Display *dpy;
static Window root, focused;
static Atom atoms[ATOM_COUNT];
static Client clients[MAX_CLIENTS];
static size_t nclients;
static int desktop, tiled;
static int screen_w, screen_h;
static int work_x, work_y, work_w, work_h;
static XButtonEvent drag;
static XWindowAttributes drag_attr;
static unsigned long focus_color, unfocus_color;
static int startup_error;
static Pixmap wallpaper_pm;

static void update_clients(void);
static void arrange(void);
static void focus_window(Window win);

static int
xerror(Display *display, XErrorEvent *event)
{
    (void)display;
    if (event->error_code == BadAccess)
        startup_error = 1;
    return 0;
}

static void
reap_children(int signal_number)
{
    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static Client *
client_for(Window win)
{
    size_t i;
    for (i = 0; i < nclients; i++)
        if (clients[i].win == win)
            return &clients[i];
    return NULL;
}

static int
get_property(Window win, Atom property, Atom type, unsigned long *values,
        unsigned long capacity, unsigned long *nvalues)
{
    Atom actual;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    int result = 0;

    if (XGetWindowProperty(dpy, win, property, 0, (long)capacity, False, type,
            &actual, &format, &count, &after, &data) == Success && data != NULL &&
            actual == type && format == 32 && count > 0 && count <= capacity) {
        memcpy(values, data, count * sizeof(*values));
        *nvalues = count;
        result = 1;
    }
    if (data != NULL)
        XFree(data);
    return result;
}

static int
has_type(Window win, Atom type)
{
    Atom actual;
    int format;
    unsigned long count, after, i;
    unsigned char *data = NULL;
    int found = 0;

    if (XGetWindowProperty(dpy, win, atoms[NET_WM_WINDOW_TYPE], 0, 32, False,
            XA_ATOM, &actual, &format, &count, &after, &data) == Success &&
            data != NULL && actual == XA_ATOM && format == 32) {
        Atom *types = (Atom *)data;
        for (i = 0; i < count; i++)
            if (types[i] == type)
                found = 1;
    }
    if (data != NULL)
        XFree(data);
    return found;
}

static void
publish_workarea(void)
{
    unsigned long area[9 * 4];
    size_t i;
    int left = 0, right = 0, top = 0, bottom = 0;
    work_x = 0;
    work_y = 0;
    work_w = screen_w;
    work_h = screen_h;
    for (i = 0; i < nclients; i++) {
        unsigned long strut[12] = {0}, nstrut = 0;
        Client *c = &clients[i];
        if (!has_type(c->win, atoms[NET_WM_WINDOW_TYPE_DOCK]))
            continue;
        if (!get_property(c->win, atoms[NET_WM_STRUT_PARTIAL], XA_CARDINAL,
                strut, LENGTH(strut), &nstrut) || nstrut < 4)
            continue;
        left = MAX(left, (int)MIN(strut[0], (unsigned long)screen_w));
        right = MAX(right, (int)MIN(strut[1], (unsigned long)screen_w));
        top = MAX(top, (int)MIN(strut[2], (unsigned long)screen_h));
        bottom = MAX(bottom, (int)MIN(strut[3], (unsigned long)screen_h));
    }
    work_x = MIN(left, screen_w - 1);
    work_y = MIN(top, screen_h - 1);
    work_w = MAX(1, screen_w - work_x - MIN(right, screen_w - work_x - 1));
    work_h = MAX(1, screen_h - work_y - MIN(bottom, screen_h - work_y - 1));
    for (i = 0; i < LENGTH(area) / 4; i++) {
        area[i * 4] = (unsigned long)work_x;
        area[i * 4 + 1] = (unsigned long)work_y;
        area[i * 4 + 2] = (unsigned long)work_w;
        area[i * 4 + 3] = (unsigned long)work_h;
    }
    XChangeProperty(dpy, root, atoms[NET_WORKAREA],
            XA_CARDINAL, 32, PropModeReplace, (unsigned char *)area,
            (int)LENGTH(area));
}

static void
update_clients(void)
{
    unsigned long list[MAX_CLIENTS];
    size_t i;
    for (i = 0; i < nclients; i++)
        list[i] = (unsigned long)clients[i].win;
    XChangeProperty(dpy, root, atoms[NET_CLIENT_LIST], XA_WINDOW, 32,
            PropModeReplace, (unsigned char *)list, (int)nclients);
}

static void
set_active(Window win)
{
    unsigned long value = (unsigned long)win;
    if (win == None)
        XDeleteProperty(dpy, root, atoms[NET_ACTIVE_WINDOW]);
    else
        XChangeProperty(dpy, root, atoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32,
                PropModeReplace, (unsigned char *)&value, 1);
}

static void
focus_window(Window win)
{
    Client *c = client_for(win);
    if (c == NULL || c->desktop != desktop || has_type(win,
            atoms[NET_WM_WINDOW_TYPE_DOCK]))
        return;
    if (focused != None)
        XSetWindowBorder(dpy, focused, unfocus_color);
    focused = win;
    XSetWindowBorder(dpy, win, focus_color);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, win);
    set_active(win);
}

static void
manage(Window win)
{
    XWindowAttributes attr;
    XSetWindowAttributes changes;
    Client *c;
    size_t i;
    if (client_for(win) != NULL || nclients == MAX_CLIENTS ||
            !XGetWindowAttributes(dpy, win, &attr) || attr.override_redirect)
        return;
    c = &clients[nclients++];
    memset(c, 0, sizeof(*c));
    c->win = win;
    c->desktop = desktop;
    c->x = attr.x;
    c->y = attr.y;
    c->w = attr.width;
    c->h = attr.height;
    c->floating = has_type(win, atoms[NET_WM_WINDOW_TYPE_DOCK]) ||
            has_type(win, atoms[NET_WM_WINDOW_TYPE_DIALOG]) ||
            has_type(win, atoms[NET_WM_WINDOW_TYPE_SPLASH]);
    c->mapped = attr.map_state != IsUnmapped;
    {
        unsigned long states[8], nstates = 0;
        if (get_property(win, atoms[NET_WM_STATE], XA_ATOM, states,
                LENGTH(states), &nstates))
            for (i = 0; i < nstates; i++)
                if (states[i] == (unsigned long)atoms[NET_WM_STATE_FULLSCREEN])
                    c->fullscreen = 1;
    }
    changes.event_mask = EnterWindowMask | FocusChangeMask | StructureNotifyMask |
            PropertyChangeMask | ButtonPressMask;
    XChangeWindowAttributes(dpy, win, CWEventMask, &changes);
    if (!has_type(win, atoms[NET_WM_WINDOW_TYPE_DOCK])) {
        XSetWindowBorderWidth(dpy, win, BORDER_WIDTH);
        XSetWindowBorder(dpy, win, unfocus_color);
    }
    if (c->desktop == desktop) {
        c->mapped = 1;
        XMapWindow(dpy, win);
    }
    update_clients();
    publish_workarea();
    if (!c->floating && tiled)
        arrange();
    if (FOCUS_FOLLOWS_MOUSE)
        focus_window(win);
    if (c->fullscreen)
        XMoveResizeWindow(dpy, win, 0, 0, (unsigned)screen_w,
                (unsigned)screen_h);
}

static void
unmanage(Window win)
{
    size_t i;
    for (i = 0; i < nclients; i++) {
        if (clients[i].win != win)
            continue;
        if (focused == win) {
            focused = None;
            set_active(None);
        }
        clients[i] = clients[--nclients];
        update_clients();
        publish_workarea();
        arrange();
        for (i = 0; i < nclients; i++)
            if (clients[i].desktop == desktop &&
                    !has_type(clients[i].win,
                        atoms[NET_WM_WINDOW_TYPE_DOCK])) {
                focus_window(clients[i].win);
                break;
            }
        return;
    }
}

static void
switch_desktop(int next)
{
    size_t i;
    desktop = next;
    for (i = 0; i < nclients; i++) {
        Client *c = &clients[i];
        int dock = has_type(c->win, atoms[NET_WM_WINDOW_TYPE_DOCK]);
        if ((c->desktop == desktop || dock) && !c->mapped) {
            c->mapped = 1;
            XMapWindow(dpy, c->win);
        } else if (c->desktop != desktop && !dock && c->mapped) {
            c->ignore_unmap++;
            c->mapped = 0;
            XUnmapWindow(dpy, c->win);
        }
    }
    {
        unsigned long value = (unsigned long)desktop;
        XChangeProperty(dpy, root, atoms[NET_CURRENT_DESKTOP], XA_CARDINAL,
                32, PropModeReplace, (unsigned char *)&value, 1);
    }
    focused = None;
    set_active(None);
    arrange();
}

static void
arrange(void)
{
    size_t i, count = 0, index = 0;
    int master_w;
    if (!tiled)
        return;
    for (i = 0; i < nclients; i++)
        if (clients[i].desktop == desktop && !clients[i].floating &&
                !clients[i].fullscreen)
            count++;
    if (count == 0)
        return;
    master_w = count > 1 ? work_w / 2 : work_w;
    for (i = 0; i < nclients; i++) {
        Client *c = &clients[i];
        int x, y, w, h;
        if (c->desktop != desktop || c->floating || c->fullscreen)
            continue;
        if (index == 0) {
            x = work_x;
            y = work_y;
            w = master_w;
            h = work_h;
        } else {
            x = work_x + master_w;
            y = work_y + (int)((index - 1) * (size_t)work_h /
                    (count - 1));
            w = work_w - master_w;
            h = work_h / (int)(count - 1);
            if (index == count - 1)
                h = work_y + work_h - y;
        }
        XMoveResizeWindow(dpy, c->win, x, y, (unsigned)MAX(1, w -
                2 * BORDER_WIDTH), (unsigned)MAX(1, h - 2 * BORDER_WIDTH));
        index++;
    }
}

static void
launch(const char *program, char *const fallback[])
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", program, (char *)NULL);
        if (fallback != NULL)
            execvp(fallback[0], fallback);
        _exit(127);
    }
}

static void
launch_script(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(path, path, (char *)NULL);
        _exit(127);
    }
}

static void
close_window(Window win)
{
    Atom *protocols;
    int count, i;
    if (XGetWMProtocols(dpy, win, &protocols, &count)) {
        for (i = 0; i < count; i++) {
            if (protocols[i] == atoms[WM_DELETE]) {
                XEvent event;
                memset(&event, 0, sizeof(event));
                event.xclient.type = ClientMessage;
                event.xclient.window = win;
                event.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
                event.xclient.format = 32;
                event.xclient.data.l[0] = (long)atoms[WM_DELETE];
                event.xclient.data.l[1] = CurrentTime;
                XSendEvent(dpy, win, False, NoEventMask, &event);
                XFree(protocols);
                return;
            }
        }
        XFree(protocols);
    }
    XKillClient(dpy, win);
}

static void
set_fullscreen(Client *c, int enabled)
{
    Atom actual;
    int format;
    unsigned long count, after, i;
    unsigned char *data = NULL;
    Atom states[32];
    int nstates = 0;
    XWindowAttributes attr;

    if (c->fullscreen == enabled)
        return;
    if (enabled && XGetWindowAttributes(dpy, c->win, &attr)) {
        c->x = attr.x;
        c->y = attr.y;
        c->w = attr.width;
        c->h = attr.height;
    }
    c->fullscreen = enabled;
    if (XGetWindowProperty(dpy, c->win, atoms[NET_WM_STATE], 0, 32, False,
            XA_ATOM, &actual, &format, &count, &after, &data) == Success &&
            data != NULL && actual == XA_ATOM && format == 32) {
        Atom *old = (Atom *)data;
        for (i = 0; i < count &&
                nstates < (int)LENGTH(states) - (enabled ? 1 : 0); i++)
            if (old[i] != atoms[NET_WM_STATE_FULLSCREEN])
                states[nstates++] = old[i];
    }
    if (data != NULL)
        XFree(data);
    if (enabled)
        states[nstates++] = atoms[NET_WM_STATE_FULLSCREEN];
    if (nstates > 0)
        XChangeProperty(dpy, c->win, atoms[NET_WM_STATE], XA_ATOM, 32,
                PropModeReplace, (unsigned char *)states, nstates);
    else
        XDeleteProperty(dpy, c->win, atoms[NET_WM_STATE]);
    if (enabled) {
        XMoveResizeWindow(dpy, c->win, 0, 0, (unsigned)screen_w,
                (unsigned)screen_h);
    } else {
        XMoveResizeWindow(dpy, c->win, c->x, c->y, (unsigned)c->w,
                (unsigned)c->h);
        arrange();
    }
}

static void
snap(Client *c, KeySym key)
{
    int x = work_x, y = work_y, w = work_w, h = work_h;
    if (key == XK_Left)
        w /= 2;
    else if (key == XK_Right) {
        x += w / 2;
        w -= w / 2;
    } else if (key == XK_Up)
        h /= 2;
    else if (key == XK_Down) {
        y += h / 2;
        h -= h / 2;
    }
    c->floating = 1;
    XMoveResizeWindow(dpy, c->win, x, y,
            (unsigned)MAX(1, w - 2 * BORDER_WIDTH),
            (unsigned)MAX(1, h - 2 * BORDER_WIDTH));
}

static int
ppm_token(FILE *f, unsigned long *out)
{
    int ch;
    do {
        ch = fgetc(f);
        if (ch == '#')
            while (ch != '\n' && ch != EOF)
                ch = fgetc(f);
    } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
    if (ch < '0' || ch > '9')
        return 0;
    *out = 0;
    while (ch >= '0' && ch <= '9') {
        *out = *out * 10 + (unsigned long)(ch - '0');
        if (*out > 65535)
            return 0;
        ch = fgetc(f);
    }
    return 1;
}

static unsigned long
pack_channel(unsigned long mask, unsigned long value)
{
    int shift = 0, bits = 0;
    unsigned long m;
    if (mask == 0)
        return 0;
    while (!((mask >> shift) & 1))
        shift++;
    for (m = mask >> shift; m; m >>= 1)
        bits++;
    if (bits > 8)
        return (value << (bits - 8)) << shift;
    return (value >> (8 - bits)) << shift;
}

/* Returns a screen-sized pixmap or None. Scales to cover, nearest-neighbor. */
static Pixmap
load_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned long iw, ih, maxval;
    unsigned char *src = NULL;
    char *dst = NULL;
    XImage *img = NULL;
    Pixmap pm = None;
    int scr = DefaultScreen(dpy), depth = DefaultDepth(dpy, scr);
    Visual *vis = DefaultVisual(dpy, scr);
    int x, y;
    double scale;
    int ox, oy;

    if (f == NULL)
        return None;
    if (fgetc(f) != 'P' || fgetc(f) != '6' || !ppm_token(f, &iw) ||
            !ppm_token(f, &ih) || !ppm_token(f, &maxval) || iw == 0 ||
            ih == 0 || maxval != 255 || vis->class != TrueColor ||
            iw > 16384 || ih > 16384)
        goto out;
    src = malloc(iw * ih * 3);
    if (src == NULL || fread(src, 3, iw * ih, f) != iw * ih)
        goto out;
    dst = malloc((size_t)screen_w * (size_t)screen_h * 4);
    if (dst == NULL)
        goto out;
    img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, dst,
            (unsigned)screen_w, (unsigned)screen_h, 32, 0);
    if (img == NULL)
        goto out;
    scale = MAX((double)screen_w / (double)iw, (double)screen_h / (double)ih);
    ox = (int)(((double)iw * scale - screen_w) / 2);
    oy = (int)(((double)ih * scale - screen_h) / 2);
    for (y = 0; y < screen_h; y++) {
        unsigned long sy = MIN(ih - 1, (unsigned long)((y + oy) / scale));
        for (x = 0; x < screen_w; x++) {
            unsigned long sx = MIN(iw - 1, (unsigned long)((x + ox) / scale));
            unsigned char *p = src + (sy * iw + sx) * 3;
            XPutPixel(img, x, y, pack_channel(vis->red_mask, p[0]) |
                    pack_channel(vis->green_mask, p[1]) |
                    pack_channel(vis->blue_mask, p[2]));
        }
    }
    pm = XCreatePixmap(dpy, root, (unsigned)screen_w, (unsigned)screen_h,
            (unsigned)depth);
    XPutImage(dpy, pm, DefaultGC(dpy, scr), img, 0, 0, 0, 0,
            (unsigned)screen_w, (unsigned)screen_h);
out:
    if (img != NULL)
        XDestroyImage(img); /* also frees dst */
    else
        free(dst);
    free(src);
    fclose(f);
    return pm;
}

static void
set_wallpaper(void)
{
    char path[1024];
    const char *home = getenv("HOME");
    Pixmap pm = None;
    Atom prop_root = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    Atom prop_eset = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);

    if (home != NULL && snprintf(path, sizeof(path), "%s/%s", home,
            WALLPAPER_PATH) < (int)sizeof(path))
        pm = load_ppm(path);
    if (pm != None) {
        unsigned long id = (unsigned long)pm;
        XSetWindowBackgroundPixmap(dpy, root, pm);
        XChangeProperty(dpy, root, prop_root, XA_PIXMAP, 32, PropModeReplace,
                (unsigned char *)&id, 1);
        XChangeProperty(dpy, root, prop_eset, XA_PIXMAP, 32, PropModeReplace,
                (unsigned char *)&id, 1);
    } else {
        XColor c, exact;
        XDeleteProperty(dpy, root, prop_root);
        XDeleteProperty(dpy, root, prop_eset);
        if (XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
                WALLPAPER_COLOR, &c, &exact))
            XSetWindowBackground(dpy, root, c.pixel);
    }
    XClearWindow(dpy, root);
    if (wallpaper_pm != None)
        XFreePixmap(dpy, wallpaper_pm);
    wallpaper_pm = pm;
}

static void
key_press(XKeyEvent *event)
{
    KeySym key = XLookupKeysym(event, 0);
    unsigned int state = event->state & (Mod1Mask | ShiftMask);
    Client *c = client_for(focused);
    int i;

    if (state == Mod1Mask && key >= XK_1 && key <= XK_9) {
        switch_desktop((int)(key - XK_1));
        return;
    }
    if (state == (Mod1Mask | ShiftMask) && key >= XK_1 && key <= XK_9) {
        if (c != NULL) {
            int target = (int)(key - XK_1);
            if (c->desktop != target) {
                c->desktop = target;
                if (focused == c->win) {
                    focused = None;
                    set_active(None);
                }
            }
            if (c->desktop != desktop && c->mapped) {
                c->ignore_unmap++;
                c->mapped = 0;
                XUnmapWindow(dpy, c->win);
            }
            update_clients();
            arrange();
        }
        return;
    }
    if (state == (Mod1Mask | ShiftMask) && key == XK_w) {
        set_wallpaper();
        return;
    }
    if (state == (Mod1Mask | ShiftMask) && key == XK_q) {
        XCloseDisplay(dpy);
        exit(0);
    }
    if (state == Mod1Mask && key == XK_F1 && focused != None)
        XRaiseWindow(dpy, focused);
    else if (state == Mod1Mask && key == XK_q && focused != None)
        close_window(focused);
    else if (state == Mod1Mask && key == XK_Return)
        launch(TERMINAL, NULL);
    else if (state == Mod1Mask && key == XK_p) {
        launch("if command -v dmenu_run >/dev/null 2>&1; then "
                "exec dmenu_run; else exec rofi -show drun; fi", NULL);
    } else if (state == Mod1Mask && key == XK_Tab && nclients > 0) {
        int start = -1;
        for (i = 0; i < (int)nclients; i++)
            if (clients[i].win == focused)
                start = i;
        for (i = 1; i <= (int)nclients; i++) {
            int index = (start + i) % (int)nclients;
            Client *next = &clients[index];
            if (next->desktop == desktop &&
                    !has_type(next->win, atoms[NET_WM_WINDOW_TYPE_DOCK])) {
                focus_window(next->win);
                break;
            }
        }
    } else if (state == Mod1Mask && key == XK_t) {
        tiled = !tiled;
        arrange();
    } else if (state == Mod1Mask && key == XK_f && c != NULL) {
        if (!c->maximized) {
            XWindowAttributes attr;
            if (XGetWindowAttributes(dpy, c->win, &attr)) {
                c->x = attr.x;
                c->y = attr.y;
                c->w = attr.width;
                c->h = attr.height;
            }
            c->maximized = 1;
            XMoveResizeWindow(dpy, c->win, 0, 0, (unsigned)screen_w,
                    (unsigned)screen_h);
        } else {
            c->maximized = 0;
            XMoveResizeWindow(dpy, c->win, c->x, c->y, (unsigned)c->w,
                    (unsigned)c->h);
            arrange();
        }
    } else if (state == Mod1Mask && c != NULL &&
            (key == XK_Left || key == XK_Right || key == XK_Up ||
             key == XK_Down))
        snap(c, key);
}

int
main(void)
{
    XEvent ev;
    XColor color, exact;
    char *names[ATOM_COUNT] = {"WM_DELETE_WINDOW", "_NET_SUPPORTED",
        "_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW", "_NET_CURRENT_DESKTOP",
        "_NET_NUMBER_OF_DESKTOPS", "_NET_WM_NAME", "_NET_WM_STATE",
        "_NET_WM_STATE_FULLSCREEN", "_NET_WM_WINDOW_TYPE",
        "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DIALOG",
        "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_STRUT_PARTIAL",
        "_NET_WORKAREA"};
    unsigned long supported[14], value;
    int i;
    struct sigaction sa;

    if ((dpy = XOpenDisplay(NULL)) == NULL)
        return 1;
    XSetErrorHandler(xerror);
    root = DefaultRootWindow(dpy);
    screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
    work_w = screen_w;
    work_h = screen_h;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
            FOCUS_COLOR, &color, &exact))
        focus_color = color.pixel;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)),
            UNFOCUS_COLOR, &color, &exact))
        unfocus_color = color.pixel;
    for (i = 0; i < ATOM_COUNT; i++)
        atoms[i] = XInternAtom(dpy, names[i], False);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
            ButtonPressMask | EnterWindowMask | PropertyChangeMask);
    XSync(dpy, False);
    if (startup_error) {
        XCloseDisplay(dpy);
        return 1;
    }

    supported[0] = atoms[NET_SUPPORTED];
    supported[1] = atoms[NET_CLIENT_LIST];
    supported[2] = atoms[NET_ACTIVE_WINDOW];
    supported[3] = atoms[NET_CURRENT_DESKTOP];
    supported[4] = atoms[NET_NUMBER_OF_DESKTOPS];
    supported[5] = atoms[NET_WM_NAME];
    supported[6] = atoms[NET_WM_STATE];
    supported[7] = atoms[NET_WM_STATE_FULLSCREEN];
    supported[8] = atoms[NET_WM_WINDOW_TYPE];
    supported[9] = atoms[NET_WM_WINDOW_TYPE_DOCK];
    supported[10] = atoms[NET_WM_WINDOW_TYPE_DIALOG];
    supported[11] = atoms[NET_WM_WINDOW_TYPE_SPLASH];
    supported[12] = atoms[NET_WM_STRUT_PARTIAL];
    supported[13] = atoms[NET_WORKAREA];
    XChangeProperty(dpy, root, atoms[NET_SUPPORTED], XA_ATOM, 32,
            PropModeReplace, (unsigned char *)supported, 14);
    value = 9;
    XChangeProperty(dpy, root, atoms[NET_NUMBER_OF_DESKTOPS], XA_CARDINAL,
            32, PropModeReplace, (unsigned char *)&value, 1);
    value = 0;
    XChangeProperty(dpy, root, atoms[NET_CURRENT_DESKTOP], XA_CARDINAL,
            32, PropModeReplace, (unsigned char *)&value, 1);
    XChangeProperty(dpy, root, atoms[NET_WM_NAME], XInternAtom(dpy,
            "UTF8_STRING", False), 8, PropModeReplace,
            (unsigned char *)"TinyWM", 6);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_F1), Mod1Mask, root, True,
            GrabModeAsync, GrabModeAsync);
    for (i = 0; i < 9; i++) {
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_1 + i), Mod1Mask, root, True,
                GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_1 + i), Mod1Mask | ShiftMask,
                root, True, GrabModeAsync, GrabModeAsync);
    }
    {
        KeySym keys[] = {XK_q, XK_Return, XK_p, XK_Tab, XK_t, XK_f,
            XK_Left, XK_Right, XK_Up, XK_Down};
        for (i = 0; i < (int)LENGTH(keys); i++)
            XGrabKey(dpy, XKeysymToKeycode(dpy, keys[i]), Mod1Mask, root,
                    True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_q), Mod1Mask | ShiftMask,
                root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_w), Mod1Mask | ShiftMask,
                root, True, GrabModeAsync, GrabModeAsync);
    }
    XGrabButton(dpy, Button1, Mod1Mask, root, True, ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
            GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, Mod1Mask, root, True, ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
            GrabModeAsync, None, None);
    {
        Window returned_root, parent, *children = NULL;
        unsigned int nchildren;
        if (XQueryTree(dpy, root, &returned_root, &parent, &children,
                &nchildren)) {
            for (i = 0; i < (int)nchildren; i++)
                manage(children[i]);
            if (children != NULL)
                XFree(children);
        }
    }
    update_clients();
    publish_workarea();
    set_wallpaper();
    {
        char path[1024];
        const char *home = getenv("HOME");
        int path_length;
        if (home != NULL) {
            path_length = snprintf(path, sizeof(path),
                    "%s/.config/tinywm/autostart.sh", home);
            if (path_length >= 0 && path_length < (int)sizeof(path) &&
                    access(path, X_OK) == 0)
                launch_script(path);
        }
    }
    XSync(dpy, False);
    for (;;) {
        XNextEvent(dpy, &ev);
        if (ev.type == MapRequest) {
            Client *c = client_for(ev.xmaprequest.window);
            if (c == NULL)
                manage(ev.xmaprequest.window);
            else if (c->desktop == desktop && !c->mapped) {
                c->mapped = 1;
                XMapWindow(dpy, c->win);
            }
        }
        else if (ev.type == DestroyNotify && ev.xdestroywindow.event == root)
            unmanage(ev.xdestroywindow.window);
        else if (ev.type == MapNotify) {
            Client *c = client_for(ev.xmap.window);
            if (c != NULL)
                c->mapped = 1;
        } else if (ev.type == UnmapNotify && ev.xunmap.event == root) {
            Client *c = client_for(ev.xunmap.window);
            if (c != NULL) {
                c->mapped = 0;
                if (c->ignore_unmap > 0)
                    c->ignore_unmap--;
                else
                    unmanage(ev.xunmap.window);
            }
        } else if (ev.type == ConfigureRequest) {
            XConfigureRequestEvent *r = &ev.xconfigurerequest;
            Client *c = client_for(r->window);
            if (c == NULL || c->floating) {
                XWindowChanges changes = {r->x, r->y, r->width, r->height,
                    r->border_width, r->above, r->detail};
                XConfigureWindow(dpy, r->window, (unsigned)r->value_mask,
                        &changes);
            }
        } else if (ev.type == ButtonPress && ev.xbutton.subwindow != None) {
            XGetWindowAttributes(dpy, ev.xbutton.subwindow, &drag_attr);
            drag = ev.xbutton;
            focus_window(ev.xbutton.subwindow);
        } else if (ev.type == ButtonPress && ev.xbutton.window != root) {
            focus_window(ev.xbutton.window);
        } else if (ev.type == MotionNotify && drag.subwindow != None) {
            Client *c = client_for(drag.subwindow);
            int dx = ev.xmotion.x_root - drag.x_root;
            int dy = ev.xmotion.y_root - drag.y_root;
            if (c != NULL) {
                c->floating = 1;
                XMoveResizeWindow(dpy, drag.subwindow,
                        drag_attr.x + (drag.button == Button1 ? dx : 0),
                        drag_attr.y + (drag.button == Button1 ? dy : 0),
                        (unsigned)MAX(1, drag_attr.width +
                            (drag.button == Button3 ? dx : 0)),
                        (unsigned)MAX(1, drag_attr.height +
                            (drag.button == Button3 ? dy : 0)));
            }
        } else if (ev.type == ButtonRelease) {
            if (drag.subwindow != None) {
                Client *c = client_for(drag.subwindow);
                XWindowAttributes attr;
                if (c != NULL && XGetWindowAttributes(dpy, c->win, &attr)) {
                    c->x = attr.x;
                    c->y = attr.y;
                    c->w = attr.width;
                    c->h = attr.height;
                }
            }
            drag.subwindow = None;
        } else if (ev.type == KeyPress)
            key_press(&ev.xkey);
        else if (ev.type == EnterNotify && FOCUS_FOLLOWS_MOUSE &&
                ev.xcrossing.mode == NotifyNormal)
            focus_window(ev.xcrossing.window);
        else if (ev.type == ClientMessage &&
                ev.xclient.message_type == atoms[NET_CURRENT_DESKTOP] &&
                ev.xclient.data.l[0] >= 0 && ev.xclient.data.l[0] < 9)
            switch_desktop((int)ev.xclient.data.l[0]);
        else if (ev.type == ClientMessage &&
                ev.xclient.message_type == atoms[NET_ACTIVE_WINDOW]) {
            Client *c = client_for(ev.xclient.window);
            if (c != NULL) {
                if (c->desktop != desktop)
                    switch_desktop(c->desktop);
                focus_window(c->win);
            }
        } else if (ev.type == ClientMessage &&
                ev.xclient.message_type == atoms[NET_WM_STATE] &&
                ((Atom)ev.xclient.data.l[1] ==
                 atoms[NET_WM_STATE_FULLSCREEN] ||
                 (Atom)ev.xclient.data.l[2] ==
                 atoms[NET_WM_STATE_FULLSCREEN])) {
            Client *c = client_for(ev.xclient.window);
            int action = (int)ev.xclient.data.l[0];
            if (c != NULL && action >= 0 && action <= 2)
                set_fullscreen(c, action == 1 ||
                        (action == 2 && !c->fullscreen));
        } else if (ev.type == PropertyNotify &&
                ev.xproperty.atom == atoms[NET_WM_STRUT_PARTIAL])
            publish_workarea();
    }
}

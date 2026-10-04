/* TinyWM is written by Nick Welch <nick@incise.org> in 2005 & 2011.
 *
 * This software is in the public domain
 * and is provided AS IS, with NO WARRANTY. */

#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
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

enum { WM_DELETE, NET_SUPPORTED, NET_CLIENT_LIST, NET_ACTIVE_WINDOW,
       NET_CURRENT_DESKTOP, NET_NUMBER_OF_DESKTOPS, NET_WM_NAME,
       NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_WINDOW_TYPE,
       NET_WM_WINDOW_TYPE_DOCK, NET_WM_WINDOW_TYPE_DIALOG,
       NET_WM_WINDOW_TYPE_SPLASH, NET_WM_STRUT_PARTIAL, ATOM_COUNT };

typedef struct {
    Window win;
    int desktop;
    int ignore_unmap;
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

static void update_clients(void);
static void arrange(void);
static void focus_window(Window win);

static int
xerror(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
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
get_property(Window win, Atom property, Atom type, unsigned long *value)
{
    Atom actual;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    int result = 0;

    if (XGetWindowProperty(dpy, win, property, 0, 32, False, type, &actual,
            &format, &count, &after, &data) == Success && data != NULL &&
            format == 32 && count > 0) {
        *value = *(unsigned long *)data;
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
            data != NULL && format == 32) {
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
    unsigned long area[4];
    size_t i;
    work_x = 0;
    work_y = 0;
    work_w = screen_w;
    work_h = screen_h;
    for (i = 0; i < nclients; i++) {
        unsigned long strut[12] = {0};
        Client *c = &clients[i];
        if (!has_type(c->win, atoms[NET_WM_WINDOW_TYPE_DOCK]))
            continue;
        if (!get_property(c->win, atoms[NET_WM_STRUT_PARTIAL], XA_CARDINAL,
                strut))
            continue;
        if (strut[0] > 0 && strut[0] < (unsigned long)screen_w) {
            work_x = (int)strut[0];
            work_w = screen_w - work_x - (int)strut[1];
        }
        if (strut[2] > 0 && strut[2] < (unsigned long)screen_h) {
            work_y = (int)strut[2];
            work_h = screen_h - work_y - (int)strut[3];
        }
    }
    area[0] = (unsigned long)work_x;
    area[1] = (unsigned long)work_y;
    area[2] = (unsigned long)work_w;
    area[3] = (unsigned long)work_h;
    XChangeProperty(dpy, root, XInternAtom(dpy, "_NET_WORKAREA", False),
            XA_CARDINAL, 32, PropModeReplace, (unsigned char *)area, 4);
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
    XChangeProperty(dpy, root, atoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32,
            PropModeReplace, win == None ? NULL : (unsigned char *)&value,
            win == None ? 0 : 1);
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

static int
managed_type(Window win)
{
    return !has_type(win, atoms[NET_WM_WINDOW_TYPE_DOCK]);
}

static void
manage(Window win)
{
    XWindowAttributes attr;
    XSetWindowAttributes changes;
    Client *c;
    unsigned long state = 0;
    if (client_for(win) != NULL || nclients == MAX_CLIENTS ||
            !XGetWindowAttributes(dpy, win, &attr) || attr.override_redirect ||
            !managed_type(win))
        return;
    c = &clients[nclients++];
    memset(c, 0, sizeof(*c));
    c->win = win;
    c->desktop = desktop;
    c->x = attr.x;
    c->y = attr.y;
    c->w = attr.width;
    c->h = attr.height;
    c->floating = has_type(win, atoms[NET_WM_WINDOW_TYPE_DIALOG]) ||
            has_type(win, atoms[NET_WM_WINDOW_TYPE_SPLASH]);
    get_property(win, atoms[NET_WM_STATE], XA_ATOM, &state);
    c->fullscreen = state == (unsigned long)atoms[NET_WM_STATE_FULLSCREEN];
    changes.event_mask = EnterWindowMask | FocusChangeMask | StructureNotifyMask |
            PropertyChangeMask;
    XChangeWindowAttributes(dpy, win, CWEventMask, &changes);
    XSetWindowBorderWidth(dpy, win, BORDER_WIDTH);
    XSetWindowBorder(dpy, win, unfocus_color);
    XMapWindow(dpy, win);
    update_clients();
    publish_workarea();
    if (!c->floating && tiled)
        arrange();
    if (FOCUS_FOLLOWS_MOUSE)
        focus_window(win);
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
        if (c->desktop == desktop)
            XMapWindow(dpy, c->win);
        else {
            c->ignore_unmap++;
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
    unsigned long state = (unsigned long)atoms[NET_WM_STATE_FULLSCREEN];
    c->fullscreen = enabled;
    if (enabled) {
        XChangeProperty(dpy, c->win, atoms[NET_WM_STATE], XA_ATOM, 32,
                PropModeReplace, (unsigned char *)&state, 1);
        XMoveResizeWindow(dpy, c->win, 0, 0, (unsigned)screen_w,
                (unsigned)screen_h);
    } else {
        XDeleteProperty(dpy, c->win, atoms[NET_WM_STATE]);
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
            c->desktop = (int)(key - XK_1);
            if (c->desktop != desktop) {
                c->ignore_unmap++;
                XUnmapWindow(dpy, c->win);
            }
            update_clients();
            arrange();
        }
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
        char *const fallback[] = {"rofi", "-show", "drun", NULL};
        launch("dmenu_run", fallback);
    } else if (state == Mod1Mask && key == XK_Tab && nclients > 0) {
        for (i = 0; i < (int)nclients; i++)
            if (clients[i].win == focused)
                break;
        for (i = 1; i <= (int)nclients; i++) {
            Client *next = &clients[((i + (i <= (int)nclients ? 0 : 0)) %
                    (int)nclients)];
            if (focused == None || next->win == focused)
                continue;
            if (next->desktop == desktop) {
                focus_window(next->win);
                break;
            }
        }
    } else if (state == Mod1Mask && key == XK_t) {
        tiled = !tiled;
        arrange();
    } else if (state == Mod1Mask && key == XK_f && c != NULL) {
        if (!c->maximized) {
            c->x = 0;
            c->y = 0;
            c->w = screen_w;
            c->h = screen_h;
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
        "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_STRUT_PARTIAL"};
    unsigned long supported[8], value;
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

    supported[0] = atoms[NET_SUPPORTED];
    supported[1] = atoms[NET_CLIENT_LIST];
    supported[2] = atoms[NET_ACTIVE_WINDOW];
    supported[3] = atoms[NET_CURRENT_DESKTOP];
    supported[4] = atoms[NET_NUMBER_OF_DESKTOPS];
    supported[5] = atoms[NET_WM_NAME];
    supported[6] = atoms[NET_WM_STATE];
    supported[7] = atoms[NET_WM_STATE_FULLSCREEN];
    XChangeProperty(dpy, root, atoms[NET_SUPPORTED], XA_ATOM, 32,
            PropModeReplace, (unsigned char *)supported, 8);
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
    publish_workarea();
    {
        char path[1024];
        const char *home = getenv("HOME");
        if (home != NULL && snprintf(path, sizeof(path),
                "%s/.config/tinywm/autostart.sh", home) < (int)sizeof(path) &&
                access(path, X_OK) == 0)
            launch(path, NULL);
    }
    XSync(dpy, False);
    for (;;) {
        XNextEvent(dpy, &ev);
        if (ev.type == MapRequest)
            manage(ev.xmaprequest.window);
        else if (ev.type == DestroyNotify)
            unmanage(ev.xdestroywindow.window);
        else if (ev.type == UnmapNotify) {
            Client *c = client_for(ev.xunmap.window);
            if (c != NULL) {
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
        } else if (ev.type == ButtonRelease)
            drag.subwindow = None;
        else if (ev.type == KeyPress)
            key_press(&ev.xkey);
        else if (ev.type == EnterNotify && FOCUS_FOLLOWS_MOUSE &&
                ev.xcrossing.mode == NotifyNormal)
            focus_window(ev.xcrossing.window);
        else if (ev.type == ClientMessage &&
                ev.xclient.message_type == atoms[NET_WM_STATE] &&
                ((Atom)ev.xclient.data.l[1] ==
                 atoms[NET_WM_STATE_FULLSCREEN] ||
                 (Atom)ev.xclient.data.l[2] ==
                 atoms[NET_WM_STATE_FULLSCREEN])) {
            Client *c = client_for(ev.xclient.window);
            int action = (int)ev.xclient.data.l[0];
            if (c != NULL)
                set_fullscreen(c, action == 1 ||
                        (action == 2 && !c->fullscreen));
        } else if (ev.type == PropertyNotify &&
                ev.xproperty.atom == atoms[NET_WM_STRUT_PARTIAL])
            publish_workarea();
    }
}

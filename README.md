# TinyWM

TinyWM is written by Nick Welch <nick@incise.org> in 2005 & 2011.
and got rewritten by Rayan Koubba <koubbamohamedrayan@gmail.com> in october 2026

This software is in the public domain and is provided AS IS, with NO WARRANTY.

TinyWM began as a ridiculously tiny window manager and learning example. This
fork adds practical X11 desktop features while keeping the implementation in a
single, readable C file and depending only on Xlib. It is an X11 window manager,
not a Wayland compositor.

## Features

- Alt+left-drag moves windows; Alt+right-drag resizes them; Alt+F1 raises the
  focused window.
- Click-to-focus by default, optional focus-follows-mouse, and focused/unfocused
  colored borders.
- Nine workspaces, configurable borders, and a master/stack tiling toggle;
  windows otherwise float.
- Alt+F toggles maximize. Fullscreen requests using `_NET_WM_STATE_FULLSCREEN`
  are also handled.
- EWMH desktop/client/active-window properties, dock/dialog/splash window types,
  and dock struts for the work area.
- Handles map, configure, destroy, and unmap events, requests WM_DELETE_WINDOW
  before falling back to XKillClient, reaps children, and runs an optional
  autostart script.

Multiple monitors are not currently managed independently; TinyWM uses the
default X screen. Workspaces and struts are basic implementations intended for
simple X11 setups.

## Keybindings

| Shortcut | Action |
| --- | --- |
| Alt+Button1, drag | Move window |
| Alt+Button3, drag | Resize window |
| Alt+F1 | Raise focused window |
| Alt+1..9 | Switch workspace |
| Alt+Shift+1..9 | Move focused window to workspace |
| Alt+Q | Close focused window |
| Alt+Enter | Launch terminal (`xterm` by default) |
| Alt+P | Launch `dmenu_run`, falling back to rofi |
| Alt+Tab | Cycle through windows on the current workspace |
| Alt+F | Toggle maximize |
| Alt+Arrow | Snap focused window to a screen half |
| Alt+T | Toggle master/stack tiling |
| Alt+Shift+Q | Quit TinyWM |

## Configuration

Edit `config.h`, then rebuild. `BORDER_WIDTH`, `FOCUS_COLOR`, and
`UNFOCUS_COLOR` configure borders; `FOCUS_FOLLOWS_MOUSE` can be set to `1`;
`TERMINAL` sets the terminal command. The defaults are deliberately modest.

TinyWM runs `~/.config/tinywm/autostart.sh` when the file exists and is
executable. Use it to start a panel, compositor, or other session utilities.

## Installation guide

### Dependencies

TinyWM needs a C compiler, `make`, `pkg-config`, Xlib development headers, and an
X11 server. XRandR and Xinerama development packages are not required; optional
multi-monitor support is not implemented yet.

Install tools and Xlib headers using your distribution's package manager:

| Distribution | Command |
| --- | --- |
| Debian / Ubuntu / Linux Mint / Pop!_OS | `sudo apt install build-essential libx11-dev pkg-config` |
| Fedora | `sudo dnf install gcc make pkgconf-pkg-config libX11-devel` |
| RHEL / CentOS / Rocky / Alma | `sudo dnf install gcc make pkgconf-pkg-config libX11-devel` |
| Arch / Manjaro / EndeavourOS | `sudo pacman -S --needed base-devel libx11` |
| openSUSE | `sudo zypper install gcc make pkg-config libX11-devel` |
| Gentoo | `sudo emerge --ask sys-devel/gcc sys-devel/make x11-libs/libX11 dev-util/pkgconf` |
| Void Linux | `sudo xbps-install -S base-devel libX11-devel pkg-config` |
| Alpine Linux | `sudo apk add build-base libx11-dev pkgconf` |

On NixOS, build in a temporary development shell:

```sh
nix-shell -p gcc gnumake pkg-config xorg.libX11
make
```

Alternatively, add `gcc`, `gnumake`, `pkg-config`, and `xorg.libX11` to
`environment.systemPackages` in your NixOS configuration and rebuild the
system.

Slackware users should install the X development packages and a compiler from
their SlackBuilds or distribution media. On FreeBSD, install `x11/libX11`,
`devel/pkgconf`, and `devel/gmake`; on OpenBSD, install the X development
headers and `pkgconf` from packages. BSD users may need GNU Make (`gmake`).

### Build and install

From the repository directory:

```sh
make
sudo make install
```

By default, the executable is installed to `/usr/local/bin` and the display
manager session file to `/usr/local/share/xsessions`. Set a different prefix
when needed, for example `sudo make PREFIX=/usr install`. Package builders can
stage installation using `make DESTDIR=/path/to/package install`.

### Starting TinyWM

- **Display manager:** choose the TinyWM session from the session selector.
  The installed `tinywm.desktop` registers it with display managers that scan
  the `xsessions` directory. Log out to return to your previous session.
- **startx:** add `exec tinywm` as the final line of `~/.xinitrc`, then run
  `startx`.
- **Safe testing with Xephyr:** from an existing X session, start a nested
  server with `Xephyr :1 -screen 1024x768 &`, then run `DISPLAY=:1 tinywm` in
  another terminal. Open test clients on that display with `DISPLAY=:1 xterm`.
  Exit TinyWM with Alt+Shift+Q and stop Xephyr when finished.

### Uninstall

Run `sudo make uninstall` with the same `PREFIX` used during installation.
Alternatively, remove the installed `tinywm` executable and
`tinywm.desktop` from their respective prefix paths.

### Troubleshooting

- If `make` cannot find Xlib, install the development package above and verify
  that `pkg-config --cflags --libs x11` succeeds.
- If starting TinyWM reports another window manager already owns the display,
  end the existing X11 session or test in a separate Xephyr display.
- If the display manager does not list TinyWM, confirm
  `tinywm.desktop` is installed under the display manager's `xsessions`
  directory and that `tinywm` is on `PATH`.
- A terminal or launcher may not be installed. Set `TERMINAL` in `config.h`;
  install `dmenu` or `rofi` for Alt+P.
- For startup applications, make `~/.config/tinywm/autostart.sh` executable.

## Original project notes

Another very small window manager is failsafewm. TinyWM originally started as a
rewrite of it, removing what its author considered unnecessary:
<http://freshmeat.net/projects/failsafewm/>.

Another small, but larger than TinyWM, window manager is aewm, which is also a
useful example for learning about window manager programming:
<http://www.red-bean.com/~decklin/aewm/>.

The original `annotated.c` and Python example are retained as historical
reference examples of the original TinyWM implementation.

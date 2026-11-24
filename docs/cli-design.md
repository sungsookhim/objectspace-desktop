# The command line in WPS: WPS Terminal and the `wps` command

Draft 2, 2026-11-23; decided by the owner the same day (below). **The owner's aim:** program objects (scripts, text-mode programs) run in a WPS-native terminal, not in Ptyxis or another terminal app. Those apps are not friendly to WPS's workspace.

## Why another terminal

Ptyxis, GNOME Console and GNOME Terminal run one process for all their windows. A program object started in one hands its window to that process, so WPS loses track of it:

- **Work Areas:** its window is not in the work area's group (0.4 steps 9 and 10).
- **In use:** its object shows in use only for a moment.
- **Close window on exit:** this needs a wrapper script.
- **Looks and drops:** the window follows neither the scheme nor WPS's fonts, and takes no WPS drops.

Terminals that run a process per window (xterm, foot) are better at the first three, but they do not look or act like WPS.

So: **WPS Terminal** (`wps-terminal`), WPS's own terminal, one process per window, drawn by WPS's toolkit. Like OS/2's **OS/2 Window**, it is where WPS's text-mode programs run. Scripts also get the **`wps` command**, to work with objects (as REXX scripts did on OS/2).

## 1. WPS Terminal

- **A real terminal.**
  - The program runs on a pseudo-terminal. **libvterm**, Fedora's `libvterm` package (the library Neovim's terminal uses), keeps the screen: VT100 and xterm escapes, 16, 256 and true colours, the alternate screen.
  - `vim`, `top`, `less`, `ssh`, `sudo`'s password prompt and progress bars work.
  - WPS draws the cells with its own text code, so Korean and other wide characters work as in folders, and input methods (IBus) type into it as into any WPS field.
- **One process per window, started for the program.**
  - wpsd starts `wps-terminal -- COMMAND...` for a program object, and the terminal is the process it tracks.
  - So the object is in use while the program runs, its window belongs to its work area's group, and it gets its scope.
  - **Close window on exit:**
    - On: the window closes when the program ends.
    - Off: the window stays and says *Ended (status N). Press Enter to close*.
    - No wrapper script.
- **A WPS window:**
  - **Frame:** the scheme's frame, and the scheme's colours for the default foreground and background.
  - **Font:** the **Terminal font**, a new System font beside Menus, Title bars and Dialogs (a monospace Font Palette name; a font dropped on the window sets it).
  - **Drops:** an object dropped on the window is typed in as its quoted path.
  - **Copy and paste:** select with the mouse; copy and paste with Ctrl+Shift+C and Ctrl+Shift+V, or the window's menu.
  - **Bell:** the Sound object's `terminal.bell` event.
  - **Title:** the program's title (it can set one), else the object's.
  - **Scrollback:** a scroll bar, Shift+Page Up and Shift+Page Down, and kept lines (the **Scrollback lines** setting, 5000 by default).
- **Command Prompt:**
  - **What it is:** a WPS Terminal running the user's shell, in a folder.
  - **Where:** a **Command Prompt** object in the Programs folder, and **Command prompt here** on every folder's menu.
  - **What it starts with:** `$WPS_FOLDER` set to that folder, and the `wps` command at hand.
- **The System's Programs page:** **WPS Terminal** becomes the first choice for text-mode programs, and the default for new users and on upgrade. Ptyxis, xterm and the others stay as choices for those who want them. Their windows are outside work areas, as now.
- **Not in the first version:** tabs, split panes, a profile per program, ligatures, and images in the terminal (sixel, kitty).

## 2. The `wps` command: WPS for scripts

A shell command whose output is plain text (one value a line, exit status 0 or 1), unlike `wpsctl`'s JSON. Underneath it makes the same requests to wpsd.

| Command | Does |
|---|---|
| `wps open PATH...` | opens each, as a double click would |
| `wps get PATH NAME` | prints a property's value |
| `wps set PATH 'NAME=VALUE;...'` | sets properties (`class:WPFolder`, `type:image/png` too) |
| `wps create FOLDER CLASS TITLE` | makes an object; prints its path |
| `wps shadow PATH FOLDER` | makes a shadow; prints its path |
| `wps say [--stay] TEXT` | a message at the bottom of the desktop |
| `wps notify TITLE [TEXT]` | a notification (Messages) |
| `wps ask TEXT [BUTTON...]` | a WPS dialog; prints the button chosen |
| `wps folder` | the folder the program was started from (`$WPS_FOLDER`) |
| `wps selection` | the paths selected in the active folder window |

Scripts already work as program objects. Objects dropped on them are their arguments, and their **Actions** (`run:`) start them with `WPS_OBJECT`, `WPS_PATH` and `WPS_EVENT` set. A script in WPS Terminal can now talk back to WPS too.

## Decided (the owner's, 2026-11-23)

1. **libvterm**, to build and run (`libvterm-devel` on the build machine).
   - **What it covers:** VT100 and xterm escapes, 16, 256 and true (24-bit) colours, the alternate screen, mouse reporting, bracketed paste, UTF-8 with wide characters. So `htop`, `btop` and Claude Code run.
   - **What WPS adds:** the glyphs (box drawing, braille, symbols) come from the Terminal font, or from the fallback fonts as in folders.
   - **What does not work:** images in the terminal (sixel, kitty), and the kitty keyboard protocol. Programs fall back from both.
   - **Tested:** `htop`, `btop` (where installed), true colours and wide characters are in the checks.
2. **0.4.1:** 0.4.0 is not released. These are the plan's steps 12 to 14, then the release as 0.4.1.
3. **WPS Terminal is the default** for text-mode programs, like OS/2's command window, for new users and on upgrade. The System's Programs page offers it first, then **The system's default terminal** (xdg-terminal-exec, else the first one installed), then the named ones (Ptyxis, xterm, ...) and a command of one's own.
4. **The command is `wps` for now.** WPS's own names (`wps`, `wpsctl`, WPS Terminal, ...) are to be looked at in 0.5, against name conflicts and trademarks.

## Steps (0.4.1)

- **Step 12. WPS Terminal, the terminal (M-L) (VM):**
  - `gui/terminal/`, using the toolkit; libvterm's screen drawn with WPS's text code (wide characters, colours, bold, underline, the cursor).
  - The pseudo-terminal, keys, resize, scrollback, selection, copy and paste, drops, the title, the bell.
  - **Checks:** unit tests (the screen to cells, keys to bytes); headless: `printf` with colours and Korean drawn as expected, `vim` opened and quit, a resize, a drop typed in.
- **Step 13. Program objects in it (M) (VM):**
  - wpsd starts `wps-terminal` for text-mode programs (the System's choice, its new default), with Close window on exit.
  - The Terminal font. **Command Prompt** and **Command prompt here**.
  - **Checks:** headless:
    - a script object in a work area runs in WPS Terminal, minimized and restored with the work area;
    - its object is in use while it runs;
    - the window closes on exit, or stays with its status.
- **Step 14. The `wps` command (S):**
  - Its commands; `wps ask` and `wps selection` through the desktop.
  - **Checks:** each command against a running wpsd; headless: `wps ask` answered, `wps selection` read.

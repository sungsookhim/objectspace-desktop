# The command line in WPS: Command Prompt and the `wps` command

Draft, 2026-11-23, for the owner's review. The owner's ask: a simple WPS command-line app, so that basic scripts fit into WPS easily, alongside a full-featured terminal (GNOME's or another one already installed).

OS/2 had the same split: the **OS/2 Window** (a command prompt that was a WPS object) and **REXX** scripts that could open, make and change objects (`SysCreateObject`, `SysSetObjectData`). This design has three parts, smallest first.

## 1. The `wps` command: WPS for scripts

A shell command, installed with WPS, whose output is plain text (one value a line, exit status 0 or 1), not `wpsctl`'s JSON. Scripts call it the way REXX scripts called the WPS functions.

| Command | Does |
|---|---|
| `wps open PATH...` | opens each, as a double click would (a folder's window, a file in its program) |
| `wps get PATH NAME` | prints a property's value (`wps get ~/Work icon_font`) |
| `wps set PATH 'NAME=VALUE;...'` | sets properties with a setup string; `class:WPFolder` and `type:image/png` too |
| `wps create FOLDER CLASS TITLE` | makes an object, prints its path |
| `wps shadow PATH FOLDER` | makes a shadow, prints its path |
| `wps say [--stay] TEXT` | a message at the bottom of the desktop |
| `wps notify TITLE [TEXT]` | a notification (Messages) |
| `wps ask TEXT [BUTTON...]` | a WPS dialog; prints the button chosen (scripts that need a yes or no) |
| `wps folder` | the folder the Command Prompt or the action was started from (`$WPS_FOLDER`) |
| `wps selection` | the paths selected in the active folder window, one a line |

It is `wpsctl` underneath (the same requests to wpsd). `wps ask` and `wps selection` go to the desktop, which has the windows, through a new request that wpsd passes on.

What scripts can already do stays: a script in a folder is a program object (its **Program** page: window or text mode, its working folder). Objects dropped on it are its arguments. An object's **Actions** (`run:`) start it with `WPS_OBJECT`, `WPS_PATH` and `WPS_EVENT` set.

## 2. Command Prompt: a WPS window for commands

A new object, **Command Prompt** (class WPCommandPrompt, in the Programs folder and on a folder's menu as **Command prompt here**). It is a WPS window drawn by the toolkit, like the other WPS windows. It is not a terminal emulator.

- **One line in, output out:**
  - An input line at the bottom, with history (Up and Down) and Tab completion of file names.
  - The output above it, scrolling, in the scheme's colours and the **Command Prompt font**, a monospace property.
  - Error output in another colour. Each command's exit status shows when it is not 0.
- **A real shell behind it:** one `bash` (or `sh`) per window, on pipes, not a pseudo-terminal. `cd`, variables and functions last for the window's life.
  - After each command the window learns its end, its status and the folder now in use. The shell prints a marker it can recognise.
  - The title shows the folder: *Command Prompt - ~/Work*.
- **Ctrl+C** interrupts the running command (its process group). **Ctrl+D**, or `exit`, closes the window.
- **Drag and drop:** objects dropped on it are typed in as their quoted paths. A command's output can be selected and copied.
- **Two-way with folders:**
  - `wps open .` (or just `open .`, an alias it sets) opens the current folder's window.
  - **Command prompt here** on a folder opens one in that folder, with `$WPS_FOLDER` set.
- **What it does not do:** full-screen and interactive programs (`vim`, `top`, `ssh`, `less`). They need a terminal, and without one they would hang or print garbage. Common ones are recognised (a short list, plus `TERM=dumb` for the rest); the window then says so and offers **Run in terminal**.

## 3. The full terminal: the user's own

A full terminal is the one already chosen on the System's **Programs** page (Ptyxis, GNOME Console, GNOME Terminal, Konsole, xterm, ... or a command). WPS does not build one.

- **Terminal here** on a folder's menu (beside Command prompt here) opens it in that folder.
- In Command Prompt, **Ctrl+Shift+T**, or the **Terminal** button, opens it in the prompt's folder.
- **Run in terminal** (above) runs the command there. With Close window on exit off, the terminal stays open afterwards.
- A script's Program page gains a third type, **Command Prompt window**, beside window and text mode. The script's output then shows in a Command Prompt window, which stays open if the script fails.

## What the owner decides

1. **Its place in the plan:**
   - **Proposed:** as 0.4's steps 12 and 13, before the release (the owner's wish: "before we publish 0.4"). The RPMs are rebuilt and the draft release updated afterwards.
   - Or as 0.4.1, right after the release.
2. **Command Prompt runs commands without a terminal** (pipes, not a pseudo-terminal):
   - **Proposed.** It is simple and safe, and its text is WPS's own (fonts, colours, copy, drops). Interactive programs go to the full terminal.
   - The other way, a terminal emulator of our own, is much bigger: it means VT100 escapes, a pseudo-terminal, and full-screen programs.
3. **The shell:**
   - **Proposed:** the user's login shell when it is bash, zsh or sh; else bash.
   - (A fish or nu prompt marker would need its own form.)
4. **Is `wps` a new command, or `wpsctl` with plain output?**
   - **Proposed:** `wps`, a short name for scripts. `wpsctl` stays as the service's tool, with its JSON.

## Steps (if approved)

- **Step 12. The `wps` command (S):**
  - Its commands as above; `wps ask` and `wps selection` through the desktop.
  - **Terminal here** on folders' menus.
  - **Checks:** a shell test of each command against a running wpsd. Headless: `wps ask` answered, `wps selection` from a folder window.
- **Step 13. Command Prompt (M) (VM):**
  - The class and its window: the shell on pipes, the marker, history, completion, Ctrl+C, drops.
  - **Command prompt here**, Run in terminal, and the Program page's third type.
  - **Checks:**
    - unit tests for the marker parsing and the completion;
    - headless: `cd`, then `ls`, the title following; a failing command's status; Ctrl+C on `sleep`; a drop typed in; `vim` offered to the terminal.

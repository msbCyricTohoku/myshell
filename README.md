# myshell

A minimal C shell.

## Features
- Dynamic Alias Engine (alias ll="ls -la")
- Persistent History (~/.myshell_history)
- Zsh-Style Prefix History Search (Up/Down Arrows)
- Live Git Branch Prompt Indicator
- Command Execution Time Tracking ([Xs])
- Ctrl+Left/Right Word Jumping
- Startup Script Auto-Loader (~/.myshellrc)
- Built-ins: alias, unalias, export, source, cd, exit
- Infinite Pipeline Chaining (ls | grep | wc)
- Logical Operators (&&, ||, ;) & Backgrounds (&)
- Wildcard Globbing Expansion (*.c, ?)
- Grid-Style Tab Autocompletion Menu


## Installation

Compile with standard C compiler (`gcc`) and `make`.

1. Clone the repository and compile:
   ```bash
   make
   ```

## Install it system-wide (requires root):

`sudo make install`

## Theming

To change the shell colors, open myshell.c and change the THEME macro at the top:

1: Ocean (Cyan / Blue)

2: Hacker (Matrix Green)

3: Sunset (Magenta / Yellow)

## Uninstallation

`sudo make uninstall`

## Developer

`developed by msb- 2026`


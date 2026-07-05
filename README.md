# mytop — A Linux System Monitor (htop-like)

A terminal-based system monitor inspired by `htop`, built with a C backend
collector and a Python (Rich) frontend UI.

## Components

- **collector.c** — Reads system stats from `/proc` (CPU, memory, swap, load
  average, uptime, and per-process info) and emits one JSON object per second
  to stdout. Uses POSIX threads: a worker thread collects and emits data while
  the main thread listens for keyboard input (`q`/`Q` or Ctrl-C) to trigger a
  graceful shutdown via a mutex + condition variable.

- **python.py (ui.py)** — A Rich-based terminal UI that reads the JSON stream
  from the collector and renders an htop-like dashboard: CPU/memory/load
  panels and a scrollable, sortable, filterable, searchable process table.

- **script.sh** — Convenience script that compiles the C collector and pipes
  its output into the Python UI in one step.

## Requirements

- GCC and a POSIX-compliant Linux environment (uses `pthread`, `/proc`,
  `termios`)
- Python 3
- Python packages: `rich`, `readchar`

Install Python dependencies:
```bash
pip install rich readchar
```

## Build & Run

### Option 1: Using the script
```bash
chmod +x script.sh
./script.sh
```
You'll be prompted for the C program name (without `.c`) and the Python
program name (without `.py`).

### Option 2: Manual
```bash
gcc -O2 -o collector collector.c -lpthread
./collector | python3 python.py
```

## Key Bindings (UI)

| Key                | Action                                                             |
|--------------------|--------------------------------------------------------------------|
| ↑ / ↓              | Scroll process list one row                                        |
| Page Up / Down     | Scroll one page                                                    |
| Home / End         | Jump to top / bottom of list                                       |
| 1–8                | Sort by CPU%, MEM%, PID, PRI, NI, VIRT, RES, SHR (toggle asc/desc) |
| `/`                | Enter filter mode (Esc to cancel, Enter to apply)                  |
| `s` / `S`          | Enter search/highlight mode                                        |
| `Esc`              | Clear active filter and search                                     |  
| `q` / `Q` / Ctrl-C | Quit                                                               |

## Architecture Notes

- The collector and UI communicate via a simple line-delimited JSON protocol
  over a pipe (one JSON object per line, emitted roughly every second).
- The UI uses `/dev/tty` for keyboard input so it doesn't interfere with the
  piped stdin stream from the collector.
- The UI runs multiple daemon threads (sort, filter, search, key listener)
  coordinated via `queue.Queue` for responsive, non-blocking interaction.

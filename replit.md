# AgenticAI CEP — Transit Terminal Departures Board

## Project Overview
A C-language terminal application that reads transit route data from a text file and displays a real-time departures board with live countdowns, similar to what you'd see at a transit terminal.

## Files

| File | Purpose |
|---|---|
| `departures.c` | Main C source — departures board logic + rendering |
| `routes.txt` | Route data file (route number, stop name, frequency, offset) |
| `Makefile` | Build rules for gcc |
| `server.js` | Minimal HTTP landing page (Replit preview) |
| `public/index.html` | Landing page explaining how to run the C program |

## Building & Running

```bash
# Compile (Linux / Seneca Matrix)
gcc -Wall -std=c99 -o departures departures.c

# Or use make
make

# Run with default routes file
./departures

# Run with a custom routes file
./departures my_routes.txt
```

## Routes File Format

```
# ROUTE_NUMBER  STOP_NAME  FREQUENCY_MINS  OFFSET_SECS
501  Queen   10   0
504  King     8  90
```

- `FREQUENCY_MINS` — how often the route departs (headway)
- `OFFSET_SECS` — stagger offset so routes don't all show the same time

## Features
- Live countdown: "Arriving in 4 min", "Due in 52 sec", "NOW DEPARTING"
- Colour-coded rows: green (< 5 min), yellow (< 1 min), red (departing now)
- Sorts all upcoming departures across all routes by next arrival time
- 60-minute lookahead window, refreshes every second
- Graceful Ctrl+C exit restores the terminal cursor

## Compatibility
- Standard C99, POSIX (`unistd.h`, `signal.h`)
- Tested with GCC 14 on Linux (NixOS)
- Compatible with Seneca Matrix (gcc)
- No external dependencies beyond the C standard library

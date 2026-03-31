# Transit Terminal Departures Board

A C-language terminal application that simulates a live transit departures board.  
The program reads route and stop information from a text file and displays a real-time countdown for departures in a Linux terminal.

## Features
- Written in standard C
- Compiles with GCC on Linux and Matrix
- Reads transit route data from a text file
- Live-updating departures board
- TTC-inspired colour theme
- Eastbound and Westbound view toggle
- Service status display
- Timezone offset support using a directive in the routes file

## Compile

Using `make`:
```bash
make
./departures

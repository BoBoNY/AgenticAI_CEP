/*
 * Transit Terminal Departures Board
 *
 * Reads route data from a text file and displays a live terminal
 * board showing upcoming departures with real-time countdowns.
 * Press E / W to switch between Eastbound and Westbound views.
 * Press Tab or Space to toggle.  Press Q (or Ctrl+C) to quit.
 *
 * Compile:  gcc -Wall -std=c99 -o departures departures.c
 * Run:      ./departures [routes_file] [utc_offset_hours]
 *
 *   routes_file      : path to routes data file  (default: routes.txt)
 *   utc_offset_hours : e.g. -4 for EDT, -5 for EST
 *                      Overrides the UTC_OFFSET directive in the file.
 *
 * Compatible with Linux and Seneca Matrix (gcc).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <termios.h>
#include <sys/select.h>

/* ── Constants ──────────────────────────────────────────────── */
#define MAX_ROUTES      100
#define MAX_DEPARTURES  300
#define MAX_NAME_LEN     64
#define SHOW_ROWS        15   /* rows displayed at once */
#define LOOKAHEAD_MINS   60   /* minutes ahead to generate slots */

#define DIR_EAST  0
#define DIR_WEST  1

/* ── ANSI escape helpers ────────────────────────────────────── */
#define CLEAR_SCREEN    "\033[2J\033[H"
#define CURSOR_HIDE     "\033[?25l"
#define CURSOR_SHOW     "\033[?25h"
#define BOLD            "\033[1m"
#define RESET           "\033[0m"

/*
 * TTC Brand Palette
 *   TTC Red  : #DA291C  — closest 256-colour cell: 160 (#d70000)
 *   Black    : #000000  — cell 16
 *   White    : #FFFFFF  — standard bright white
 *   Amber    : #FFB300  — cell 214 (#ffaf00)  used on TTC info screens
 */
#define TTC_RED_BG      "\033[48;5;160m"   /* TTC Red background            */
#define TTC_RED_FG      "\033[38;5;160m"   /* TTC Red foreground            */
#define TTC_DKRED_BG    "\033[48;5;88m"    /* dark red — column-header band */
#define TTC_BLACK_BG    "\033[48;5;16m"    /* true black — even rows        */
#define TTC_DKGRAY_BG   "\033[48;5;233m"   /* near-black — odd rows         */
#define TTC_DEPART_BG   "\033[48;5;52m"    /* deep red — departing now row  */
#define TTC_AMBER_FG    "\033[38;5;214m"   /* amber — normal countdown      */
#define TTC_WHITE_FG    "\033[97m"          /* white — destination text      */
#define TTC_LGRAY_FG    "\033[37m"          /* light gray — departs-at time  */
#define TTC_DGRAY_FG    "\033[90m"          /* dark gray — inactive tabs     */
#define TTC_BRED_FG     "\033[91m"          /* bright red — NOW DEPARTING    */
#define TTC_BWHITE_FG   "\033[97m"          /* bright white — arriving soon  */

/* ── Data structures ────────────────────────────────────────── */
typedef struct {
    int  route_num;
    char stop_name[MAX_NAME_LEN];
    int  freq_mins;
    int  offset_secs;
    int  direction;   /* DIR_EAST or DIR_WEST */
} Route;

typedef struct {
    int  route_num;
    char stop_name[MAX_NAME_LEN];
    long depart_at;
    int  direction;
} Departure;

/* ── Globals ─────────────────────────────────────────────────  */
static volatile int     running     = 1;
static long             g_tz_offset = 0;
static int              g_direction = DIR_EAST;  /* current view */
static struct termios   g_orig_tty;              /* saved terminal state */

/* ── Terminal raw-mode helpers ──────────────────────────────── */
/*
 * Put stdin in raw mode: characters are available immediately,
 * without waiting for Enter.  Echo is suppressed.
 */
static void set_raw_mode(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_tty);
    raw          = g_orig_tty;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_tty);
}

/* ── Signal handler ─────────────────────────────────────────── */
static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

/* ── Timezone helpers ───────────────────────────────────────── */
static long detect_tz_offset(void)
{
    time_t    t  = time(NULL);
    struct tm gm = *gmtime(&t);
    return (long)t - (long)mktime(&gm);
}

static struct tm *display_tm(time_t t, struct tm *out)
{
    time_t adj = t + (time_t)g_tz_offset;
    *out = *gmtime(&adj);
    return out;
}

/* ── File parsing ───────────────────────────────────────────── */
/*
 * Parses routes and the optional UTC_OFFSET directive.
 * Direction field: 'E' or 'e' → DIR_EAST, 'W' or 'w' → DIR_WEST.
 * Routes without a direction field default to DIR_EAST.
 */
static int load_routes(const char *filename, Route *routes, int max,
                       long *file_tz_offset)
{
    FILE *fp;
    char  line[256];
    int   count = 0;

    fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s'\n", filename);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && count < max) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n') continue;

        /* UTC_OFFSET directive */
        if (strncmp(p, "UTC_OFFSET", 10) == 0) {
            double hours = 0.0;
            if (sscanf(p + 10, "%lf", &hours) == 1)
                *file_tz_offset = (long)(hours * 3600.0);
            continue;
        }

        if (*p == '#') continue;

        Route r;
        char  dir_ch = 'E';   /* default */
        int   fields = sscanf(p, "%d %63s %d %d %c",
                              &r.route_num, r.stop_name,
                              &r.freq_mins, &r.offset_secs, &dir_ch);

        if (fields >= 4) {
            if (r.freq_mins < 1)   r.freq_mins   = 1;
            if (r.offset_secs < 0) r.offset_secs = 0;
            r.direction = (dir_ch == 'W' || dir_ch == 'w') ? DIR_WEST : DIR_EAST;
            routes[count++] = r;
        }
    }

    fclose(fp);
    return count;
}

/* ── Departure generation ───────────────────────────────────── */
/*
 * Builds upcoming departure slots for routes matching `direction`.
 */
static int build_departures(const Route *routes, int nroutes,
                             Departure   *deps,   int max_deps,
                             long now,            int direction)
{
    int  count  = 0;
    long window = (long)LOOKAHEAD_MINS * 60;

    for (int i = 0; i < nroutes && count < max_deps; i++) {
        const Route *r = &routes[i];
        if (r->direction != direction) continue;

        long period     = (long)r->freq_mins * 60;
        long phase      = (now + r->offset_secs) % period;
        long first_next = now + (period - phase);

        for (long t = first_next; t <= now + window && count < max_deps; t += period) {
            Departure d;
            d.route_num  = r->route_num;
            d.depart_at  = t;
            d.direction  = r->direction;
            strncpy(d.stop_name, r->stop_name, MAX_NAME_LEN - 1);
            d.stop_name[MAX_NAME_LEN - 1] = '\0';
            deps[count++] = d;
        }
    }
    return count;
}

/* ── Sorting ─────────────────────────────────────────────────── */
static int cmp_departure(const void *a, const void *b)
{
    const Departure *da = (const Departure *)a;
    const Departure *db = (const Departure *)b;
    if (da->depart_at < db->depart_at) return -1;
    if (da->depart_at > db->depart_at) return  1;
    return da->route_num - db->route_num;
}

/* ── Countdown label (TTC style) ─────────────────────────────── */
/*
 * "NOW DEPARTING"  — bright-red bold on deep-red bg  (handled by draw_board)
 * < 60 s           — bright white bold  "  0:42"
 * < 5 min          — bright white bold  "  3:10"
 * >= 5 min         — amber              "  8:22"
 *
 * All values are printed as  MM:SS  to match real TTC info screens.
 */
static void print_countdown(long secs, const char *row_bg)
{
    if (secs <= 0) {
        /* handled per-row in draw_board; nothing extra needed */
        return;
    }
    int  mins = (int)(secs / 60);
    int  rem  = (int)(secs % 60);
    char buf[16];
    snprintf(buf, sizeof(buf), "%2d:%02d", mins, rem);

    if (secs < 300)
        printf("%s%s  %s%-8s%s", row_bg, BOLD, TTC_BWHITE_FG, buf, RESET);
    else
        printf("%s  %s%-8s%s", row_bg, TTC_AMBER_FG, buf, RESET);
}

/* ── Direction tab bar (TTC style) ──────────────────────────── */
/*
 * Active tab  : TTC Red background, white bold text, arrow indicator.
 * Inactive tab: black background, dark-gray text.
 */
static void print_direction_bar(void)
{
    if (g_direction == DIR_EAST) {
        printf("%s%s%s >> EASTBOUND %s", TTC_RED_BG, BOLD, TTC_WHITE_FG, RESET);
        printf(" ");
        printf("%s%s    Westbound %s", TTC_BLACK_BG, TTC_DGRAY_FG, RESET);
    } else {
        printf("%s%s    Eastbound %s", TTC_BLACK_BG, TTC_DGRAY_FG, RESET);
        printf(" ");
        printf("%s%s%s << WESTBOUND %s", TTC_RED_BG, BOLD, TTC_WHITE_FG, RESET);
    }
}

/* ── Board renderer (TTC style) ─────────────────────────────── */
static void draw_board(const Departure *deps, int ndeps, long now, int nroutes)
{
    struct tm tbuf;

    printf(CLEAR_SCREEN);

    /* ════════════════════════════════════════════════════════════
     * Row 1 — TTC Red header bar
     *   [ TTC ]  REAL-TIME DEPARTURE BOARD      13:44:18
     * ════════════════════════════════════════════════════════════ */
    printf("%s%s", TTC_RED_BG, BOLD);
    printf("  %sTTC%s ", TTC_BLACK_BG, TTC_RED_BG);   /* TTC badge: black-on-red inset */
    printf("%s  REAL-TIME DEPARTURE BOARD", TTC_WHITE_FG);

    display_tm((time_t)now, &tbuf);
    printf("%25s%02d:%02d:%02d  %s\n", "",
           tbuf.tm_hour, tbuf.tm_min, tbuf.tm_sec, RESET);

    /* ════════════════════════════════════════════════════════════
     * Row 2 — Direction tab strip
     * ════════════════════════════════════════════════════════════ */
    printf("%s  ", TTC_BLACK_BG);
    print_direction_bar();
    printf("%s\n", RESET);

    /* ════════════════════════════════════════════════════════════
     * Row 3 — Column headers on dark-red band
     * ════════════════════════════════════════════════════════════ */
    printf("%s%s%s", TTC_DKRED_BG, BOLD, TTC_WHITE_FG);
    printf("  %-6s  %-20s  %-10s  %-10s  %s\n",
           "ROUTE", "DESTINATION", "ARRIVES", "DEPARTS", "");
    printf("%s", RESET);

    /* ════════════════════════════════════════════════════════════
     * Separator — thin TTC red line
     * ════════════════════════════════════════════════════════════ */
    printf("%s", TTC_RED_FG);
    for (int i = 0; i < 72; i++) putchar('-');
    printf("%s\n", RESET);

    /* ════════════════════════════════════════════════════════════
     * Departure rows
     * ════════════════════════════════════════════════════════════ */
    int shown = 0;
    for (int i = 0; i < ndeps && shown < SHOW_ROWS; i++) {
        long secs_away = deps[i].depart_at - now;
        if (secs_away < -30) continue;

        /* choose row background */
        const char *row_bg;
        if (secs_away <= 0)
            row_bg = TTC_DEPART_BG;           /* departing — deep red */
        else
            row_bg = (shown % 2 == 0) ? TTC_BLACK_BG : TTC_DKGRAY_BG;

        printf("%s", row_bg);

        /* route number badge:  [501]  in TTC Red on current row bg */
        char badge[12];
        snprintf(badge, sizeof(badge), "[%d]", deps[i].route_num);
        printf("  %s%s%-6s%s", BOLD, TTC_RED_FG, badge, RESET);
        printf("%s  ", row_bg);

        /* destination */
        printf("%s%-20s%s  ", TTC_WHITE_FG, deps[i].stop_name, RESET);
        printf("%s", row_bg);

        /* countdown / status */
        if (secs_away <= 0) {
            printf("%s%s  %-10s%s  ", TTC_BRED_FG, BOLD, "DEPARTING", RESET);
        } else {
            print_countdown(secs_away, row_bg);
            printf("  ");
        }
        printf("%s", row_bg);

        /* scheduled departure time */
        display_tm((time_t)deps[i].depart_at, &tbuf);
        printf("%s  %02d:%02d   %s",
               TTC_LGRAY_FG, tbuf.tm_hour, tbuf.tm_min, RESET);

        printf("%s\n", RESET);
        shown++;
    }

    /* empty-row filler to hold fixed board height */
    for (int i = shown; i < SHOW_ROWS; i++) {
        const char *row_bg = (i % 2 == 0) ? TTC_BLACK_BG : TTC_DKGRAY_BG;
        printf("%s  %-6s  %-20s  %-10s  %-10s%s\n",
               row_bg, "---", "---", "---", "", RESET);
    }

    /* ════════════════════════════════════════════════════════════
     * Footer — TTC Red separator + key hints
     * ════════════════════════════════════════════════════════════ */
    printf("%s", TTC_RED_FG);
    for (int i = 0; i < 72; i++) putchar('-');
    printf("%s\n", RESET);

    int  off_h = (int)(g_tz_offset / 3600);
    int  off_m = (int)((g_tz_offset % 3600) / 60);
    if (off_m < 0) off_m = -off_m;
    char tz_label[32];
    snprintf(tz_label, sizeof(tz_label), "UTC%+d:%02d", off_h, off_m);

    printf("%s  Toronto Transit Commission  |  %d routes  |  %s  |  "
           "[E] East  [W] West  [Tab] Toggle  [Q] Quit%s\n",
           TTC_DGRAY_FG, nroutes, tz_label, RESET);

    fflush(stdout);
}

/* ── Entry point ─────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *filename = (argc > 1) ? argv[1] : "routes.txt";

    /* load routes + read UTC_OFFSET from file */
    long  file_tz = LONG_MIN;
    Route routes[MAX_ROUTES];
    int   nroutes = load_routes(filename, routes, MAX_ROUTES, &file_tz);
    if (nroutes <= 0) {
        fprintf(stderr, "No routes loaded.  Exiting.\n");
        return 1;
    }

    /* resolve timezone offset (CLI arg > file directive > system detect) */
    if (argc > 2) {
        g_tz_offset = (long)(atof(argv[2]) * 3600.0);
        printf("UTC offset (from argument): %+d h\n", (int)(g_tz_offset / 3600));
    } else if (file_tz != LONG_MIN) {
        g_tz_offset = file_tz;
        printf("UTC offset (from routes file): %+d h\n", (int)(g_tz_offset / 3600));
    } else {
        g_tz_offset = detect_tz_offset();
        printf("UTC offset (system detected): %+d h\n", (int)(g_tz_offset / 3600));
        if (g_tz_offset == 0)
            printf("  Tip: add  UTC_OFFSET -4  to routes.txt to set your timezone.\n");
    }

    printf("Loaded %d route(s) from '%s'.\n", nroutes, filename);
    printf("Controls: [E] Eastbound  [W] Westbound  [Tab/Space] Toggle  [Q] Quit\n");

    /* graceful Ctrl+C */
    signal(SIGINT, handle_sigint);

    /* raw terminal so keystrokes are read without pressing Enter */
    set_raw_mode();

    /* hide cursor */
    printf(CURSOR_HIDE);
    fflush(stdout);

    /* ── main loop ── */
    while (running) {
        /* wait up to 1 second for a keypress */
        struct timeval tv = {1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                switch (c) {
                    case 'e': case 'E':
                        g_direction = DIR_EAST;  break;
                    case 'w': case 'W':
                        g_direction = DIR_WEST;  break;
                    case '\t': case ' ':
                        g_direction = !g_direction;  break;
                    case 'q': case 'Q':
                    case  3:  /* Ctrl+C */
                        running = 0;  break;
                    default:  break;
                }
            }
        }

        if (!running) break;

        long now = (long)time(NULL);

        Departure deps[MAX_DEPARTURES];
        int ndeps = build_departures(routes, nroutes, deps, MAX_DEPARTURES,
                                     now, g_direction);
        qsort(deps, (size_t)ndeps, sizeof(Departure), cmp_departure);

        draw_board(deps, ndeps, now, nroutes);
    }

    /* restore terminal and exit cleanly */
    restore_terminal();
    printf(CURSOR_SHOW CLEAR_SCREEN);
    printf("Departures board closed.  Have a good trip!\n");

    return 0;
}

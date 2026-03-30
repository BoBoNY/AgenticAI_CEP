/*
 * Transit Terminal Departures Board
 *
 * Reads route data from a text file and displays a live terminal
 * board showing upcoming departures with real-time countdowns.
 *
 * Compile:  gcc -Wall -std=c99 -o departures departures.c
 * Run:      ./departures [routes_file] [utc_offset_hours]
 *
 *   routes_file      : path to routes data file  (default: routes.txt)
 *   utc_offset_hours : e.g. -4 for EDT, -5 for EST, +1 for CET
 *                      If omitted the program detects the system timezone.
 *
 * You can also set the TZ variable before running instead:
 *   TZ=America/Toronto ./departures
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

/* ── Constants ──────────────────────────────────────────────── */
#define MAX_ROUTES      100
#define MAX_DEPARTURES  300   /* total departure slots across all routes */
#define MAX_NAME_LEN     64
#define SHOW_ROWS        15   /* how many departures to display at once */
#define REFRESH_SECS      1   /* redraw interval */
#define LOOKAHEAD_MINS   60   /* how many minutes ahead to generate slots */

/* ── ANSI escape helpers ────────────────────────────────────── */
#define CLEAR_SCREEN   "\033[2J\033[H"
#define CURSOR_HIDE    "\033[?25l"
#define CURSOR_SHOW    "\033[?25h"
#define BOLD           "\033[1m"
#define RESET          "\033[0m"
#define FG_WHITE       "\033[97m"
#define FG_YELLOW      "\033[93m"
#define FG_GREEN       "\033[92m"
#define FG_CYAN        "\033[96m"
#define FG_RED         "\033[91m"
#define FG_GRAY        "\033[90m"
#define BG_DARK        "\033[48;5;234m"   /* near-black background */
#define BG_HEADER      "\033[48;5;19m"    /* dark-blue header strip */
#define BG_SUBHDR      "\033[48;5;17m"    /* slightly lighter strip */
#define BG_ARRIVING    "\033[48;5;22m"    /* dark-green: arriving soon */
#define BG_ROW_ALT    "\033[48;5;235m"    /* alternating row tint */

/* ── Data structures ────────────────────────────────────────── */
typedef struct {
    int  route_num;
    char stop_name[MAX_NAME_LEN];
    int  freq_mins;    /* headway in minutes */
    int  offset_secs;  /* stagger offset so routes don't all depart together */
} Route;

typedef struct {
    int  route_num;
    char stop_name[MAX_NAME_LEN];
    long depart_at;    /* absolute Unix timestamp of departure */
} Departure;

/* ── Globals ─────────────────────────────────────────────────  */
static volatile int running     = 1;
static long         g_tz_offset = 0;   /* display offset in seconds (e.g. -14400 for EDT) */

/* ── Timezone helpers ───────────────────────────────────────── */

/*
 * Detect the local UTC offset in seconds by comparing how mktime()
 * interprets a UTC broken-down time vs the original epoch value.
 * Respects the TZ environment variable (e.g. TZ=America/Toronto).
 *
 * Example: UTC-4 (EDT) returns -14400.
 */
static long detect_tz_offset(void)
{
    time_t    t  = time(NULL);
    struct tm gm = *gmtime(&t);
    /* mktime treats 'gm' as *local* time and converts to epoch.
       Subtracting from t gives the local UTC offset. */
    return (long)t - (long)mktime(&gm);
}

/*
 * Break down a Unix timestamp adjusted by g_tz_offset for display.
 * Uses gmtime() so we are not affected by the server's system timezone.
 * Writes the result into *out and returns out.
 */
static struct tm *display_tm(time_t t, struct tm *out)
{
    time_t adjusted = t + (time_t)g_tz_offset;
    *out = *gmtime(&adjusted);
    return out;
}

/* ── Signal handler ─────────────────────────────────────────── */
static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

/* ── File parsing ───────────────────────────────────────────── */
/*
 * Load routes from file.  Also scans for a UTC_OFFSET directive and
 * writes the offset in seconds to *file_tz_offset when found
 * (leaves it unchanged when the directive is absent).
 *
 * Returns number of routes loaded, or -1 on error.
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

        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* skip blank lines */
        if (*p == '\0' || *p == '\n') continue;

        /* UTC_OFFSET directive (not a comment — must be first word) */
        if (strncmp(p, "UTC_OFFSET", 10) == 0) {
            double hours = 0.0;
            if (sscanf(p + 10, "%lf", &hours) == 1)
                *file_tz_offset = (long)(hours * 3600.0);
            continue;
        }

        /* skip comment lines */
        if (*p == '#') continue;

        Route r;
        if (sscanf(p, "%d %63s %d %d",
                   &r.route_num, r.stop_name,
                   &r.freq_mins, &r.offset_secs) == 4) {
            if (r.freq_mins < 1)   r.freq_mins   = 1;
            if (r.offset_secs < 0) r.offset_secs = 0;
            routes[count++] = r;
        }
    }

    fclose(fp);
    return count;
}

/* ── Departure generation ───────────────────────────────────── */
/*
 * For each route, compute departure times within the next LOOKAHEAD_MINS
 * minutes.  Departures repeat every freq_mins.  The offset_secs staggers
 * the initial slot so routes don't all share the same schedule.
 */
static int build_departures(const Route *routes, int nroutes,
                             Departure *deps,   int max_deps,
                             long now)
{
    int  count = 0;
    long window = (long)LOOKAHEAD_MINS * 60;

    for (int i = 0; i < nroutes && count < max_deps; i++) {
        const Route *r   = &routes[i];
        long  period     = (long)r->freq_mins * 60;
        long  phase      = (now + r->offset_secs) % period;
        long  first_next = now + (period - phase);   /* first departure >= now */

        for (long t = first_next; t <= now + window && count < max_deps; t += period) {
            Departure d;
            d.route_num  = r->route_num;
            d.depart_at  = t;
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

/* ── Formatting helpers ──────────────────────────────────────── */

/* Print the countdown label for a departure that is `secs` away. */
static void print_countdown(long secs)
{
    if (secs <= 0) {
        printf("%s%-20s%s", FG_RED BOLD, "  NOW DEPARTING", RESET);
    } else if (secs < 60) {
        printf("%s%s  Due in %2ld sec%-5s%s",
               FG_YELLOW, BOLD, secs, "", RESET);
    } else {
        long mins = secs / 60;
        long rem  = secs % 60;
        char buf[48];
        if (rem == 0)
            snprintf(buf, sizeof(buf), "  Arriving in %2ld min", mins);
        else
            snprintf(buf, sizeof(buf), "  Arriving in %2ld min %02lds", mins, rem);
        if (secs < 300)
            printf("%s%s%-22s%s", FG_GREEN, BOLD, buf, RESET);  /* < 5 min */
        else
            printf("%s%-22s%s", FG_CYAN, buf, RESET);
    }
}

/* ── Board renderer ──────────────────────────────────────────── */
static void draw_board(const Departure *deps, int ndeps, long now,
                       int nroutes)
{
    struct tm tbuf;   /* reusable buffer for display_tm() */

    /* ── title bar ── */
    printf(CLEAR_SCREEN);
    printf("%s%s%s", BG_HEADER, BOLD, FG_WHITE);
    printf("  %-30s", "TRANSIT TERMINAL DEPARTURES");

    /* live clock — uses display timezone */
    display_tm((time_t)now, &tbuf);
    printf("  %s  %02d:%02d:%02d  %s\n",
           FG_YELLOW,
           tbuf.tm_hour, tbuf.tm_min, tbuf.tm_sec,
           RESET);

    /* ── column headers ── */
    printf("%s%s%s", BG_SUBHDR, BOLD, FG_WHITE);
    printf("  %-6s  %-18s  %-24s  %s\n",
           "ROUTE", "DESTINATION", "STATUS", "DEPARTS AT");
    printf("%s", RESET);

    /* ── separator ── */
    printf("%s", FG_GRAY);
    for (int i = 0; i < 70; i++) putchar('-');
    printf("%s\n", RESET);

    /* ── rows ── */
    int shown = 0;
    for (int i = 0; i < ndeps && shown < SHOW_ROWS; i++) {
        long secs_away = deps[i].depart_at - now;
        if (secs_away < -30) continue;   /* skip already-gone entries */

        /* alternating row tint for readability */
        const char *row_bg = (shown % 2 == 0) ? BG_DARK : BG_ROW_ALT;

        /* "arriving soon" highlight */
        if (secs_away >= 0 && secs_away < 120)
            row_bg = BG_ARRIVING;

        printf("%s", row_bg);

        /* route number */
        char route_buf[16];
        snprintf(route_buf, sizeof(route_buf), "%d", deps[i].route_num);
        printf("  %s%s%-6s%s  ", BOLD, FG_YELLOW, route_buf, RESET BG_DARK);
        printf("%s", row_bg);

        /* stop name */
        printf("%s%-18s%s  ", FG_WHITE, deps[i].stop_name, RESET);
        printf("%s", row_bg);

        /* countdown */
        print_countdown(secs_away);

        /* scheduled departure time — adjusted for display timezone */
        display_tm((time_t)deps[i].depart_at, &tbuf);
        printf("  %s%02d:%02d%s",
               FG_GRAY,
               tbuf.tm_hour, tbuf.tm_min,
               RESET);

        printf("%s\n", RESET);
        shown++;
    }

    /* fill empty rows so the board stays a fixed height */
    for (int i = shown; i < SHOW_ROWS; i++) {
        printf("%s  %-6s  %-18s  %-24s\n",
               (i % 2 == 0) ? BG_DARK : BG_ROW_ALT,
               "---", "---", "---");
        printf("%s", RESET);
    }

    /* ── footer ── */
    printf("%s", FG_GRAY);
    for (int i = 0; i < 70; i++) putchar('-');
    printf("%s\n", RESET);

    /* show UTC offset so the user can confirm the right timezone */
    int  off_h = (int)(g_tz_offset / 3600);
    int  off_m = (int)((g_tz_offset % 3600) / 60);
    if (off_m < 0) off_m = -off_m;
    char tz_label[32];
    snprintf(tz_label, sizeof(tz_label), "UTC%+d:%02d", off_h, off_m);

    printf("%s  %d routes  |  %s  |  Next %d deps  |  Ctrl+C to exit%s\n",
           FG_GRAY, nroutes, tz_label, SHOW_ROWS, RESET);

    fflush(stdout);
}

/* ── Entry point ─────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *filename = (argc > 1) ? argv[1] : "routes.txt";

    /*
     * Determine display timezone offset — three-level priority:
     *
     *  1. Command-line argument  (highest):  ./departures routes.txt -4
     *  2. UTC_OFFSET directive in routes file:  UTC_OFFSET -4
     *  3. System / TZ env-var detection  (lowest):  TZ=America/Toronto ./departures
     *
     * The routes file directive (option 2) is the recommended approach —
     * set it once in routes.txt and every run is correct automatically.
     */

    /* load routes (also reads UTC_OFFSET from file if present) */
    long   file_tz   = LONG_MIN;   /* sentinel: "not set" */
    Route  routes[MAX_ROUTES];
    int    nroutes   = load_routes(filename, routes, MAX_ROUTES, &file_tz);
    if (nroutes <= 0) {
        fprintf(stderr, "No routes loaded.  Exiting.\n");
        return 1;
    }

    if (argc > 2) {
        /* 1 — explicit command-line override */
        double hours = atof(argv[2]);
        g_tz_offset  = (long)(hours * 3600.0);
        printf("UTC offset (from argument): %+.1f h\n", hours);
    } else if (file_tz != LONG_MIN) {
        /* 2 — UTC_OFFSET directive found inside the routes file */
        g_tz_offset = file_tz;
        printf("UTC offset (from routes file): %+d h\n", (int)(g_tz_offset / 3600));
    } else {
        /* 3 — fall back to system / TZ env-var detection */
        g_tz_offset = detect_tz_offset();
        printf("UTC offset (system detected): %+d h\n", (int)(g_tz_offset / 3600));
        if (g_tz_offset == 0) {
            printf("  Tip: add  UTC_OFFSET -4  to routes.txt to set your timezone.\n");
        }
    }

    printf("Loaded %d route(s) from '%s'.\n", nroutes, filename);

    /* set up signal handler so Ctrl+C restores the cursor */
    signal(SIGINT, handle_sigint);

    /* hide cursor for clean display */
    printf(CURSOR_HIDE);
    fflush(stdout);

    /* ── main loop ── */
    while (running) {
        long now = (long)time(NULL);

        Departure deps[MAX_DEPARTURES];
        int ndeps = build_departures(routes, nroutes, deps, MAX_DEPARTURES, now);
        qsort(deps, (size_t)ndeps, sizeof(Departure), cmp_departure);

        draw_board(deps, ndeps, now, nroutes);

        sleep(REFRESH_SECS);
    }

    /* restore terminal state */
    printf(CURSOR_SHOW CLEAR_SCREEN);
    printf("Departures board closed.  Have a good trip!\n");

    return 0;
}

/*
 * Transit Terminal Departures Board
 *
 * Reads route data from a text file and displays a live terminal
 * board showing upcoming departures with real-time countdowns.
 *
 * Compile:  gcc -Wall -std=c99 -o departures departures.c
 * Run:      ./departures [routes_file]   (default: routes.txt)
 *
 * Compatible with Linux and Seneca Matrix (gcc).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

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
static volatile int running = 1;

/* ── Signal handler ─────────────────────────────────────────── */
static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

/* ── File parsing ───────────────────────────────────────────── */
static int load_routes(const char *filename, Route *routes, int max)
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

        /* skip blank lines and comments */
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        Route r;
        if (sscanf(p, "%d %63s %d %d",
                   &r.route_num, r.stop_name,
                   &r.freq_mins, &r.offset_secs) == 4) {
            if (r.freq_mins < 1)  r.freq_mins  = 1;
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
                       int nroutes, const char *filename)
{
    /* ── title bar ── */
    printf(CLEAR_SCREEN);
    printf("%s%s%s", BG_HEADER, BOLD, FG_WHITE);
    printf("  %-30s", "TRANSIT TERMINAL DEPARTURES");

    /* live clock */
    time_t t_now = (time_t)now;
    struct tm *tm_now = localtime(&t_now);
    printf("  %s  %02d:%02d:%02d  %s\n",
           FG_YELLOW,
           tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec,
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

        /* scheduled departure time */
        time_t dep_t = (time_t)deps[i].depart_at;
        struct tm *dep_tm = localtime(&dep_t);
        printf("  %s%02d:%02d%s",
               FG_GRAY,
               dep_tm->tm_hour, dep_tm->tm_min,
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

    printf("%s  Loaded %d routes from '%s'  |  Next %d departures shown  |  "
           "Press Ctrl+C to exit%s\n",
           FG_GRAY, nroutes, filename, SHOW_ROWS, RESET);

    fflush(stdout);
}

/* ── Entry point ─────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *filename = (argc > 1) ? argv[1] : "routes.txt";

    /* load routes */
    Route routes[MAX_ROUTES];
    int   nroutes = load_routes(filename, routes, MAX_ROUTES);
    if (nroutes <= 0) {
        fprintf(stderr, "No routes loaded.  Exiting.\n");
        return 1;
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

        draw_board(deps, ndeps, now, nroutes, filename);

        sleep(REFRESH_SECS);
    }

    /* restore terminal state */
    printf(CURSOR_SHOW CLEAR_SCREEN);
    printf("Departures board closed.  Have a good trip!\n");

    return 0;
}

/**
 * @file    serc_os_gui.c(we will change the name some day)
 * @brief   SERC Mini-OS — Full ncurses TUI Dashboard
 *
 * Smart Emergency Response Center Operating System Simulation.
 * Replaces the original console I/O with a live, multi-panel ncurses(if you want the og file it was the first draft during last meeting)
 * interface: animated scheduling, modal input dialogs, a scrollable
 * metrics popup, and a real-time kernel log.
 *
 * OS Features Implemented:
 *   1. Process Management     — PCB table, 5-state machine, aging
 *   2. CPU Scheduling         — FCFS, Round Robin (Q=3), Priority+Aging
 *   3. Memory Management      — Best-Fit allocation with coalescing
 *   4. Deadlock Handling      — Detection + OOM-Killer resolution
 *   5. File Management        — Persistent kernel log (serc_kernel.log)
 *   (IPC via shared PCB table across scheduler routines)
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -o serc_os_gui serc_os_gui.c -lncurses
 *
 * Author  : i think it was something likw LMSK
 * Course  : CS 225 — Introduction to Operating Systems
 * Inst.   : Dr Derrick Ntalasha, Copperbelt University
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <ncurses.h>
#include <locale.h>

/* ================================================================
 * SECTION 1 — CONSTANTS
 * ================================================================ */

/* ncurses color-pair IDs */
#define CP_NORMAL       1
#define CP_HEADER       2   /* inverse: black on cyan              */
#define CP_FIRE         3   /* red   — Fire/Critical tasks         */
#define CP_AMBULANCE    4   /* yellow — Ambulance/Medium tasks     */
#define CP_POLICE       5   /* cyan  — Police/Low tasks            */
#define CP_SUCCESS      6   /* green — DONE / healthy messages     */
#define CP_INFO         7   /* cyan  — informational messages      */
#define CP_DIM          8   /* white — dimmed / terminated items   */
#define CP_OS_EVENT     9   /* magenta — kernel-level events       */
#define CP_MENU_BASE    10  /* inverse: black on white             */
#define CP_MENU_ACT     11  /* inverse: white on blue              */
#define CP_MEM_OK       12  /* green  memory bar                   */
#define CP_MEM_WARN     13  /* yellow memory bar                   */
#define CP_MEM_CRIT     14  /* red    memory bar                   */
#define CP_BORDER       15  /* cyan   panel borders                */
#define CP_TITLE        16  /* bold magenta panel titles           */

/* OS tuning */
#define MAX_PROCESSES   100
#define TOTAL_MEMORY    1024    /* MB  */
#define TIME_QUANTUM    3       /* RR quantum in ticks */
#define MAX_LOG_LINES   200     /* ring-buffer depth   */
#define EXEC_DELAY_US   350000  /* 350 ms per scheduler tick (animation) */

/* Fixed layout sizes */
#define HDR_H   3
#define MENU_H  4
#define LOG_H   8

/* ================================================================
 * SECTION 2 — DATA STRUCTURES
 * ================================================================ */

typedef enum {
    STATE_NEW, STATE_READY, STATE_RUNNING, STATE_WAITING, STATE_TERMINATED
} ProcState;

/** Process Control Block */
typedef struct {
    int       pid;
    char      name[32];
    int       type;            /* 1=Fire  2=Ambulance  3=Police           */
    int       base_priority;   /* set at creation                         */
    int       curr_priority;   /* may decrease via aging                  */
    int       burst_time;      /* total CPU ticks needed                  */
    int       remaining_time;  /* ticks left                              */
    int       mem_required;    /* MB allocated                            */
    ProcState state;
    int       arrival_time;
    int       wait_cycles;     /* used by aging algorithm                 */
    int       completion_time; /* set on TERMINATED                       */
} PCB;

/** Memory linked-list node (Best-Fit allocator) */
typedef struct MemBlock {
    int            stavrt;
    int            size;
    bool           is_free;
    int            pid;
    struct MemBlock *next;
} MemBlock;

/** One line in the on-screen ring buffer */
typedef struct {
    char text[256];
    int  cp;   /* color pair */
} LogEntry;

/* ================================================================
 * SECTION 3 — GLOBAL OS STATE
 * ================================================================ */

static PCB      pcb_table[MAX_PROCESSES];
static int      proc_count   = 0;
static int      sys_clock    = 0;
static MemBlock *mem_head    = NULL;

static LogEntry log_buf[MAX_LOG_LINES];
static int      log_count    = 0;

/* ================================================================
 * SECTION 4 — NCURSES WINDOW HANDLES & COMPUTED LAYOUT
 * ================================================================ */

static WINDOW *W_hdr    = NULL;   /* top header bar         */
static WINDOW *W_stat   = NULL;   /* left status panel      */
static WINDOW *W_ptbl   = NULL;   /* right process table    */
static WINDOW *W_log    = NULL;   /* bottom-center log pane */
static WINDOW *W_menu   = NULL;   /* command menu bar       */

/* Recomputed on init and KEY_RESIZE */
static int G_rows, G_cols;
static int G_content_y;   /* row where status/ptbl start */
static int G_content_h;   /* height of status/ptbl area  */
static int G_log_y;       /* row where log panel starts  */
static int G_stat_w;      /* width of status panel (0 = hidden) */
static int G_ptbl_w;      /* width of process table panel        */

/* ================================================================
 * SECTION 5 — FORWARD DECLARATIONS
 * ================================================================ */

/* ncurses lifecycle */
static void ui_init(void);
static void ui_teardown(void);
static void ui_recalc(void);
static void ui_make_windows(void);
static void ui_free_windows(void);

/* Rendering */
static void draw_header(void);
static void draw_status(void);
static void draw_proctbl(void);
static void draw_log(void);
static void draw_menu(void);
static void draw_all(void);

/* Dialogs */
static int  dlg_int(const char *title, const char *prompt, int lo, int hi);
static void dlg_str(const char *title, const char *prompt, char *out, int cap);
static void dlg_msg(const char *title, const char *body,   int cp);
static void dlg_metrics(void);
static void dlg_file_log(void);

/* In-memory log */
static void klog(const char *msg, int cp);

/* File log */
static void flog(const char *msg);

/* Memory subsystem */
static void init_memory(void);
static bool mem_alloc(int pid, int sz);
static void mem_free(int pid);
static int  mem_available(void);

/* Helper utilities */
static int         task_cp(int type);
static const char *state_name(ProcState s);
static void        touch_all(void);

/* OS operations */
static void op_new_task(void);
static void op_suspend(void);
static void op_resume_waiting(void);
static void op_exec(int idx, int ticks);
static void op_fcfs(void);
static void op_rr(void);
static void op_priority(void);
static void op_deadlock(void);

/* ================================================================
 * SECTION 6 — ENTRY POINT
 * ================================================================ */

int main(void) {
    setlocale(LC_ALL, "");
    init_memory();
    ui_init();

    int key;
    while (1) {
        draw_all();
        key = wgetch(W_menu);   /* blocking wait for input */

        switch (key) {
            case '1':                     op_new_task();  break;
            case '2':                     op_suspend();   break;
            case '3':                     op_fcfs();      break;
            case '4':                     op_rr();        break;
            case '5':                     op_priority();  break;
            case '6':                     op_deadlock();  break;
            case '7':                     dlg_file_log(); break;
            case '0': case 'q': case 'Q':
                ui_teardown();
                flog("System halted by root.");
                puts("SERC Mini-OS — System halted.");
                return 0;
            case KEY_RESIZE:
                ui_free_windows();
                ui_recalc();
                ui_make_windows();
                break;
            default: break;
        }
    }
    return 0;
}

/* ================================================================
 * SECTION 7 — NCURSES LIFECYCLE
 * ================================================================ */

static void ui_init(void) {
    initscr();
    cbreak();
    noecho();\
    keypad(stdscr, TRUE);
    curs_set(0);

    if (!has_colors()) {
        endwin();
        fputs("Error: terminal does not support colors.\n", stderr);
        exit(EXIT_FAILURE);
    }
    start_color();
    use_default_colors();   /* allows color_pair(x, y, -1) for transparent bg */

    /* Define every color pair used in the UI */
    init_pair(CP_NORMAL,    COLOR_WHITE,   -1);
    init_pair(CP_HEADER,    COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_FIRE,      COLOR_RED,     -1);
    init_pair(CP_AMBULANCE, COLOR_YELLOW,  -1);
    init_pair(CP_POLICE,    COLOR_CYAN,    -1);
    init_pair(CP_SUCCESS,   COLOR_GREEN,   -1);
    init_pair(CP_INFO,      COLOR_CYAN,    -1);
    init_pair(CP_DIM,       COLOR_WHITE,   -1);
    init_pair(CP_OS_EVENT,  COLOR_MAGENTA, -1);
    init_pair(CP_MENU_BASE, COLOR_BLACK,   COLOR_WHITE);
    init_pair(CP_MENU_ACT,  COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_MEM_OK,    COLOR_GREEN,   -1);
    init_pair(CP_MEM_WARN,  COLOR_YELLOW,  -1);
    init_pair(CP_MEM_CRIT,  COLOR_RED,     -1);
    init_pair(CP_BORDER,    COLOR_CYAN,    -1);
    init_pair(CP_TITLE,     COLOR_MAGENTA, -1);

    getmaxyx(stdscr, G_rows, G_cols);
    ui_recalc();
    ui_make_windows();

    /* Boot log messages */
    klog("SERC Mini-OS kernel booted successfully.", CP_SUCCESS);
    klog("Best-Fit memory allocator online. 1024 MB available.", CP_INFO);
    klog("Schedulers ready: FCFS | Round Robin (Q=3) | Priority+Aging", CP_INFO);
    klog("Deadlock detector armed. Awaiting emergency dispatch...", CP_OS_EVENT);
    flog("System boot.");
}

static void ui_teardown(void) {
    ui_free_windows();
    endwin();
}

/** Recalculate all panel dimensions from the current terminal size. */
static void ui_recalc(void) {
    getmaxyx(stdscr, G_rows, G_cols);

    G_content_y = HDR_H;
    G_content_h = G_rows - HDR_H - LOG_H - MENU_H;
    if (G_content_h < 5) G_content_h = 5;

    G_log_y = G_content_y + G_content_h;

    /* Status panel is hidden on very narrow terminals */
    if      (G_cols >= 100) G_stat_w = 32;
    else if (G_cols >=  80) G_stat_w = 28;
    else                    G_stat_w = 0;

    G_ptbl_w = G_cols - G_stat_w;
}

static void ui_make_windows(void) {
    W_hdr  = newwin(HDR_H,      G_cols,   0,          0);
    W_stat = (G_stat_w > 0)
            ? newwin(G_content_h, G_stat_w,  G_content_y, 0)
            : NULL;
    W_ptbl = newwin(G_content_h, G_ptbl_w,  G_content_y, G_stat_w);
    W_log  = newwin(LOG_H,      G_cols,   G_log_y,    0);
    W_menu = newwin(MENU_H,     G_cols,   G_log_y + LOG_H, 0);
    keypad(W_menu,  TRUE);
    keypad(stdscr,  TRUE);
}

static void ui_free_windows(void) {
#define WDEL(w)  do { if (w) { delwin(w); w = NULL; } } while (0)
    WDEL(W_hdr);
    WDEL(W_stat);
    WDEL(W_ptbl);
    WDEL(W_log);
    WDEL(W_menu);
#undef WDEL
}
/*why this $$$$$$$$$$$$$*/
/* ================================================================
 * SECTION 8 — RENDERING ENGINE
 * ================================================================ */

/** Touch every window so ncurses redraws overlapping regions. */
static void touch_all(void) {
    if (W_hdr)  touchwin(W_hdr);
    if (W_stat) touchwin(W_stat);
    if (W_ptbl) touchwin(W_ptbl);
    if (W_log)  touchwin(W_log);
    if (W_menu) touchwin(W_menu);
}

/** Draw the top header bar: title, version, active-process count, clock. */
static void draw_header(void) {
    WINDOW *w = W_hdr;
    werase(w);
    wbkgd(w, COLOR_PAIR(CP_HEADER) | A_BOLD);

    /* Fill all three rows with the background colour */
    for (int r = 0; r < HDR_H; r++)
        mvwhline(w, r, 0, ' ', G_cols);

    const char *TITLE = "SERC MINI-OS KERNEL DASHBOARD";
    int tx = (G_cols - (int)strlen(TITLE)) / 2;

    wattron(w, A_BOLD);
    mvwprintw(w, 1, tx, "%s", TITLE);

    /* Left: version tag */
    mvwprintw(w, 1, 2, "[ v2.0 ]");

    /* Right: live clock + active process count */
    int active = 0;
    for (int i = 0; i < proc_count; i++)
        if (pcb_table[i].state != STATE_TERMINATED) active++;

    char info[64];
    snprintf(info, sizeof info, "CLK: %04d  PROCS: %d  ", sys_clock, active);
    mvwprintw(w, 1, G_cols - (int)strlen(info) - 1, "%s", info);
    wattroff(w, A_BOLD);

    wnoutrefresh(w);
}

/** Draw the left status panel: memory bar, per-state counts, legend. */
static void draw_status(void) {
    if (!W_stat) return;
    WINDOW *w = W_stat;
    int wh, ww;
    getmaxyx(w, wh, ww);
    (void)wh;
    werase(w);

    wattron(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(w, 0, 0);
    mvwprintw(w, 0, 2, " STATUS ");
    wattroff(w, COLOR_PAIR(CP_BORDER) | A_BOLD);

    int row   = 2;
    int bar_w = ww - 4;

    /* ---- Memory bar ---- */
    int free_m  = mem_available();
    int used_m  = TOTAL_MEMORY - free_m;
    float pct   = (float)used_m / TOTAL_MEMORY * 100.0f;
    int   filled = (int)(pct / 100.0f * bar_w);
    int   mem_cp = (pct > 85.0f) ? CP_MEM_CRIT
                : (pct > 60.0f) ? CP_MEM_WARN
                :                  CP_MEM_OK;

    wattron(w, A_BOLD | COLOR_PAIR(CP_INFO));
    mvwprintw(w, row++, 2, "MEMORY");
    wattroff(w, A_BOLD | COLOR_PAIR(CP_INFO));

    mvwaddch(w, row, 2, '[');
    for (int i = 0; i < bar_w; i++) {
        if (i < filled) {
            wattron(w, COLOR_PAIR(mem_cp) | A_BOLD);
            waddch(w, '|');
            wattroff(w, COLOR_PAIR(mem_cp) | A_BOLD);
        } else {
            wattron(w, COLOR_PAIR(CP_DIM) | A_DIM);
            waddch(w, '.');
            wattroff(w, COLOR_PAIR(CP_DIM) | A_DIM);
        }
    }
    waddch(w, ']');
    row++;
    mvwprintw(w, row++, 2, "%.0f%%  %d / %d MB", pct, used_m, TOTAL_MEMORY);
    row++;

    /* ---- Process state counts ---- */
    wattron(w, A_BOLD | COLOR_PAIR(CP_INFO));
    mvwprintw(w, row++, 2, "PROCESSES");
    wattroff(w, A_BOLD | COLOR_PAIR(CP_INFO));

    int cnt[5] = {0};
    for (int i = 0; i < proc_count; i++) {
        int s = (int)pcb_table[i].state;
        if (s >= 0 && s < 5) cnt[s]++;
    }

    static const struct { const char *lbl; int cp; int idx; } S[] = {
        { "New:        ", CP_DIM,       STATE_NEW        },
        { "Ready:      ", CP_SUCCESS,   STATE_READY      },
        { "Running:    ", CP_OS_EVENT,  STATE_RUNNING    },
        { "Waiting:    ", CP_AMBULANCE, STATE_WAITING    },
        { "Terminated: ", CP_DIM,       STATE_TERMINATED },
    };

    for (int s = 0; s < 5; s++) {
        attr_t a = (s == 4) ? A_DIM : 0;
        wattron(w, COLOR_PAIR(S[s].cp) | a);
        mvwprintw(w, row++, 2, "%s%d", S[s].lbl, cnt[S[s].idx]);
        wattroff(w, COLOR_PAIR(S[s].cp) | A_DIM);
    }
    row++;

    /* ---- Legend ---- */
    wattron(w, A_BOLD | COLOR_PAIR(CP_INFO));
    mvwprintw(w, row++, 2, "LEGEND");
    wattroff(w, A_BOLD | COLOR_PAIR(CP_INFO));

    wattron(w, COLOR_PAIR(CP_FIRE)      | A_BOLD);
    mvwprintw(w, row++, 2, "* Fire Alert");
    wattroff(w, COLOR_PAIR(CP_FIRE)     | A_BOLD);

    wattron(w, COLOR_PAIR(CP_AMBULANCE) | A_BOLD);
    mvwprintw(w, row++, 2, "* Ambulance");
    wattroff(w, COLOR_PAIR(CP_AMBULANCE)| A_BOLD);

    wattron(w, COLOR_PAIR(CP_POLICE)    | A_BOLD);
    mvwprintw(w, row++, 2, "* Police");
    wattroff(w, COLOR_PAIR(CP_POLICE)   | A_BOLD);

    wnoutrefresh(w);
}

/** Draw the process table with coloured rows and ASCII progress bars. */
static void draw_proctbl(void) {
    WINDOW *w = W_ptbl;
    int wh, ww;
    getmaxyx(w, wh, ww);
    werase(w);

    wattron(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(w, 0, 0);
    mvwprintw(w, 0, 2, " PROCESS TABLE ");
    wattroff(w, COLOR_PAIR(CP_BORDER) | A_BOLD);

    /* Column header */
    int row = 1;
    wattron(w, A_BOLD | A_UNDERLINE);
    mvwprintw(w, row++, 1, " %-4s %-20s %-3s %-5s %-5s %-5s %-11s %s",
            "PID", "TASK NAME", "PRI", "MEM", "BRST", "REM", "STATE", "PROGRESS");
    wattroff(w, A_BOLD | A_UNDERLINE);

    int max_vis = wh - 3;
    int shown   = 0;

    for (int i = 0; i < proc_count && shown < max_vis; i++) {
        PCB *p = &pcb_table[i];
        if (p->state == STATE_TERMINATED) continue;
        shown++;

        int cp = task_cp(p->type);

        /* 14-char ASCII progress bar */
        float frac = (p->burst_time > 0)
                   ? (1.0f - (float)p->remaining_time / p->burst_time) * 14.0f
                    : 14.0f;
        int  filled_b = (int)frac;
        char bar[16];
        for (int b = 0; b < 14; b++) bar[b] = (b < filled_b) ? '=' : '-';
        bar[14] = '\0';

        attr_t bold_flag = (p->state == STATE_RUNNING) ? A_BOLD : 0;
        wattron(w, COLOR_PAIR(cp) | bold_flag);
        mvwprintw(w, row++, 1, " %-4d %-20.20s %-3d %-5d %-5d %-5d %-11s [%s]",
                    p->pid, p->name, p->curr_priority,
                    p->mem_required, p->burst_time, p->remaining_time,
                    state_name(p->state), bar);
        wattroff(w, COLOR_PAIR(cp) | A_BOLD);
        (void)ww;
    }

    if (shown == 0) {
        wattron(w, COLOR_PAIR(CP_DIM) | A_DIM);
        mvwprintw(w, row, 3, "No active processes in queue.");
        wattroff(w, COLOR_PAIR(CP_DIM) | A_DIM);
    }

    wnoutrefresh(w);
}

/** Draw the kernel log panel (newest lines at the bottom). */
static void draw_log(void) {
    WINDOW *w = W_log;
    int wh, ww;
    getmaxyx(w, wh, ww);
    werase(w);

    wattron(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(w, 0, 0);
    mvwprintw(w, 0, 2, " KERNEL LOG ");
    wattroff(w, COLOR_PAIR(CP_BORDER) | A_BOLD);

    int vis   = wh - 2;
    int start = log_count - vis;
    if (start < 0) start = 0;

    for (int i = 0; i < vis && (start + i) < log_count; i++) {
        LogEntry *e = &log_buf[start + i];
        wattron(w, COLOR_PAIR(e->cp));
        mvwprintw(w, i + 1, 1, "%-*.*s", ww - 2, ww - 2, e->text);
        wattroff(w, COLOR_PAIR(e->cp));
    }

    wnoutrefresh(w);
}

/** Draw the bottom command menu bar that dash below. */
static void draw_menu(void) {
    WINDOW *w = W_menu;
    int mh, mw;
    getmaxyx(w, mh, mw);
    werase(w);
    wbkgd(w, COLOR_PAIR(CP_MENU_BASE));
    for (int r = 0; r < mh; r++) mvwhline(w, r, 0, ' ', mw);

    wattron(w, A_BOLD | COLOR_PAIR(CP_MENU_BASE));
    mvwprintw(w, 0, 2, "OS COMMAND INTERFACE");
    wattroff(w, A_BOLD | COLOR_PAIR(CP_MENU_BASE));

    /* Row 1: creation + schedulers */
    mvwprintw(w, 1, 2,
        "[1] New Task   [2] Suspend   [3] FCFS   [4] Round Robin   [5] Priority");

    /* Row 2: diagnostics + I/O */
    mvwprintw(w, 2, 2,
        "[6] Deadlock   [7] View Log  [0] Shutdown");

    wnoutrefresh(w);
}

/** Composite refresh: draw all panels, then flush to terminal once. */
static void draw_all(void) {
    draw_header();
    draw_status();
    draw_proctbl();
    draw_log();
    draw_menu();
    doupdate();
}

/* ================================================================
 * SECTION 9 — DIALOG HELPERS
 * ================================================================ */

/**
 * Show a centred popup window, collect an integer in [lo, hi].
 * Safely restores noecho + hidden cursor before returning.
 */
static int dlg_int(const char *title, const char *prompt, int lo, int hi) {
    int dh = 9, dw = 56;
    int dy = (G_rows - dh) / 2;
    int dx = (G_cols - dw) / 2;

    WINDOW *d = newwin(dh, dw, dy, dx);
    wattron(d, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(d, 0, 0);
    mvwprintw(d, 0, 2, " %s ", title);
    wattroff(d, COLOR_PAIR(CP_BORDER) | A_BOLD);

    wattron(d, COLOR_PAIR(CP_INFO));
    mvwprintw(d, 2, 3, "%s", prompt);
    wattroff(d, COLOR_PAIR(CP_INFO));

    wattron(d, COLOR_PAIR(CP_DIM) | A_DIM);
    mvwprintw(d, 4, 3, "Valid range:  %d  to  %d", lo, hi);
    wattroff(d, COLOR_PAIR(CP_DIM) | A_DIM);

    mvwprintw(d, 6, 3, "Enter: ");
    wrefresh(d);

    echo();
    curs_set(1);
    wmove(d, 6, 10);

    char buf[24] = {0};
    wgetnstr(d, buf, (int)sizeof(buf) - 1);

    noecho();
    curs_set(0);
    delwin(d);
    touch_all();

    int v = atoi(buf);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

/**
 * Show a centred popup window and collect a string (max cap-1 chars).
 */
static void dlg_str(const char *title, const char *prompt,
                    char *out, int cap) {
    int dh = 7, dw = 58;
    int dy = (G_rows - dh) / 2;
    int dx = (G_cols - dw) / 2;

    WINDOW *d = newwin(dh, dw, dy, dx);
    wattron(d, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(d, 0, 0);
    mvwprintw(d, 0, 2, " %s ", title);
    wattroff(d, COLOR_PAIR(CP_BORDER) | A_BOLD);

    wattron(d, COLOR_PAIR(CP_INFO));
    mvwprintw(d, 2, 3, "%s", prompt);
    wattroff(d, COLOR_PAIR(CP_INFO));

    mvwprintw(d, 4, 3, "Enter: ");
    wrefresh(d);

    echo();
    curs_set(1);
    wmove(d, 4, 10);
    wgetnstr(d, out, cap - 1);
    out[cap - 1] = '\0';

    noecho();
    curs_set(0);
    delwin(d);
    touch_all();
}

/**
 * Show a centred info/error popup with a single body line.
 * Waits for any key (patience pays:)).
 */
static void dlg_msg(const char *title, const char *body, int cp) {
    int dw  = (int)strlen(body) + 8;
    if (dw < 40) dw = 40;
    int dh  = 7;
    int dy  = (G_rows - dh) / 2;
    int dx  = (G_cols - dw) / 2;
    if (dx < 0) { dx = 0; dw = G_cols; }

    WINDOW *d = newwin(dh, dw, dy, dx);
    wattron(d, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(d, 0, 0);
    mvwprintw(d, 0, 2, " %s ", title);
    wattroff(d, COLOR_PAIR(CP_BORDER) | A_BOLD);

    wattron(d, COLOR_PAIR(cp));
    mvwprintw(d, 3, 3, "%s", body);
    wattroff(d, COLOR_PAIR(cp));

    wattron(d, COLOR_PAIR(CP_DIM) | A_DIM);
    mvwprintw(d, dh - 2, 3, "Press any key to continue...");
    wattroff(d, COLOR_PAIR(CP_DIM) | A_DIM);

    nodelay(d, FALSE);
    wrefresh(d);
    wgetch(d);
    delwin(d);
    touch_all();
}

/** Popup showing a table of scheduling metrics for terminated processes. */
static void dlg_metrics(void) {
    /* Count terminated processes */
    int n = 0;
    for (int i = 0; i < proc_count; i++)
        if (pcb_table[i].state == STATE_TERMINATED && pcb_table[i].completion_time > 0)
            n++;

    if (n == 0) {
        dlg_msg("METRICS", "No terminated processes to report.", CP_DIM);
        return;
    }

    int dw = 64, dh = n + 9;
    if (dh > G_rows - 2) dh = G_rows - 2;
    int dy = (G_rows - dh) / 2;
    int dx = (G_cols - dw) / 2;
    if (dx < 0) { dx = 0; dw = G_cols; }

    WINDOW *d = newwin(dh, dw, dy, dx);
    wattron(d, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(d, 0, 0);
    mvwprintw(d, 0, 2, " SCHEDULING METRICS ");
    wattroff(d, COLOR_PAIR(CP_BORDER) | A_BOLD);

    int row = 1;
    wattron(d, A_BOLD | A_UNDERLINE);
    mvwprintw(d, row++, 2, "%-4s %-20s %-8s %-8s %-12s",
                "PID", "TASK", "Burst", "Wait", "Turnaround");
    wattroff(d, A_BOLD | A_UNDERLINE);

    float tot_wt = 0, tot_tat = 0;
    int   counted = 0;

    for (int i = 0; i < proc_count && row < dh - 5; i++) {
        PCB *p = &pcb_table[i];
        if (p->state != STATE_TERMINATED || p->completion_time == 0) continue;

        int tat = p->completion_time - p->arrival_time;
        int wt  = tat - p->burst_time;
        if (wt < 0) wt = 0;

        tot_tat += tat;
        tot_wt  += wt;
        counted++;

        wattron(d, COLOR_PAIR(task_cp(p->type)));
        mvwprintw(d, row++, 2, "%-4d %-20.20s %-8d %-8d %-12d",
                    p->pid, p->name, p->burst_time, wt, tat);
        wattroff(d, COLOR_PAIR(task_cp(p->type)));
    }

    row++;
    wattron(d, A_BOLD | COLOR_PAIR(CP_INFO));
    mvwprintw(d, dh - 5, 2, "Avg Wait Time:        %.2f ticks", counted ? tot_wt / counted : 0.0f);
    mvwprintw(d, dh - 4, 2, "Avg Turnaround Time:  %.2f ticks", counted ? tot_tat / counted : 0.0f);
    mvwprintw(d, dh - 3, 2, "System Clock:         %d ticks", sys_clock);
    wattroff(d, A_BOLD | COLOR_PAIR(CP_INFO));

    wattron(d, COLOR_PAIR(CP_DIM) | A_DIM);
    mvwprintw(d, dh - 2, 2, "Press any key to close...");
    wattroff(d, COLOR_PAIR(CP_DIM) | A_DIM);

    nodelay(d, FALSE);
    wrefresh(d);
    wgetch(d);
    delwin(d);
    touch_all();
}

/** Full-screen scrollable view of the persistent kernel log file. */
static void dlg_file_log(void) {
    FILE *f = fopen("serc_kernel.log", "r");
    if (!f) {
        dlg_msg("LOG FILE", "serc_kernel.log not found.", CP_FIRE);
        return;
    }

    /* Read up to 500 lines from disk */
    static char lines[500][256];
    int n = 0;
    while (n < 500 && fgets(lines[n], 256, f)) {
        char *nl = strchr(lines[n], '\n');
        if (nl) *nl = '\0';
        n++;
    }
    fclose(f);

    int dh = G_rows - 2;
    int dw = G_cols - 4;
    WINDOW *d = newwin(dh, dw, 1, 2);
    keypad(d, TRUE);

    wattron(d, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(d, 0, 0);
    mvwprintw(d, 0, 2, " SERC KERNEL LOG — %d entries ", n);
    wattroff(d, COLOR_PAIR(CP_BORDER) | A_BOLD);

    int vis    = dh - 4;
    int scroll = (n > vis) ? n - vis : 0;

    while (1) {
        /* Redraw visible content area */
        for (int i = 0; i < vis; i++) {
            mvwhline(d, i + 1, 1, ' ', dw - 2);
            int li = scroll + i;
            if (li < n) {
                wattron(d, COLOR_PAIR(CP_INFO));
                mvwprintw(d, i + 1, 1, "%-*.*s", dw - 2, dw - 2, lines[li]);
                wattroff(d, COLOR_PAIR(CP_INFO));
            }
        }
        mvwprintw(d, dh - 2, 2,
                    "UP/DOWN/PgUp/PgDn: scroll  |  Q: close  |  Line %d / %d",
                    scroll + 1, n);
        wrefresh(d);

        int key = wgetch(d);
        if (key == 'q' || key == 'Q' || key == 27) break;
        if ((key == KEY_DOWN || key == 'j')  && scroll < n - vis) scroll++;
        if ((key == KEY_UP   || key == 'k')  && scroll > 0)       scroll--;
        if (key == KEY_NPAGE)
            scroll = (scroll + vis < n - vis) ? scroll + vis : (n > vis ? n - vis : 0);
        if (key == KEY_PPAGE)
            scroll = (scroll - vis > 0) ? scroll - vis : 0;
    }

    delwin(d);
    touch_all();
}

/* ================================================================
 * SECTION 10 — KERNEL LOG (IN-MEMORY RING BUFFER + FILE)
 * ================================================================ */

/**
 * Append a message to the on-screen ring buffer AND the disk log.
 * If the ring is full, the oldest entry is silently overwritten.
 */
static void klog(const char *msg, int cp) {
    if (log_count >= MAX_LOG_LINES) {
        memmove(&log_buf[0], &log_buf[1],
                (MAX_LOG_LINES - 1) * sizeof(LogEntry));
        log_count = MAX_LOG_LINES - 1;
    }
    snprintf(log_buf[log_count].text, sizeof(log_buf[0].text),
                "[%04d] %s", sys_clock, msg);
    log_buf[log_count].cp = cp;
    log_count++;
    flog(msg);
}

/** Append a timestamped record to serc_kernel.log on disk. */
static void flog(const char *msg) {
    FILE *f = fopen("serc_kernel.log", "a");
    if (!f) return;
    time_t t  = time(NULL);
    char  *ts = ctime(&t);
    ts[strlen(ts) - 1] = '\0';
    fprintf(f, "[%s][CLK:%04d] %s\n", ts, sys_clock, msg);
    fclose(f);
}

/* ================================================================
 * SECTION 11 — MEMORY MANAGEMENT  (Best-Fit + Coalescing)
 * ================================================================ */

static void init_memory(void) {
    mem_head = (MemBlock *)malloc(sizeof(MemBlock));
    if (!mem_head) { perror("malloc"); exit(EXIT_FAILURE); }
    mem_head->start   = 0;
    mem_head->size    = TOTAL_MEMORY;
    mem_head->is_free = true;
    mem_head->pid     = -1;
    mem_head->next    = NULL;
}

/** Returns total free MB currently in the memory list. */
/* the total memory that is remaining */
static int mem_available(void) {
    int total = 0;
    for (MemBlock *b = mem_head; b; b = b->next)
        if (b->is_free) total += b->size;
    return total;
}

/**
 * Best-Fit allocation: scan the entire list, pick the smallest block
 * that still fits `sz` MB, split the remainder if needed.
 */
static bool mem_alloc(int pid, int sz) {
    MemBlock *best = NULL;
    for (MemBlock *b = mem_head; b; b = b->next)
        if (b->is_free && b->size >= sz)
            if (!best || b->size < best->size) best = b;

    if (!best) return false;

    if (best->size > sz) {
        /* Split: carve off a new free remainder block */
        MemBlock *rem = (MemBlock *)malloc(sizeof(MemBlock));
        if (!rem) return false;
        rem->start   = best->start + sz;
        rem->size    = best->size - sz;
        rem->is_free = true;
        rem->pid     = -1;
        rem->next    = best->next;
        best->size   = sz;
        best->next   = rem;
    }
    best->is_free = false;
    best->pid     = pid;
    return true;
}

/**
 * Free all blocks owned by `pid`, then merge adjacent free blocks
 * to prevent external fragmentation.
 */
static void mem_free(int pid) {
    for (MemBlock *b = mem_head; b; b = b->next)
        if (!b->is_free && b->pid == pid) { b->is_free = true; b->pid = -1; }

    /* Coalesce */
    for (MemBlock *b = mem_head; b && b->next; ) {
        if (b->is_free && b->next->is_free) {
            MemBlock *tmp = b->next;
            b->size += tmp->size;
            b->next  = tmp->next;
            free(tmp);
        } else {
            b = b->next;
        }
    }
}

/* ================================================================
 * SECTION 12 — HELPER UTILITIES
 * ================================================================ */

static int task_cp(int type) {
    if (type == 1) return CP_FIRE;
    if (type == 2) return CP_AMBULANCE;
    return CP_POLICE;
}

static const char *state_name(ProcState s) {
    switch (s) {
        case STATE_NEW:        return "NEW";
        case STATE_READY:      return "READY";
        case STATE_RUNNING:    return "RUNNING";
        case STATE_WAITING:    return "WAITING";
        case STATE_TERMINATED: return "TERMINATED";
    }
    return "UNKNOWN";
}

/* ================================================================
 * SECTION 13 — OS OPERATIONS
 * ================================================================ */

/** Create a new task via interactive dialogs, allocate memory, enqueue. */
static void op_new_task(void) {
    if (proc_count >= MAX_PROCESSES) {
        dlg_msg("ERROR", "Process table is full (100 processes).", CP_FIRE);
        return;
    }

    PCB *p = &pcb_table[proc_count];
    memset(p, 0, sizeof(PCB));

    /* Collect fields */
    p->type = dlg_int("NEW TASK — TYPE",
                        "1 = Fire Alert    2 = Ambulance    3 = Police",
                        1, 3);

    dlg_str("NEW TASK — NAME",
            "Enter task name  (max 20 characters):",
            p->name, 21);
    if (strlen(p->name) == 0) strcpy(p->name, "EmergencyTask");

    p->burst_time     = dlg_int("NEW TASK — BURST",
                                "CPU burst time  (ticks, 1-99):", 1, 99);
    p->remaining_time = p->burst_time;
    p->mem_required   = dlg_int("NEW TASK — MEMORY",
                                "Memory required  (MB, 1-512):", 1, 512);

    p->pid            = proc_count + 1;
    p->base_priority  = p->type;
    p->curr_priority  = p->base_priority;
    p->arrival_time   = sys_clock;

    /* Attempt memory allocation */
    char msg[256];
    if (mem_alloc(p->pid, p->mem_required)) {
        p->state = STATE_READY;
        snprintf(msg, sizeof msg,
                "Task '%s' (PID %d, %s) -> READY.  %d MB allocated via Best-Fit.",
                p->name, p->pid,
                (p->type==1)?"Fire":(p->type==2)?"Ambulance":"Police",
                p->mem_required);
        klog(msg, CP_SUCCESS);
    } else {
        p->state = STATE_WAITING;
        snprintf(msg, sizeof msg,
                "Task '%s' (PID %d) -> WAITING.  Insufficient RAM (%d MB needed).",
                p->name, p->pid, p->mem_required);
        klog(msg, CP_AMBULANCE);
    }

    proc_count++;
}

/** Swap a READY process out to WAITING and reclaim its memory. */
static void op_suspend(void) {
    int pid = dlg_int("SUSPEND TASK", "Enter PID to suspend:", 1, MAX_PROCESSES);

    for (int i = 0; i < proc_count; i++) {
        if (pcb_table[i].pid == pid && pcb_table[i].state == STATE_READY) {
            pcb_table[i].state = STATE_WAITING;
            mem_free(pid);
            char msg[80];
            snprintf(msg, sizeof msg,
                    "PID %d swapped out -> WAITING.  RAM reclaimed.", pid);
            klog(msg, CP_INFO);
            return;
        }
    }
    dlg_msg("SUSPEND", "PID not found or not in READY state.", CP_FIRE);
}

/** After a task terminates, attempt to wake up any WAITING tasks. */
static void op_resume_waiting(void) {
    for (int i = 0; i < proc_count; i++) {
        if (pcb_table[i].state == STATE_WAITING &&
            mem_alloc(pcb_table[i].pid, pcb_table[i].mem_required)) {
            pcb_table[i].state       = STATE_READY;
            pcb_table[i].wait_cycles = 0;
            char msg[80];
            snprintf(msg, sizeof msg,
                    "PID %d auto-resumed from WAITING (memory now available).",
                    pcb_table[i].pid);
            klog(msg, CP_INFO);
        }
    }
}

/**
 * Execute process at `idx` for `ticks` CPU ticks.
 * Sets it RUNNING, sleeps for the animation delay, updates the clock,
 * then marks it TERMINATED (+ frees memory) or READY (preempted).
 */
static void op_exec(int idx, int ticks) {
    PCB *p    = &pcb_table[idx];
    p->state  = STATE_RUNNING;

    char msg[256];
    snprintf(msg, sizeof msg,
            "[CPU] Dispatching '%s' (PID %d) for %d tick(s).",
            p->name, p->pid, ticks);
    klog(msg, CP_OS_EVENT);

    draw_all();              /* show RUNNING state visually      */
    usleep(EXEC_DELAY_US);  /* animation delay                  */

    sys_clock         += ticks;
    p->remaining_time -= ticks;

    if (p->remaining_time <= 0) {
        p->state           = STATE_TERMINATED;
        p->completion_time = sys_clock;
        mem_free(p->pid);

        int tat = p->completion_time - p->arrival_time;
        int wt  = tat - p->burst_time;
        if (wt < 0) wt = 0;

        snprintf(msg, sizeof msg,
                "[DONE] PID %d '%s' terminated.  WT=%d  TAT=%d  RAM freed.",
                p->pid, p->name, wt, tat);
        klog(msg, CP_SUCCESS);
        op_resume_waiting();
    } else {
        p->state = STATE_READY;
        snprintf(msg, sizeof msg,
                    "[PREEMPT] PID %d preempted.  Remaining: %d ticks.",
                    p->pid, p->remaining_time);
        klog(msg, CP_AMBULANCE);
    }
}

/* ---- FCFS Scheduler ------------------------------------------------- */
/**
 * First Come First Served: execute each READY task to completion in
 * the order it was created (== arrival order since we append to pcb_table).
 * Non-preemptive.
 */
static void op_fcfs(void) {
    klog("=== FCFS SCHEDULER STARTED ===", CP_INFO);
    flog("FCFS scheduler triggered.");

    bool any = false;
    for (int i = 0; i < proc_count; i++) {
        if (pcb_table[i].state == STATE_READY) {
            any = true;
            op_exec(i, pcb_table[i].remaining_time);
            draw_all();
        }
    }

    if (!any) klog("FCFS idle: no READY tasks in queue.", CP_DIM);
    else {
        klog("=== FCFS COMPLETE ===", CP_SUCCESS);
        draw_all();
        dlg_metrics();
    }
}

/* ---- Round Robin Scheduler ------------------------------------------ */
/**
 * Preemptive Round Robin with time quantum Q=3.
 * Loops until no READY tasks remain.
 */
static void op_rr(void) {
    klog("=== ROUND ROBIN SCHEDULER STARTED (Q=3) ===", CP_INFO);
    flog("Round Robin scheduler triggered.");

    bool any = false;
    bool done;
    do {
        done = true;
        for (int i = 0; i < proc_count; i++) {
            if (pcb_table[i].state == STATE_READY) {
                done = false;
                any  = true;
                int t = (pcb_table[i].remaining_time > TIME_QUANTUM)
                        ? TIME_QUANTUM
                        : pcb_table[i].remaining_time;
                op_exec(i, t);
                draw_all();
            }
        }
    } while (!done);

    if (!any) klog("Round Robin idle: no READY tasks.", CP_DIM);
    else {
        klog("=== ROUND ROBIN COMPLETE ===", CP_SUCCESS);
        draw_all();
        dlg_metrics();
    }
}

/* ---- Priority + Aging Scheduler ------------------------------------- */
/**
 * Non-preemptive Priority Scheduling with Aging.
 * Lower numeric priority value = higher urgency (Fire=1, Ambulance=2, Police=3).
 * Aging: every 3 idle cycles, a READY task's priority is decremented by 1
 * (elevated) to prevent starvation.
 */
static void op_priority(void) {
    klog("=== PRIORITY SCHEDULER (AGING) STARTED ===", CP_INFO);
    flog("Priority+Aging scheduler triggered.");

    bool any   = false;
    bool found = true;

    while (found) {
        found = false;

        /* Apply aging pass: increment wait_cycles, elevate if threshold hit */
        for (int i = 0; i < proc_count; i++) {
            if (pcb_table[i].state == STATE_READY) {
                pcb_table[i].wait_cycles++;
                if (pcb_table[i].wait_cycles > 3 && pcb_table[i].curr_priority > 1) {
                    pcb_table[i].curr_priority--;
                    pcb_table[i].wait_cycles = 0;
                    char msg[100];
                    snprintf(msg, sizeof msg,
                                "Aging: PID %d elevated to priority %d.",
                                pcb_table[i].pid, pcb_table[i].curr_priority);
                    klog(msg, CP_AMBULANCE);
                }
            }
        }

        /* Select the READY task with the lowest (most urgent) priority value */
        int best_idx = -1, best_pri = 9999;
        for (int i = 0; i < proc_count; i++) {
            if (pcb_table[i].state == STATE_READY &&
                pcb_table[i].curr_priority < best_pri) {
                best_pri = pcb_table[i].curr_priority;
                best_idx = i;
            }
        }

        if (best_idx != -1) {
            found = true;
            any   = true;
            /* Run the chosen task to completion (non-preemptive) */
            op_exec(best_idx, pcb_table[best_idx].remaining_time);
            draw_all();
        }
    }

    if (!any) klog("Priority idle: no READY tasks.", CP_DIM);
    else {
        klog("=== PRIORITY SCHEDULER COMPLETE ===", CP_SUCCESS);
        draw_all();
        dlg_metrics();
    }
}

/* ---- Deadlock Detection + OOM Killer -------------------------------- */
/**
 * Deadlock detection heuristic:
 *   If there are WAITING tasks AND free memory is critically low (<100 MB),
 *   the system is likely in a resource deadlock.
 *
 * Resolution: OOM (Out-of-Memory) Killer terminates the lowest-priority
 * READY/WAITING task to free memory and break the circular wait.
 */
static void op_deadlock(void) {
    klog("Running Banker's deadlock detector...", CP_OS_EVENT);

    int  waiting = 0;
    int  victim_pid = -1, victim_pri = -1;

    for (int i = 0; i < proc_count; i++) {
        if (pcb_table[i].state == STATE_WAITING) waiting++;
        /* Pick the task with the highest priority number (= lowest urgency) */
        if ((pcb_table[i].state == STATE_READY ||
                pcb_table[i].state == STATE_WAITING) &&
                pcb_table[i].curr_priority > victim_pri) {
            victim_pri = pcb_table[i].curr_priority;
            victim_pid = pcb_table[i].pid;
        }
    }

    if (waiting > 0 && mem_available() < 100) {
        klog("DEADLOCK DETECTED: memory exhausted + tasks blocked.", CP_FIRE);

        if (victim_pid != -1) {
            for (int i = 0; i < proc_count; i++) {
                if (pcb_table[i].pid == victim_pid) {
                    pcb_table[i].state = STATE_TERMINATED;
                    mem_free(victim_pid);

                    char msg[120];
                    snprintf(msg, sizeof msg,
                                "OOM Killer: PID %d terminated.  Deadlock resolved.",
                                victim_pid);
                    klog(msg, CP_SUCCESS);
                    op_resume_waiting();
                    dlg_msg("DEADLOCK RESOLVED",
                            "OOM Killer terminated lowest-priority task.", CP_SUCCESS);
                    return;
                }
            }
        }
    } else {
        klog("No deadlock detected.  System resources are healthy.", CP_SUCCESS);
        dlg_msg("DEADLOCK ANALYSIS",
                "No deadlock detected.  System is healthy.", CP_SUCCESS);
    }
}
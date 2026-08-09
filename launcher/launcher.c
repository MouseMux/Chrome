/* MouseMux Chrome launcher
 *
 * Chrome will not start a second browser process against a profile that is
 * already in use: it forwards the command line to the running instance and
 * exits.  No new process means PostBrowserStart never runs, which means no
 * second MouseMux dialog.  Each seat therefore needs its own --user-data-dir.
 *
 * Seats are tracked with a numbered named mutex per seat, created and held by
 * the launcher, which stays alive for as long as its browser does.  Seat N is
 * running exactly while a launcher holds its mutex - no probing, no timers,
 * no state on disk to go stale.  A launcher that dies for any reason releases
 * the seat, and the kernel does that even on a hard kill.
 *
 * Two earlier schemes are worth not repeating.  Binding the control port to
 * test a seat answered the wrong question: a port can be squatted, sits in
 * TIME_WAIT, and says nothing about whether the browser came up.  Having
 * Chrome claim the mutex itself worked, but coupled launcher and browser
 * versions and left the launcher guessing when a launch had failed.
 *
 * Waiting on the child is only exact because we pass --do-not-de-elevate.
 * Without it an elevated launch makes Chrome relaunch itself and let the
 * original process go, so the process we spawned would exit within a second
 * while the browser it left behind kept running - and the seat would be
 * released with a live browser still on it.
 *
 * Runs chrome.exe from its own folder.  Links only system DLLs and the static
 * CRT, so it needs neither the Chromium build nor the VC redistributable.
 *
 * Options:
 *   launcher              launch the next free seat
 *   launcher -s N         launch seat N specifically
 *   launcher -n N         launch N seats
 *   launcher -p PORT      base port (default 52000)
 *   launcher -q           quiet, no output
 *   launcher -h           show help
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Chrome's own control ports, deliberately in a different range from the
 * MouseMux server's port (41001).  They are unrelated systems — Chrome dials
 * out to 41001, and listens on one of these per seat — and adjacent numbers
 * made them look like a single shared range.  The port is just a per-seat
 * setting we pass along; it plays no part in tracking seats. */
#define DEFAULT_BASE_PORT 52000
#define MAX_SEATS         64

/* hold_seats() waits on one handle per seat in a single call, so the seat
 * count cannot exceed what WaitForMultipleObjects accepts.  Raising MAX_SEATS
 * past this would make that call fail outright, and the failure path releases
 * every held seat - seats would appear to free themselves at random.  Fixing
 * it means waiting in batches, not bumping the number. */
_STATIC_ASSERT(MAX_SEATS <= MAXIMUM_WAIT_OBJECTS);

/* A browser that is going to serve a seat does not exit this fast.  One that
 * does has handed its command line to an instance already holding the
 * profile, so the seat is taken and the next one is worth trying. */
#define STARTUP_PROBE_MS  3000

/* The control server binds shortly after startup, so its port is checked
 * separately and only to warn.  Short, because by this point the browser is
 * already known to be up. */
#define PORT_GRACE_MS     5000
#define POLL_INTERVAL_MS  250

/* How many spawned processes may fail to hold a seat before the whole run is
 * abandoned.  Skipping a seat whose mutex is already held costs nothing and
 * is not counted; this budget covers only launches that started a process and
 * got nothing back.  A handful of those is not bad luck, it is a stale
 * profile or a broken install, and scanning on would leave one unmanageable
 * browser window per remaining seat. */
#define MAX_SPAWN_MISSES  3

/* Session-local, so seats do not collide across logon sessions, and no
 * privilege is needed to create them.  The name carries a hash of the folder
 * holding chrome.exe: two copies of the release folder are two independent
 * sets of seats, matching how user-data-N is already per-folder. */
#define SEAT_LOCK_FORMAT "Local\\MouseMuxChromeSeat_%08x_%d"
#define LOCK_NAME_MAX    128

/* Browser windows are cascaded so seats stay visually distinct.  Chrome
 * otherwise restores each profile's saved placement, which puts every seat in
 * the same spot on a first run. */
#define WINDOW_WIDTH      1200
#define WINDOW_HEIGHT     800
#define CASCADE_ORIGIN    40
#define CASCADE_STEP      60

/* Why a seat did not start.  The distinction drives control flow: OCCUPIED
 * means "this seat is spoken for, try the next", FAILED means "stop asking".
 * Collapsing the two turns one broken launch into a sweep of all 64 seats. */
typedef enum {
    SEAT_STARTED,   /* browser is up; we hold its mutex and its handle */
    SEAT_OCCUPIED,  /* something already owns this seat - move on */
    SEAT_FAILED     /* the launch itself is broken - do not try more seats */
} seat_result;

/* A seat this launcher is holding open. */
typedef struct {
    int seat;
    HANDLE lock;     /* released when the browser exits, freeing the seat */
    HANDLE process;
} seat_slot;

static int g_quiet = 0;
static int g_base_port = DEFAULT_BASE_PORT;
static int g_keep_placement = 0;
static int g_console = 0;
static int g_spawn_misses = 0;
static unsigned g_install_hash = 0;
static int g_winsock = 0;
/* Set by fail().  Suppresses the end-of-run summary when a specific error has
 * already been shown, so a double-clicked launcher never stacks message boxes. */
static int g_error_reported = 0;

/* Built as a GUI subsystem app so double-clicking does not pop up a console
 * window.  When launched FROM a command prompt we adopt that console instead,
 * so scripted use still prints normally.  Double-clicked there is no parent
 * console and progress output simply goes nowhere - which is the point. */
static void console_attach(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream = NULL;
        if (freopen_s(&stream, "CONOUT$", "w", stdout) == 0) {
            g_console = 1;
        }
    }
}

static void say(const char *fmt, ...)
{
    va_list args;

    if (g_quiet || !g_console) {
        return;
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

/* Errors the user must see.  Falls back to a message box when there is no
 * console, so a double-clicked launcher cannot fail silently. */
static void fail(const char *fmt, ...)
{
    char msg[512];
    va_list args;

    va_start(args, fmt);
    _vsnprintf_s(msg, sizeof msg, _TRUNCATE, fmt, args);
    va_end(args);

    g_error_reported = 1;

    /* Callers' format strings already carry their own newline. */
    if (g_console) {
        printf("%s", msg);
        fflush(stdout);
    } else if (!g_quiet) {
        MessageBoxA(NULL, msg, "MouseMux Launcher", MB_OK | MB_ICONERROR);
    }
}

static void usage(void)
{
    char text[1024];

    _snprintf_s(text, sizeof text, _TRUNCATE,
        "MouseMux Chrome launcher\n\n"
        "  launcher            launch the next free seat\n"
        "  launcher -s N       launch seat N (1..%d)\n"
        "  launcher -n N       launch N seats\n"
        "  launcher -p PORT    base port (default %d)\n"
        "  launcher -k         keep Chrome's saved window placement\n"
        "  launcher -q         quiet, no output\n"
        "  launcher -h         this help\n\n"
        "Seat N uses port (base + N - 1) and profile user-data-N,\n"
        "both alongside chrome.exe in this folder.\n\n"
        "The launcher stays running while its browsers are open - that is\n"
        "what holds the seats.  Closing a browser frees its seat.\n\n"
        "Windows are cascaded %dpx per seat so seats stay distinct;\n"
        "pass -k to let Chrome restore each profile's own placement.",
        MAX_SEATS, DEFAULT_BASE_PORT, CASCADE_STEP);

    if (g_console) {
        printf("%s\n", text);
        fflush(stdout);
    } else {
        MessageBoxA(NULL, text, "MouseMux Launcher", MB_OK | MB_ICONINFORMATION);
    }
}

/* Strict integer parse.  atoi() cannot report failure: it maps "abc" to 0 and
 * "3x" to 3, so a typo would silently launch the wrong seat. */
static int parse_int(const char *text, int *out)
{
    char *end;
    long value;

    if (text == NULL || *text == '\0') {
        return 0;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

/* Directory this executable lives in, including the trailing backslash. */
static int get_exe_dir(char *out, DWORD count)
{
    char *last;
    DWORD len = GetModuleFileNameA(NULL, out, count);

    if (len == 0 || len >= count) {
        return 0;
    }
    last = strrchr(out, '\\');
    if (last == NULL) {
        return 0;
    }
    last[1] = '\0';
    return 1;
}

/* FNV-1a over the case-folded install path.  Only needs to separate one
 * install folder from another, not to resist anything, and both sides of a
 * collision would simply share a seat range. */
static unsigned path_hash(const char *path)
{
    unsigned hash = 2166136261u;

    while (*path) {
        hash ^= (unsigned char)tolower((unsigned char)*path++);
        hash *= 16777619u;
    }
    return hash;
}

/* Claims seat N, or returns NULL if another launcher already holds it.
 *
 * Creating rather than testing-then-creating: CreateMutex reports whether the
 * object already existed, so the claim and the check are one uninterruptible
 * step.  Two launchers starting at the same instant cannot both win. */
static HANDLE claim_seat(int seat)
{
    char name[LOCK_NAME_MAX];
    HANDLE lock;

    if (_snprintf_s(name, sizeof(name), _TRUNCATE, SEAT_LOCK_FORMAT,
                    g_install_hash, seat) < 0) {
        return NULL;
    }
    lock = CreateMutexA(NULL, FALSE, name);
    if (lock == NULL) {
        return NULL;
    }
    /* A handle comes back either way, so an existing object must be closed
     * rather than leaked - it belongs to the launcher that got there first. */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(lock);
        return NULL;
    }
    return lock;
}

/* True if nothing is listening on 127.0.0.1:port.  Used only to warn about a
 * control server that did not come up; it takes no part in tracking seats,
 * which is what it was bad at. */
static int is_port_free(int port)
{
    SOCKET s;
    struct sockaddr_in addr;
    BOOL exclusive = TRUE;
    int result;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return 0;
    }
    /* Without this, bind() succeeds against a socket in TIME_WAIT and we
     * would report a port free while its previous owner is still closing. */
    (void)setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive,
                     sizeof(exclusive));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        closesocket(s);
        return 0;
    }

    result = bind(s, (struct sockaddr *)&addr, sizeof(addr));
    closesocket(s);
    return result == 0;
}

/* Advisory only: the seat is up and usable by a person either way, and only
 * the control API would notice. */
static void warn_if_control_port_dead(int seat, int port)
{
    int waited = 0;

    if (!g_winsock || g_quiet || !g_console) {
        return;
    }
    while (waited < PORT_GRACE_MS) {
        if (!is_port_free(port)) {
            return;
        }
        Sleep(POLL_INTERVAL_MS);
        waited += POLL_INTERVAL_MS;
    }
    say("  NOTE: seat %d is up but its control port %d is not listening.\n"
        "        The browser works; the control API for this seat does not.\n",
        seat, port);
}

/* Starts a browser on seat N and, on success, leaves the seat mutex and the
 * process handle open in *slot for the caller to hold. */
static seat_result start_seat(const char *dir, int seat, seat_slot *slot)
{
    char exe[MAX_PATH];
    char cmd[2048];
    char placement[128];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE lock;
    int port = g_base_port + seat - 1;

    /* Costs nothing and spawns nothing, so this skip is not charged against
     * the spawn budget however many seats are already running. */
    lock = claim_seat(seat);
    if (lock == NULL) {
        say("Seat %d already running\n", seat);
        return SEAT_OCCUPIED;
    }

    /* _TRUNCATE makes these return -1 rather than overflow.  A silently
     * truncated path would launch the wrong executable, or hand Chrome a
     * malformed profile path, so treat it as fatal. */
    if (_snprintf_s(exe, sizeof(exe), _TRUNCATE, "%schrome.exe", dir) < 0) {
        fail("ERROR: path too long for seat %d\n  %s\n", seat, dir);
        CloseHandle(lock);
        return SEAT_FAILED;
    }
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        fail("ERROR: chrome.exe not found next to launcher\n  %s\n", exe);
        CloseHandle(lock);
        return SEAT_FAILED;
    }

    if (g_keep_placement) {
        placement[0] = '\0';
    } else if (_snprintf_s(placement, sizeof(placement), _TRUNCATE,
                           " --window-position=%d,%d --window-size=%d,%d",
                           CASCADE_ORIGIN + (seat - 1) * CASCADE_STEP,
                           CASCADE_ORIGIN + (seat - 1) * CASCADE_STEP,
                           WINDOW_WIDTH, WINDOW_HEIGHT) < 0) {
        fail("ERROR: could not build window placement for seat %d\n", seat);
        CloseHandle(lock);
        return SEAT_FAILED;
    }

    /* --do-not-de-elevate is what makes waiting on this process meaningful.
     * An elevated launch otherwise relaunches Chrome and lets the original go,
     * and we would release the seat while its browser was still running. */
    if (_snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                    "\"%schrome.exe\" --enable-features=MouseMuxIntegration "
                    "--do-not-de-elevate --mousemux-control-port=%d "
                    "--user-data-dir=\"%suser-data-%d\" --no-first-run%s",
                    dir, port, dir, seat, placement) < 0) {
        fail("ERROR: command line too long for seat %d\n", seat);
        CloseHandle(lock);
        return SEAT_FAILED;
    }

    say("Starting seat %d  ->  port %d  profile user-data-%d\n", seat, port,
        seat);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* cmd must be writable: CreateProcess may modify it in place. */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        fail("ERROR: CreateProcess failed (%lu)\n", GetLastError());
        CloseHandle(lock);
        return SEAT_FAILED;
    }
    CloseHandle(pi.hThread);

    if (WaitForSingleObject(pi.hProcess, STARTUP_PROBE_MS) == WAIT_OBJECT_0) {
        /* Gone already: it handed its command line to an instance already
         * holding this profile.  The next seat has its own profile, so it is
         * worth trying - but this cost a process launch, and a run of them
         * means the profiles are in a state no amount of scanning will fix. */
        CloseHandle(pi.hProcess);
        CloseHandle(lock);
        say("  Seat %d handed off to a running instance - trying the next\n",
            seat);
        if (++g_spawn_misses >= MAX_SPAWN_MISSES) {
            fail("ERROR: %d launches in a row exited without taking a seat.\n"
                 "Chrome is already running on these profiles.  Close every\n"
                 "Chrome window from this folder and try again.\n",
                 g_spawn_misses);
            return SEAT_FAILED;
        }
        return SEAT_OCCUPIED;
    }

    say("  Seat %d ready\n", seat);
    warn_if_control_port_dead(seat, port);

    slot->seat = seat;
    slot->lock = lock;
    slot->process = pi.hProcess;
    return SEAT_STARTED;
}

/* Starts the first free seat. */
static seat_result start_next_free_seat(const char *dir, seat_slot *slot)
{
    int seat;

    for (seat = 1; seat <= MAX_SEATS; seat++) {
        seat_result result = start_seat(dir, seat, slot);

        if (result != SEAT_OCCUPIED) {
            return result;
        }
    }
    fail("ERROR: all %d seats are in use\n", MAX_SEATS);
    return SEAT_FAILED;
}

static void release_slot(seat_slot *slot)
{
    CloseHandle(slot->process);
    CloseHandle(slot->lock);  /* frees the seat */
}

/* Holds every started seat until its browser exits.  This is the whole point
 * of the launcher staying alive: the mutex it holds IS the seat, so seat N is
 * running for exactly as long as this call keeps its handle open.  Seats are
 * released one at a time, as each browser closes. */
static void hold_seats(seat_slot *slots, int count)
{
    while (count > 0) {
        HANDLE handles[MAX_SEATS];
        DWORD signalled;
        int i;

        for (i = 0; i < count; i++) {
            handles[i] = slots[i].process;
        }

        signalled = WaitForMultipleObjects((DWORD)count, handles, FALSE,
                                           INFINITE);
        if (signalled >= WAIT_OBJECT_0 &&
            signalled < WAIT_OBJECT_0 + (DWORD)count) {
            i = (int)(signalled - WAIT_OBJECT_0);
        } else {
            /* Cannot wait any longer.  Releasing everything is the safer
             * failure: a seat wrongly marked free costs one duplicate
             * profile, while pinning seats forever needs a reboot to undo. */
            say("WARNING: cannot wait on seats (%lu), releasing %d seat(s)\n",
                GetLastError(), count);
            for (i = 0; i < count; i++) {
                release_slot(&slots[i]);
            }
            return;
        }

        say("Seat %d closed\n", slots[i].seat);
        release_slot(&slots[i]);
        slots[i] = slots[count - 1];
        count--;
    }
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    char dir[MAX_PATH];
    seat_slot slots[MAX_SEATS];
    int seat = 0;
    int count = 1;
    int started = 0;
    int i;

    console_attach();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "/?") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "-q") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "-k") == 0) {
            g_keep_placement = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                fail("ERROR: -s needs a seat number\n");
                return 1;
            }
            if (!parse_int(argv[++i], &seat) || seat < 1 || seat > MAX_SEATS) {
                fail("ERROR: seat must be 1..%d\n", MAX_SEATS);
                return 1;
            }
        } else if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                fail("ERROR: -n needs a count\n");
                return 1;
            }
            if (!parse_int(argv[++i], &count) || count < 1 ||
                count > MAX_SEATS) {
                fail("ERROR: count must be 1..%d\n", MAX_SEATS);
                return 1;
            }
        } else if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                fail("ERROR: -p needs a port number\n");
                return 1;
            }
            if (!parse_int(argv[++i], &g_base_port) || g_base_port < 1 ||
                g_base_port > 65535 - MAX_SEATS) {
                fail("ERROR: base port must be 1..%d\n", 65535 - MAX_SEATS);
                return 1;
            }
        } else {
            fail("ERROR: unknown option '%s' (try -h)\n", argv[i]);
            return 1;
        }
    }

    if (!get_exe_dir(dir, MAX_PATH)) {
        fail("ERROR: could not determine launcher directory\n");
        return 1;
    }
    g_install_hash = path_hash(dir);

    /* Only the control-port warning needs sockets now, so a winsock failure
     * costs a diagnostic rather than the whole run. */
    g_winsock = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    if (seat > 0) {
        count = 1;
        started = (start_seat(dir, seat, &slots[0]) == SEAT_STARTED) ? 1 : 0;
        if (started == 0 && !g_error_reported) {
            fail("Seat %d is already running\n", seat);
        }
    } else {
        for (i = 0; i < count; i++) {
            if (start_next_free_seat(dir, &slots[started]) != SEAT_STARTED) {
                break;
            }
            started++;
        }
    }

    /* Reported before settling in to wait, since the wait lasts as long as
     * the browsers do and nothing would be printed until they closed. */
    if (started < count) {
        if (started > 0) {
            fail("Started %d of %d requested seat(s)\n", started, count);
        }
    }

    hold_seats(slots, started);

    if (g_winsock) {
        WSACleanup();
    }
    return started < count ? 1 : 0;
}

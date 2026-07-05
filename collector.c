/*
 * collector.c  —  System stats collector for htop-like Python UI
 *
 * Collects: CPU (total + per-core %), Memory, Swap,
 *           Load average, Uptime, and full Process list.
 *
 * Output: one JSON object per line, every second, forever.
 * Pipe  : ./collector | python3 ui.py
 *
 * Build : gcc -O2 -o collector collector.c -lpthread
 *
 * Threading model:
 *   - worker_thread  : collects /proc data + emits JSON every second
 *   - main thread    : waits for keyboard input ('q' = quit, Ctrl-C = quit)
 *                      uses a mutex + condvar to signal the worker to stop
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>   /* POSIX threads */
#include <termios.h>   /* for non-blocking raw keyboard read */

/* ─────────────────────── LIMITS ─────────────────────── */

#define MAX_CPUS   128
#define MAX_PROCS  1024
#define MAX_NAME   256
#define MAX_USER   64

/* ─────────────────────── STRUCTS ────────────────────── */

typedef struct {
    unsigned long long user, nice, system, idle;
    unsigned long long iowait, irq, softirq, steal;
} CPUStat;

typedef struct {
    long total, free, available, buffers, cached;
    long swap_total, swap_free;
} MemStat;

typedef struct {
    int    pid;
    char   user[MAX_USER];
    int    priority, nice;
    long   virt_kb, res_kb, shr_kb;
    char   state;
    double cpu_pct;
    double mem_pct;
    unsigned long long utime, stime;
    char   name[MAX_NAME];
    char   cmd[MAX_NAME];
    long   num_threads;
} ProcInfo;

/* ─────────────────────── GLOBALS ────────────────────── */

static CPUStat prev_total;
static CPUStat prev_cores[MAX_CPUS];
static CPUStat cur_total;
static CPUStat cur_cores[MAX_CPUS];
static int     num_cpus = 0;

static MemStat mem;

static ProcInfo procs[MAX_PROCS];
static int      num_procs = 0;

static int total_tasks   = 0;
static int total_threads = 0;
static int running_tasks = 0;

static double load1 = 0, load5 = 0, load15 = 0;
static double uptime_secs = 0;

/* saved previous ticks for per-process CPU % */
static unsigned long long prev_proc_ticks[MAX_PROCS];
static int                prev_proc_pids[MAX_PROCS];
static int                prev_proc_count = 0;

/* ─────────────────────── THREADING PRIMITIVES ────────── */

/*
 * mutex  : protects all shared globals above (procs[], mem, cpu stats…)
 *          The worker holds it while writing; main holds it while reading
 *          (not needed in this design since main only signals, but good
 *           practice if you later add sorting/filtering from main).
 *
 * cond   : used by main to wake the worker early when we want to stop,
 *          instead of waiting a full second for sleep() to finish.
 *
 * worker_should_stop : flag set by main (or signal handler) → worker exits.
 */
static pthread_mutex_t data_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stop_cond   = PTHREAD_COND_INITIALIZER;
static int             worker_should_stop = 0;   /* protected by data_mutex */

/* ─────────────────────── HELPERS ────────────────────── */

static void uid_to_name(int uid, char *buf, int buflen)
{
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) { snprintf(buf, buflen, "%d", uid); return; }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char name[64]; int u;
        if (sscanf(line, "%63[^:]:%*[^:]:%d", name, &u) == 2 && u == uid) {
            strncpy(buf, name, buflen - 1);
            buf[buflen - 1] = '\0';
            fclose(fp);
            return;
        }
    }
    fclose(fp);
    snprintf(buf, buflen, "%d", uid);
}

static unsigned long long total_ticks(const CPUStat *s)
{
    return s->user + s->nice + s->system + s->idle
         + s->iowait + s->irq + s->softirq + s->steal;
}

static double cpu_pct(const CPUStat *prev, const CPUStat *cur)
{
    unsigned long long dt = total_ticks(cur)  - total_ticks(prev);
    unsigned long long di = cur->idle - prev->idle;
    if (dt == 0) return 0.0;
    return (1.0 - (double)di / dt) * 100.0;
}

static void json_str(const char *s, char *out, int outlen)
{
    int i = 0, j = 0;
    while (s[i] && j < outlen - 2) {
        unsigned char c = (unsigned char)s[i++];
        if      (c == '"')  { out[j++] = '\\'; out[j++] = '"'; }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if (c < 0x20)  { /* skip other control chars */ }
        else                { out[j++] = c; }
    }
    out[j] = '\0';
}

/* ─────────────────────── READERS ────────────────────── */

static void read_cpu(void)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;

    char line[512];
    num_cpus = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &cur_total.user,   &cur_total.nice,
                   &cur_total.system, &cur_total.idle,
                   &cur_total.iowait, &cur_total.irq,
                   &cur_total.softirq,&cur_total.steal);
        } else if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
            int id;
            if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                       &id,
                       &cur_cores[num_cpus].user,   &cur_cores[num_cpus].nice,
                       &cur_cores[num_cpus].system, &cur_cores[num_cpus].idle,
                       &cur_cores[num_cpus].iowait, &cur_cores[num_cpus].irq,
                       &cur_cores[num_cpus].softirq,&cur_cores[num_cpus].steal) == 9)
                num_cpus++;
        } else break;
    }
    fclose(fp);
}

static void read_mem(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;

    char key[64]; long value; char unit[32];
    while (fscanf(fp, "%63s %ld %31s", key, &value, unit) >= 2) {
        if      (!strcmp(key,"MemTotal:"))     mem.total      = value;
        else if (!strcmp(key,"MemFree:"))      mem.free       = value;
        else if (!strcmp(key,"MemAvailable:")) mem.available  = value;
        else if (!strcmp(key,"Buffers:"))      mem.buffers    = value;
        else if (!strcmp(key,"Cached:"))       mem.cached     = value;
        else if (!strcmp(key,"SwapTotal:"))    mem.swap_total = value;
        else if (!strcmp(key,"SwapFree:"))     mem.swap_free  = value;
    }
    fclose(fp);
}

static void read_loadavg(void)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return;
    int r, t;
    fscanf(fp, "%lf %lf %lf %d/%d", &load1, &load5, &load15, &r, &t);
    fclose(fp);
}

static void read_uptime(void)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return;
    fscanf(fp, "%lf", &uptime_secs);
    fclose(fp);
}

static int read_proc_stat(int pid, ProcInfo *p)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    unsigned long utime_l, stime_l;
    long priority_l, nice_l, nthreads_l;
    unsigned long vsize;
    long rss;
    char comm[MAX_NAME], state;

    int r = fscanf(fp,
        "%*d (%255[^)]) %c %*d %*d %*d %*d %*d %*u "
        "%*u %*u %*u %*u "
        "%lu %lu %*d %*d "
        "%ld %ld %ld %*d %*u "
        "%lu %ld",
        comm, &state,
        &utime_l, &stime_l,
        &priority_l, &nice_l, &nthreads_l,
        &vsize, &rss);
    fclose(fp);
    if (r < 9) return 0;

    strncpy(p->name, comm, MAX_NAME - 1);
    p->state       = state;
    p->utime       = utime_l;
    p->stime       = stime_l;
    p->priority    = (int)priority_l;
    p->nice        = (int)nice_l;
    p->num_threads = nthreads_l;
    p->virt_kb     = (long)(vsize / 1024);
    p->res_kb      = rss * 4;
    return 1;
}

static long read_proc_shr(int pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    long size, res, shr;
    fscanf(fp, "%ld %ld %ld", &size, &res, &shr);
    fclose(fp);
    return shr * 4;
}

static int read_proc_uid(int pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256]; int uid = -1;
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, "Uid:", 4) == 0) { sscanf(line, "Uid:\t%d", &uid); break; }
    fclose(fp);
    return uid;
}

static void read_proc_cmdline(int pid, char *buf, int buflen)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) { buf[0] = '\0'; return; }
    int c, i = 0;
    while (i < buflen - 1 && (c = fgetc(fp)) != EOF)
        buf[i++] = (c == '\0') ? ' ' : c;
    while (i > 0 && buf[i-1] == ' ') i--;
    buf[i] = '\0';
    fclose(fp);
}

static void read_all_procs(void)
{
    DIR *dir = opendir("/proc");
    if (!dir) return;

    num_procs = total_tasks = total_threads = running_tasks = 0;

    unsigned long long cpu_dt =
        total_ticks(&cur_total) - total_ticks(&prev_total);
    if (cpu_dt == 0) cpu_dt = 1;

    struct dirent *ent;
    while ((ent = readdir(dir)) && num_procs < MAX_PROCS) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        int pid = atoi(ent->d_name);
        ProcInfo p;
        memset(&p, 0, sizeof(p));
        p.pid = pid;

        if (!read_proc_stat(pid, &p)) continue;

        p.shr_kb = read_proc_shr(pid);
        int uid  = read_proc_uid(pid);
        uid_to_name(uid, p.user, MAX_USER);
        read_proc_cmdline(pid, p.cmd, MAX_NAME);
        if (p.cmd[0] == '\0') {
            char tmp[MAX_NAME];
            snprintf(tmp, MAX_NAME, "[%s]", p.name);
            strncpy(p.cmd, tmp, MAX_NAME - 1);
        }

        unsigned long long cur_ticks = p.utime + p.stime;
        unsigned long long prev_ticks_val = 0;
        for (int j = 0; j < prev_proc_count; j++) {
            if (prev_proc_pids[j] == pid) {
                prev_ticks_val = prev_proc_ticks[j];
                break;
            }
        }
        unsigned long long proc_dt = cur_ticks - prev_ticks_val;
        p.cpu_pct = (double)proc_dt / cpu_dt * 100.0 * num_cpus;
        if (p.cpu_pct < 0.0) p.cpu_pct = 0.0;

        p.mem_pct = mem.total > 0
            ? (double)p.res_kb / mem.total * 100.0 : 0.0;

        total_tasks++;
        total_threads += (int)p.num_threads;
        if (p.state == 'R') running_tasks++;

        procs[num_procs++] = p;
    }
    closedir(dir);

    for (int i = 0; i < num_procs && i < MAX_PROCS; i++) {
        prev_proc_pids[i]   = procs[i].pid;
        prev_proc_ticks[i]  = procs[i].utime + procs[i].stime;
    }
    prev_proc_count = num_procs;
}

/* ─────────────────────── JSON OUTPUT ────────────────── */

static void emit_json(void)
{
    char safe[MAX_NAME * 2];

    printf("{");
    printf("\"timestamp\":%lld,", (long long)time(NULL));
    printf("\"uptime\":%.2f,", uptime_secs);
    printf("\"load\":[%.2f,%.2f,%.2f],", load1, load5, load15);
    printf("\"tasks\":{\"total\":%d,\"threads\":%d,\"running\":%d},",
           total_tasks, total_threads, running_tasks);
    printf("\"cpu_total\":%.2f,", cpu_pct(&prev_total, &cur_total));

    printf("\"cpu_cores\":[");
    for (int i = 0; i < num_cpus; i++) {
        printf("%.2f", cpu_pct(&prev_cores[i], &cur_cores[i]));
        if (i < num_cpus - 1) printf(",");
    }
    printf("],");

    printf("\"memory\":{"
           "\"total_kb\":%ld,"
           "\"free_kb\":%ld,"
           "\"available_kb\":%ld,"
           "\"used_kb\":%ld,"
           "\"buffers_kb\":%ld,"
           "\"cached_kb\":%ld"
           "},",
           mem.total, mem.free, mem.available,
           mem.total - mem.available,
           mem.buffers, mem.cached);

    printf("\"swap\":{"
           "\"total_kb\":%ld,"
           "\"free_kb\":%ld,"
           "\"used_kb\":%ld"
           "},",
           mem.swap_total, mem.swap_free,
           mem.swap_total - mem.swap_free);

    printf("\"processes\":[");
    for (int i = 0; i < num_procs; i++) {
        ProcInfo *p = &procs[i];

        json_str(p->user, safe, sizeof(safe));
        char user_safe[MAX_USER * 2];
        strncpy(user_safe, safe, sizeof(user_safe) - 1);

        json_str(p->name, safe, sizeof(safe));
        char name_safe[MAX_NAME * 2];
        strncpy(name_safe, safe, sizeof(name_safe) - 1);

        json_str(p->cmd, safe, sizeof(safe));

        printf("{\"pid\":%d,"
               "\"user\":\"%s\","
               "\"priority\":%d,"
               "\"nice\":%d,"
               "\"virt_kb\":%ld,"
               "\"res_kb\":%ld,"
               "\"shr_kb\":%ld,"
               "\"state\":\"%c\","
               "\"cpu_pct\":%.2f,"
               "\"mem_pct\":%.2f,"
               "\"threads\":%ld,"
               "\"name\":\"%s\","
               "\"cmd\":\"%s\"}",
               p->pid, user_safe,
               p->priority, p->nice,
               p->virt_kb, p->res_kb, p->shr_kb,
               p->state, p->cpu_pct, p->mem_pct,
               p->num_threads, name_safe, safe);

        if (i < num_procs - 1) printf(",");
    }
    printf("]");
    printf("}\n");
    fflush(stdout);
}

/* ─────────────────────── WORKER THREAD ──────────────── */

/*
 * worker_thread_func()
 *
 * Runs in a background thread. Every iteration:
 *   1. Collect all /proc data (under the mutex so main can safely read it)
 *   2. Emit one JSON line to stdout
 *   3. Sleep for 1 second — but instead of sleep(1) we use a timed
 *      pthread_cond_timedwait so that main can wake us immediately
 *      when it wants to shut down.
 */
static void *worker_thread_func(void *arg)
{
    (void)arg;

    /* ── seed the baseline CPU snapshot (same as original main) ── */
    read_cpu();
    pthread_mutex_lock(&data_mutex);
    prev_total = cur_total;
    memcpy(prev_cores, cur_cores, sizeof(CPUStat) * num_cpus);
    pthread_mutex_unlock(&data_mutex);

    /* wait 1 second before first real sample */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_mutex_lock(&data_mutex);
        /* also exit early if main signalled stop during the initial sleep */
        if (!worker_should_stop)
            pthread_cond_timedwait(&stop_cond, &data_mutex, &ts);
        int should_stop = worker_should_stop;
        pthread_mutex_unlock(&data_mutex);
        if (should_stop) return NULL;
    }

    while (1) {
        /* ── collect data (no mutex needed while writing local vars) ── */
        read_cpu();
        read_mem();
        read_loadavg();
        read_uptime();

        /* lock while updating the shared procs[] array and emitting JSON */
        pthread_mutex_lock(&data_mutex);

        read_all_procs();
        emit_json();

        /* promote current → previous CPU snapshots */
        prev_total = cur_total;
        memcpy(prev_cores, cur_cores, sizeof(CPUStat) * num_cpus);

        int should_stop = worker_should_stop;
        pthread_mutex_unlock(&data_mutex);

        if (should_stop) break;

        /* ── sleep 1 s, but wake immediately if stop is signalled ── */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        pthread_mutex_lock(&data_mutex);
        if (!worker_should_stop)
            pthread_cond_timedwait(&stop_cond, &data_mutex, &ts);
        should_stop = worker_should_stop;
        pthread_mutex_unlock(&data_mutex);

        if (should_stop) break;
    }

    return NULL;
}

/* ─────────────────────── SIGNAL HANDLER ─────────────── */

/*
 * On SIGINT / SIGTERM: set the stop flag and broadcast on the condvar
 * so the worker wakes up and exits cleanly instead of being killed mid-write.
 */
static void handle_signal(int sig)
{
    (void)sig;
    pthread_mutex_lock(&data_mutex);
    worker_should_stop = 1;
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&data_mutex);
}

/* ─────────────────────── TERMINAL HELPERS ───────────── */

/*
 * Put stdin into raw / non-blocking mode so we can read keystrokes
 * one character at a time without the user pressing Enter.
 * Restored on exit via restore_terminal().
 */
static struct termios orig_termios;

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void set_raw_terminal(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);          /* auto-restore on normal exit */

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);  /* disable line-buffering + echo */
    raw.c_cc[VMIN]  = 0;              /* non-blocking read */
    raw.c_cc[VTIME] = 1;              /* 100 ms timeout per read call */
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* ─────────────────────── MAIN ───────────────────────── */

int main(void)
{
    /* Install signal handlers */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Switch terminal to raw mode so 'q' is caught without Enter */
    set_raw_terminal();

    fprintf(stderr, "collector running — press 'q' or Ctrl-C to quit\n");

    /* Start the worker thread */
    pthread_t worker;
    if (pthread_create(&worker, NULL, worker_thread_func, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    /*
     * Main thread input loop.
     *
     * Recognized keys:
     *   q / Q  →  graceful shutdown
     *   (more keys can be added here later, e.g. sort order, pause…)
     */
    while (1) {
        /* Check if worker already stopped (e.g. from a signal) */
        pthread_mutex_lock(&data_mutex);
        int stopped = worker_should_stop;
        pthread_mutex_unlock(&data_mutex);
        if (stopped) break;

        char c = 0;
        int  n = read(STDIN_FILENO, &c, 1);

        if (n > 0) {
            if (c == 'q' || c == 'Q') {
                fprintf(stderr, "\nQuitting...\n");
                pthread_mutex_lock(&data_mutex);
                worker_should_stop = 1;
                pthread_cond_broadcast(&stop_cond);
                pthread_mutex_unlock(&data_mutex);
                break;
            }
            /* Additional key handlers can go here */
        }
    }

    /* Wait for the worker to finish its current cycle and exit */
    pthread_join(worker, NULL);

    /* Clean up POSIX primitives */
    pthread_mutex_destroy(&data_mutex);
    pthread_cond_destroy(&stop_cond);

    fprintf(stderr, "collector stopped cleanly.\n");
    return 0;
}

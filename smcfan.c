/*
 * smcfan - read SMC sensors and control fans on Intel Macs from the command line.
 *
 *   smcfan fans                       show each fan: actual / min / max / target / mode
 *   smcfan temps                      list every temperature sensor the SMC knows,
 *                                     flagging ones that read 0 or nonsense
 *   smcfan read KEY                   raw read of one SMC key (e.g. TC0P, F0Md)
 *   sudo smcfan set RPM               put all fans in manual mode at RPM (clamped to min..max)
 *   sudo smcfan auto                  give the fans back to the SMC
 *   sudo smcfan curve KEY LOW HIGH [SECS]
 *                                     loop forever: read temperature sensor KEY, fans at min
 *                                     at or below LOW C, at max at or above HIGH C, linear in
 *                                     between. Goes to max if the sensor can't be read.
 *                                     Returns fans to auto on Ctrl+C / launchd stop.
 *
 * Built without the macOS SDK: the few libc / IOKit declarations needed are below.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- libc / Mach / IOKit declarations ---- */
int printf(const char *, ...);
int fflush(void *);
int strcmp(const char *, const char *);
size_t strlen(const char *);
long strtol(const char *, char **, int);
double strtod(const char *, char **);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int usleep(unsigned int);
void exit(int);
unsigned int getuid(void);
typedef void (*sig_t)(int);
sig_t signal(int, sig_t);

typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
void *IOServiceMatching(const char *);
mach_port_t IOServiceGetMatchingService(mach_port_t, void *);
kern_return_t IOServiceOpen(mach_port_t, mach_port_t, uint32_t, mach_port_t *);
kern_return_t IOServiceClose(mach_port_t);
kern_return_t IOObjectRelease(mach_port_t);
kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);

#define OUT(...) do { printf(__VA_ARGS__); fflush(0); } while (0)

/* ---- AppleSMC user-client protocol ---- */
typedef struct { char major, minor, build, reserved; uint16_t release; } smc_vers_t;
typedef struct { uint16_t version, length; uint32_t cpu, gpu, mem; } smc_plimit_t;
typedef struct { uint32_t size; uint32_t type; char attr; } smc_keyinfo_t;
typedef struct {
    uint32_t key;
    smc_vers_t vers;
    smc_plimit_t plimit;
    smc_keyinfo_t info;
    char result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} smc_data_t;
_Static_assert(sizeof(smc_data_t) == 80, "SMC struct must be 80 bytes");

enum { SMC_READ = 5, SMC_WRITE = 6, SMC_INDEX = 8, SMC_INFO = 9, SMC_SELECTOR = 2 };

static mach_port_t conn;

static uint32_t fourcc(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}
static void unfourcc(uint32_t v, char *out) {
    out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v; out[4] = 0;
}

static int smc_call(smc_data_t *in, smc_data_t *out) {
    size_t outsz = sizeof(*out);
    memset(out, 0, sizeof(*out));
    kern_return_t kr = IOConnectCallStructMethod(conn, SMC_SELECTOR, in, sizeof(*in), out, &outsz);
    if (kr != 0) return -1;
    if (out->result != 0) return -2;
    return 0;
}

static void smc_open(void) {
    mach_port_t svc = IOServiceGetMatchingService(0, IOServiceMatching("AppleSMC"));
    if (!svc) { OUT("error: AppleSMC service not found\n"); exit(1); }
    if (IOServiceOpen(svc, mach_task_self_, 0, &conn) != 0) {
        OUT("error: cannot open AppleSMC\n"); exit(1);
    }
    IOObjectRelease(svc);
}

typedef struct { uint32_t size, type; uint8_t b[32]; } smc_val_t;

static int smc_read(const char *key, smc_val_t *v) {
    smc_data_t in, out;
    memset(&in, 0, sizeof in);
    in.key = fourcc(key);
    in.data8 = SMC_INFO;
    if (smc_call(&in, &out)) return -1;
    v->size = out.info.size;
    v->type = out.info.type;
    in.info.size = out.info.size;
    in.data8 = SMC_READ;
    if (smc_call(&in, &out)) return -1;
    memcpy(v->b, out.bytes, 32);
    return 0;
}

static int smc_write(const char *key, const uint8_t *bytes, uint32_t size) {
    smc_data_t in, out;
    memset(&in, 0, sizeof in);
    in.key = fourcc(key);
    in.data8 = SMC_WRITE;
    in.info.size = size;
    memcpy(in.bytes, bytes, size);
    return smc_call(&in, &out);
}

/* Decode common SMC numeric types to a double. Returns 0 on success. */
static int smc_num(const smc_val_t *v, double *d) {
    char t[5]; unfourcc(v->type, t);
    if (!strcmp(t, "flt ") && v->size == 4) { float f; memcpy(&f, v->b, 4); *d = f; return 0; }
    if (!strcmp(t, "sp78") && v->size == 2) { *d = (int16_t)((v->b[0] << 8) | v->b[1]) / 256.0; return 0; }
    if (!strcmp(t, "fpe2") && v->size == 2) { *d = ((v->b[0] << 8) | v->b[1]) / 4.0; return 0; }
    if (!strcmp(t, "ui8 ") && v->size == 1) { *d = v->b[0]; return 0; }
    if (!strcmp(t, "ui16") && v->size == 2) { *d = (v->b[0] << 8) | v->b[1]; return 0; }
    if (!strcmp(t, "ui32") && v->size == 4) {
        *d = ((uint32_t)v->b[0] << 24) | (v->b[1] << 16) | (v->b[2] << 8) | v->b[3]; return 0;
    }
    return -1;
}

static int read_num(const char *key, double *d) {
    smc_val_t v;
    if (smc_read(key, &v)) return -1;
    return smc_num(&v, d);
}

/* Write an RPM value to a fan key, using whatever type the key has. */
static int write_rpm(const char *key, double rpm) {
    smc_val_t v;
    if (smc_read(key, &v)) return -1;
    char t[5]; unfourcc(v.type, t);
    uint8_t b[4];
    if (!strcmp(t, "flt ")) { float f = (float)rpm; memcpy(b, &f, 4); return smc_write(key, b, 4); }
    if (!strcmp(t, "fpe2")) { unsigned r = (unsigned)rpm << 2; b[0] = r >> 8; b[1] = r; return smc_write(key, b, 2); }
    return -2;
}

static int fan_count(void) {
    double n;
    if (read_num("FNum", &n)) return 0;
    return (int)n;
}

static void fkey(char *buf, int fan, const char *suffix) {
    buf[0] = 'F'; buf[1] = '0' + fan; buf[2] = suffix[0]; buf[3] = suffix[1]; buf[4] = 0;
}

static int set_mode(int fan, int manual) {
    char k[5]; fkey(k, fan, "Md");
    uint8_t m = manual ? 1 : 0;
    return smc_write(k, &m, 1);
}

static void need_root(void) {
    if (getuid() != 0) { OUT("This needs root: run it with sudo\n"); exit(1); }
}

/* ---- commands ---- */
static void cmd_fans(void) {
    int n = fan_count();
    OUT("%d fan(s)\n", n);
    for (int i = 0; i < n; i++) {
        char k[5]; double ac = -1, mn = -1, mx = -1, tg = -1, md = -1;
        fkey(k, i, "Ac"); read_num(k, &ac);
        fkey(k, i, "Mn"); read_num(k, &mn);
        fkey(k, i, "Mx"); read_num(k, &mx);
        fkey(k, i, "Tg"); read_num(k, &tg);
        fkey(k, i, "Md"); read_num(k, &md);
        OUT("fan %d: actual %.0f rpm | min %.0f | max %.0f | target %.0f | mode %s\n",
            i, ac, mn, mx, tg, md < 0 ? "?" : (md > 0 ? "manual" : "auto"));
    }
}

static void cmd_temps(void) {
    double total;
    if (read_num("#KEY", &total)) { OUT("error: cannot read key count\n"); return; }
    OUT("Scanning %.0f SMC keys for temperature sensors...\n", total);
    int shown = 0, bad = 0;
    for (uint32_t i = 0; i < (uint32_t)total; i++) {
        smc_data_t in, out;
        memset(&in, 0, sizeof in);
        in.data8 = SMC_INDEX;
        in.data32 = i;
        if (smc_call(&in, &out)) continue;
        char name[5]; unfourcc(out.key, name);
        if (name[0] != 'T') continue;
        smc_val_t v; double d;
        if (smc_read(name, &v) || smc_num(&v, &d)) continue;
        char t[5]; unfourcc(v.type, t);
        const char *flag = "";
        if (d <= 0.5 || d > 125) { flag = "   <-- suspicious"; bad++; }
        OUT("%s  %7.2f C  (%s)%s\n", name, d, t, flag);
        shown++;
    }
    OUT("%d temperature keys, %d suspicious (0, negative or above 125 C)\n", shown, bad);
}

static void cmd_read(const char *key) {
    if (strlen(key) != 4) { OUT("SMC keys are exactly 4 characters (pad with spaces, e.g. \"FNum\")\n"); return; }
    smc_val_t v;
    if (smc_read(key, &v)) { OUT("%s: cannot read (missing key?)\n", key); return; }
    char t[5]; unfourcc(v.type, t);
    OUT("%s  type '%s'  size %u  bytes:", key, t, v.size);
    for (uint32_t i = 0; i < v.size && i < 32; i++) OUT(" %02x", v.b[i]);
    double d;
    if (!smc_num(&v, &d)) OUT("  = %.2f", d);
    OUT("\n");
}

static void set_all(double rpm_frac_or_abs, int is_fraction, int quiet) {
    int n = fan_count();
    for (int i = 0; i < n; i++) {
        char k[5]; double mn = 0, mx = 0;
        fkey(k, i, "Mn"); read_num(k, &mn);
        fkey(k, i, "Mx"); read_num(k, &mx);
        double rpm = is_fraction ? mn + (mx - mn) * rpm_frac_or_abs : rpm_frac_or_abs;
        if (rpm < mn) rpm = mn;
        if (mx > 0 && rpm > mx) rpm = mx;
        int e1 = set_mode(i, 1);
        fkey(k, i, "Tg");
        int e2 = write_rpm(k, rpm);
        if (!quiet || e1 || e2)
            OUT("fan %d -> %.0f rpm%s\n", i, rpm, (e1 || e2) ? "  (SMC refused the write)" : "");
    }
}

static volatile int stop_flag = 0;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void cmd_auto(void) {
    int n = fan_count();
    for (int i = 0; i < n; i++)
        OUT("fan %d -> auto%s\n", i, set_mode(i, 0) ? "  (SMC refused the write)" : "");
}

static void cmd_curve(const char *key, double lo, double hi, double secs) {
    if (hi <= lo) { OUT("HIGH must be above LOW\n"); return; }
    signal(2, on_signal);   /* SIGINT  */
    signal(15, on_signal);  /* SIGTERM (launchd stop) */
    OUT("Fan curve on %s: min rpm at <= %.0f C, max rpm at >= %.0f C, every %.0fs\n", key, lo, hi, secs);
    int last_pct = -1;
    while (!stop_flag) {
        double t, frac;
        if (read_num(key, &t) || t <= 0.5 || t > 125) {
            frac = 1.0;
            OUT("cannot read a sane value from %s -> fans to max\n", key);
            last_pct = -1;
        } else {
            frac = (t - lo) / (hi - lo);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int pct = (int)(frac * 100 + 0.5);
            if (last_pct < 0 || pct - last_pct >= 5 || last_pct - pct >= 5) {
                OUT("%s %.1f C -> fans at %d%% of range\n", key, t, pct);
                last_pct = pct;
            }
        }
        set_all(frac, 1, 1);
        for (int i = 0; i < (int)(secs * 10) && !stop_flag; i++) usleep(100000);
    }
    OUT("stopping: returning fans to SMC control\n");
    cmd_auto();
}

static void usage(void) {
    OUT("usage:\n"
        "  smcfan fans\n"
        "  smcfan temps\n"
        "  smcfan read KEY\n"
        "  sudo smcfan set RPM\n"
        "  sudo smcfan auto\n"
        "  sudo smcfan curve KEY LOW_C HIGH_C [SECS]\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    smc_open();
    const char *c = argv[1];
    if (!strcmp(c, "fans")) cmd_fans();
    else if (!strcmp(c, "temps")) cmd_temps();
    else if (!strcmp(c, "read") && argc == 3) cmd_read(argv[2]);
    else if (!strcmp(c, "set") && argc == 3) { need_root(); set_all(strtod(argv[2], 0), 0, 0); }
    else if (!strcmp(c, "auto")) { need_root(); cmd_auto(); }
    else if (!strcmp(c, "curve") && (argc == 5 || argc == 6)) {
        need_root();
        cmd_curve(argv[2], strtod(argv[3], 0), strtod(argv[4], 0), argc == 6 ? strtod(argv[5], 0) : 3);
    }
    else { usage(); IOServiceClose(conn); return 1; }
    IOServiceClose(conn);
    return 0;
}

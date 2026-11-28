/* wifi_osi_funcs_t (OS adapter v0x9) for the S31 blob on the llgo fiber
 * runtime.
 *
 * The table matches idfparts/include/esp_wifi/esp_private/wifi_os_adapter.h
 * from the pinned blob build (espressif/idf:latest, IDF ad20ded62a2, see
 * idfparts/MANIFEST.md); the S31 #if blocks resolve as: no phy_common_clock
 * (ESP32/S2 only), has slowclk_cal_get, has the S31 regdma/sleep-retention
 * and coex_configure_preemption_end_cb blocks, has wifi_disable_ac_ax
 * (CONFIG_SOC_WIFI_HE_SUPPORT=1).
 *
 * Entry mapping rules (espradio's osi.c is the reference for the pitfalls):
 *
 *   - blocking primitives (sem/mutex/queue/event-group/task-delay/timer)
 *     point at Go exports: they park the calling goroutine on fiber-runtime
 *     primitives and preserve the goroutine-identity variable across the
 *     park (the package comment in osal.go).
 *   - everything reachable from the MAC/BB ISRs stays in C: int masking,
 *     _is_from_isr, _queue_send_from_isr, _task_yield_from_isr. ISRs never
 *     touch the Go scheduler; see _wrap/osal_intr.c for the trap machinery.
 *   - memory entries are newlib allocator passthroughs - the blob also
 *     calls malloc/free directly (outside the table), so one allocator
 *     serves both worlds and pointers may cross in either direction.
 *   - coex/sleep-retention/NVS entries are stubs: no BT coexistence, no
 *     modem power management, and PHY calibration is not persisted (P1).
 *     Their stub return values follow the IDF adapter's CONFIG=n branches
 *     (verified in esp32s31/esp_adapter.c of the pinned IDF).
 *
 * ISR staging rings: _queue_send_from_isr may run on the MAC ISR (pp_post
 * -> xphyQueue). It stages the item into a C-owned lock-free ring (one per
 * queue) and the task-side _queue_recv drains it before each park, which is
 * how an ISR-posted item wakes a parked goroutine without any scheduler
 * access from interrupt context (espradio solves this with the same shape:
 * wifi_isr.go's lock-free ring plus drain-on-entry).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include "esp_private/wifi_os_adapter.h"
#include "esp_phy_init.h"   /* esp_phy_init_data_t, esp_phy_calibration_data_t */
#include "phy_init_data.h"  /* phy_init_data default table (phy_init_data.c) */

/* ---- Go exports referenced by the table (exported.go) ---- */

extern bool llgo_osal_env_is_chip(void);

extern void *llgo_osal_semphr_create(uint32_t max, uint32_t init);
extern void llgo_osal_semphr_delete(void *sem);
extern int32_t llgo_osal_semphr_take(void *sem, uint32_t tick);
extern void *llgo_osal_mutex_create(void);
extern void *llgo_osal_recursive_mutex_create(void);
extern void llgo_osal_mutex_delete(void *m);
extern void *llgo_osal_queue_create(uint32_t len, uint32_t item_size);
extern void llgo_osal_queue_delete(void *q);
extern void *llgo_osal_event_group_create(void);
extern void llgo_osal_event_group_delete(void *e);
extern uint32_t llgo_osal_event_group_clear_bits(void *e, uint32_t bits);
extern int32_t llgo_osal_task_get_max_priority(void);
extern int32_t llgo_osal_semphr_give(void *sem);
extern void *llgo_osal_wifi_thread_semphr_get(void);
extern int32_t llgo_osal_mutex_lock(void *m, uint32_t tick);
extern int32_t llgo_osal_mutex_unlock(void *m);
extern int32_t llgo_osal_queue_send(void *q, void *item, uint32_t tick);
extern int32_t llgo_osal_queue_send_to_back(void *q, void *item, uint32_t tick);
extern int32_t llgo_osal_queue_send_to_front(void *q, void *item, uint32_t tick);
extern int32_t llgo_osal_queue_recv(void *q, void *item, uint32_t tick);
extern uint32_t llgo_osal_queue_msg_waiting(void *q);
extern uint32_t llgo_osal_event_group_set_bits(void *e, uint32_t bits);
extern uint32_t llgo_osal_event_group_wait_bits(void *e, uint32_t bits, int32_t clear, int32_t all, uint32_t tick);
extern int32_t llgo_osal_task_create_pinned_to_core(void *fn, const char *name, uint32_t stack, void *param, uint32_t prio, void *handle_out, uint32_t core);
extern int32_t llgo_osal_task_create(void *fn, const char *name, uint32_t stack, void *param, uint32_t prio, void *handle_out);
extern void llgo_osal_task_delete(void *h);
extern void llgo_osal_task_delay(uint32_t tick);
extern int32_t llgo_osal_task_ms_to_tick(uint32_t ms);
extern void *llgo_osal_task_get_current_task(void);
extern int32_t llgo_osal_event_post(const char *base, int32_t id, void *data, size_t size, uint32_t tick);
extern void llgo_osal_timer_arm(void *t, uint32_t ms, int32_t repeat);
extern void llgo_osal_timer_arm_us(void *t, uint32_t us, int32_t repeat);

/* The OSI mutex entries carry no tick parameter (IDF adapters take the
 * mutex without timeout); the Go exports keep the tick shape for queue-recv
 * parity, normalized through these shims. */
static int32_t osal_mutex_lock_c(void *m) {
    return llgo_osal_mutex_lock(m, OSI_FUNCS_TIME_BLOCKING);
}
static int32_t osal_mutex_unlock_c(void *m) {
    return llgo_osal_mutex_unlock(m);
}

/* bool is a 1-byte ABI type; the Go side reads a full word, so normalize
 * through these shims instead of casting the table entry. */
static void osal_timer_arm_c(void *t, uint32_t ms, bool repeat) {
    llgo_osal_timer_arm(t, ms, repeat ? 1 : 0);
}
static void osal_timer_arm_us_c(void *t, uint32_t us, bool repeat) {
    llgo_osal_timer_arm_us(t, us, repeat ? 1 : 0);
}
extern void llgo_osal_timer_disarm(void *t);
extern void llgo_osal_timer_setfn(void *t, void *fn, void *arg);
extern void llgo_osal_timer_done(void *t);
extern void llgo_osal_crit_enter(void);
extern void llgo_osal_crit_exit(void);

/* ---- interrupt machinery (osal_intr.c) ---- */

extern bool llgo_osal_is_from_isr_context(void);
extern void llgo_osal_set_intr(int32_t cpu, uint32_t source, uint32_t num, int32_t prio);
extern void llgo_osal_clear_intr(uint32_t source, uint32_t num);
extern void llgo_osal_set_isr(int32_t n, void *f, void *arg);
extern void llgo_osal_ints_on(uint32_t mask);
extern void llgo_osal_ints_off(uint32_t mask);
extern uint32_t llgo_osal_wifi_int_disable(void *mux);
extern void llgo_osal_wifi_int_restore(void *mux, uint32_t prev);
extern void *llgo_osal_spin_lock_create(void);
extern void llgo_osal_spin_lock_delete(void *lock);
extern void llgo_osal_task_yield_from_isr(void);

/* ---- ISR staging rings ---- */

/* One ring per queue handle. The blob's queues are small (the pp task queue
 * is created via _wifi_create_queue; pp_post stages into xphyQueue which is
 * one of them), so 8 slots at the create-time item size covers the real
 * producer (MAC ISR posts pp events) with slack. A full ring drops and
 * counts - the same observable failure shape as espradio's QueueSendFull.
 */
#define OSAL_ISR_RING_SLOTS 8

typedef struct {
    uint32_t item_size;
    volatile uint32_t head; /* writer index (ISR side) */
    volatile uint32_t tail; /* reader index (task side) */
    uint32_t drops;
    uint8_t slots[OSAL_ISR_RING_SLOTS][64];
} osal_isr_ring_t;

#define OSAL_ISR_RING_MAX 16
static osal_isr_ring_t *s_isr_rings[OSAL_ISR_RING_MAX];

/* Task-side helpers shared with queue_recv (called from Go via linkname). */
int32_t llgo_osal_isr_ring_put(uint32_t handle, void *item, uint32_t item_size) {
    if (handle == 0 || handle > OSAL_ISR_RING_MAX) {
        return 0;
    }
    osal_isr_ring_t *r = s_isr_rings[handle - 1];
    if (r == 0) {
        return 0;
    }
    /* Same-hart producer/consumer: the mask only guards against re-entrant
     * pushes from nested interrupts. */
    uint32_t head = r->head;
    uint32_t next = (head + 1u) % OSAL_ISR_RING_SLOTS;
    if (next == r->tail) {
        r->drops++;
        return 0;
    }
    memcpy(r->slots[head], item, r->item_size);
    (void)item_size;
    r->head = next;
    return 1;
}

int32_t llgo_osal_isr_ring_get(uint32_t handle, void *out) {
    if (handle == 0 || handle > OSAL_ISR_RING_MAX) {
        return 0;
    }
    osal_isr_ring_t *r = s_isr_rings[handle - 1];
    if (r == 0 || r->tail == r->head) {
        return 0;
    }
    memcpy(out, r->slots[r->tail], r->item_size);
    r->tail = (r->tail + 1u) % OSAL_ISR_RING_SLOTS;
    return 1;
}

/* _queue_send_from_isr: stage into the C ring, never touch Go state. */
int32_t llgo_osal_queue_send_from_isr(void *q, void *item, void *hptw) {
    (void)hptw; /* FreeRTOS high-priority-task wake marker; no preemption here */
    uintptr_t h = (uintptr_t)q;
    return llgo_osal_isr_ring_put(h, item, 0);
}

/* ---- queue create/delete: Go registry + C ring, both keyed by the handle ---- */


/* _wifi_create_queue contract (pinned IDF esp32s31/esp_adapter.c:
 * wifi_create_queue): the return value is a wifi_static_queue_t* - the blob
 * dereferences it (pp_create_task reads the handle at offset 0 and publishes
 * it to the ROM xphyQueue slot), so this cannot be an opaque integer like
 * the plain _os_queue_create handles. The wrapper carries our opaque queue
 * id in the handle field and no storage (our Go queue owns its buffer). */
typedef struct {
    uintptr_t handle; /* our opaque queue id, at the offset the blob reads */
    void *storage;    /* FreeRTOS static-storage slot; unused here */
} osal_wifi_queue_t;

void *llgo_osal_wifi_create_queue(int queue_len, int item_size) {
    uintptr_t h = (uintptr_t)llgo_osal_queue_create((uint32_t)queue_len, (uint32_t)item_size);
    if (h == 0 || h > OSAL_ISR_RING_MAX) {
        return 0;
    }
    uint32_t ring_slot = (uint32_t)h - 1;
    if (s_isr_rings[ring_slot] == 0) {
        osal_isr_ring_t *r = malloc(sizeof(osal_isr_ring_t));
        if (r != 0) {
            memset(r, 0, sizeof(*r));
            r->item_size = item_size > 64 ? 64 : (uint32_t)item_size;
            s_isr_rings[ring_slot] = r;
        }
    }
    osal_wifi_queue_t *w = malloc(sizeof(osal_wifi_queue_t));
    if (w == 0) {
        return 0;
    }
    w->handle = h;
    w->storage = 0;
    return w;
}

void llgo_osal_wifi_delete_queue(void *q) {
    osal_wifi_queue_t *w = (osal_wifi_queue_t *)q;
    if (w == 0) {
        return;
    }
    uintptr_t h = w->handle;
    if (h >= 1 && h <= OSAL_ISR_RING_MAX) {
        free(s_isr_rings[h - 1]);
        s_isr_rings[h - 1] = 0;
    }
    llgo_osal_queue_delete((void *)h);
    free(w);
}

/* ---- queue_recv with ISR-ring drain (Go owns the park; this is the C
 * wrapper the table points at) ---- */

/* Forensics: dump the wifi g_ic scan-window words + the message's handler
 * slot before the wifi task body dereferences it. Offsets chosen from the
 * gate-3 crash analysis (g_ic+0x560/0x568/0x570); g_ic itself lives at
 * g_ic_ptr slot 0x2f0033ac-linked blob global. */
void osal_dump_msg(void *q, void *item, const char *how) {
    volatile uint32_t *gic = (volatile uint32_t *)0x2f0033ac;
    uint32_t *m = (uint32_t *)item;
    char buf[192];
    snprintf(buf, sizeof(buf),
             "qmsg %s q=0x%p h=0x%08x a0=0x%08x a1=0x%08x | gic+0x560=0x%08x +0x568=0x%08x +0x570=0x%08x\n",
             how, q, m[0], m[1], m[2],
             gic[0x560 / 4], gic[0x568 / 4], gic[0x570 / 4]);
    write(1, buf, strlen(buf));
}

int32_t llgo_osal_queue_recv_wrap(void *q, void *item, uint32_t tick) {
    /* Drain staged ISR items first so an ISR post wakes the parked recv on
     * its next bounded-park tick. */
    if (llgo_osal_isr_ring_get((uintptr_t)q, item)) {
        osal_dump_msg(q, item, "isr");
        return 1;
    }
    int32_t rc = llgo_osal_queue_recv(q, item, tick);
    if (rc == 1) {
        osal_dump_msg(q, item, "recv");
    }
    return rc;
}


/* ---- blob-facing helpers ---- */

/* ---- ROM newlib syscall table ----
 * The ESP32-S31 ROM's newlib entries are table-jump stubs: they load
 * `syscall_table_ptr` (ROM BSS at 0x2f07ffc8, zero-initialized here - the
 * emu's ROM model does not run the ROM's own init) and jump through a slot
 * (observed: __getreent slot 0, _malloc_r slot 4, _printf_float slot 0x88,
 * __sinit slot 0x94). Any call reaching such a stub with the pointer still
 * NULL faults on the slot dereference. This device has strong newlib
 * implementations of everything the ROM table routes to, so install a
 * static table that pins every slot used by ROM entries to a safe target:
 * the known slots to the matching strong symbols, everything else to a
 * logging abort stub. */
extern void **syscall_table_ptr;

/* The strong device-libc implementations the table routes to. malloc.h /
 * stdlib.h (included above) already carry the _r allocator prototypes;
 * reent/stdio prototypes come from the same headers, so only the entries
 * without a usable declaration are spelled out here. */
extern struct _reent *__getreent(void);
extern int _fstat_r(struct _reent *, int, void *);
extern void *_sbrk_r(struct _reent *, int);
/* Internal newlib stdio initializer - no public header declares it. */
extern void __sinit(struct _reent *);

/* Newlib stdio retarget locks (syscall table slots 0x74/0x84). The ROM stdio
 * paths (the blob's direct printf calls land there) take and release these
 * around every FILE operation. Single-hart cooperative fibers never observe
 * concurrent stdio, and no OSAL entry yields inside a stdio call, so no-op
 * bodies are sufficient for P1. */
static void osal_lock_noop(void) {}

static void *s_rom_syscall_table[42]; /* covers slot 0x98 and a little more */

/* Slot-catch-all: called when a ROM stub jumps through a slot we did not
 * route deliberately. The slot-jump stubs tail into the table entry, so the
 * return address is the ROM stub itself - print it so a new ROM caller shows
 * up as an identifiable address instead of a bare fault. Manual hex (no
 * newlib stdio in this path). */
static void osal_print_hex(uintptr_t v) {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 9; i >= 2; i--) {
        unsigned d = (unsigned)(v & 0xfu);
        buf[i] = d < 10 ? ('0' + d) : ('a' + d - 10);
        v >>= 4;
    }
    buf[10] = '\n';
    write(1, buf, sizeof(buf));
}

static void *llgo_osal_rom_slot_miss(void) {
    static const char msg[] = "osal: ROM syscall slot miss, stub=";
    uintptr_t ra;
    __asm__ volatile ("mv %0, ra" : "=r"(ra));
    write(1, msg, sizeof(msg) - 1);
    osal_print_hex(ra);
    return 0;
}

void llgo_osal_boot_install(void) {
    static const char ok[] = "osal: syscall table installed\n";
    for (unsigned i = 0; i < 42; i++) {
        s_rom_syscall_table[i] = (void *)llgo_osal_rom_slot_miss;
    }
    /* Slot offsets read off the ROM stub disassembly (each stub loads
     * syscall_table_ptr then jumps through its slot); routed to the matching
     * strong device-libc implementations so a stray ROM newlib call behaves
     * instead of faulting. Unrouted slots (assert helper, _printf_float,
     * _cleanup_r) stay on the logging catch-all so their callers show up as
     * text. */
    s_rom_syscall_table[0] = (void *)__getreent;   /* __getreent      0x00 */
    s_rom_syscall_table[1] = (void *)_malloc_r;    /* malloc          0x04 */
    s_rom_syscall_table[2] = (void *)_free_r;      /* _free_r         0x08 */
    s_rom_syscall_table[3] = (void *)_realloc_r;   /* _realloc_r      0x0c */
    s_rom_syscall_table[5] = (void *)abort;        /* abort           0x14 */
    s_rom_syscall_table[14] = (void *)_fstat_r;    /* _fstat_r        0x38 */
    s_rom_syscall_table[15] = (void *)_sbrk_r;     /* _sbrk_r         0x3c */
    s_rom_syscall_table[29] = (void *)osal_lock_noop; /* acquire_recursive 0x74 */
    s_rom_syscall_table[33] = (void *)osal_lock_noop; /* release_recursive 0x84 */
    s_rom_syscall_table[37] = (void *)__sinit;     /* __sinit         0x94 */
    syscall_table_ptr = s_rom_syscall_table;
    /* The ROM code was built hard-float: even non-FP library entries carry
     * fsw/flw prologue saves (ROM setjmp does), and the boot state leaves
     * mstatus.FS off, so any of those executing traps illegal-instruction.
     * This core has an FPU - turn it on (FS = Initial, bits 14:13 = 01). */
    __asm__ volatile("csrs mstatus, %0" :: "r"(1u << 14) : "memory");
    write(1, ok, sizeof(ok) - 1);
}

/* Run from crt0's __libc_init_array (the same hook the device file uses for
 * its WDT/trap setup) - before main and before Go package inits, i.e. before
 * anything can reach a ROM newlib entry. */
__attribute__((constructor)) static void osal_boot_early(void) {
    static const char hi[] = "osal: constructor\n";
    write(1, hi, sizeof(hi) - 1);
    llgo_osal_boot_install();
}

/* Goroutine trampoline for _task_create: enter a library task body through
 * its raw C function pointer (espradio's espradio_run_task shape; the Go
 * trampoline calls this from its goroutine). */
void llgo_osal_run_task(void (*fn)(void *), void *arg) {
    fn(arg);
}

/* ets_timer callback entry: void (*)(void *). */
void llgo_osal_call_void_ptrptr(void (*fn)(void *), void *arg) {
    fn(arg);
}

/* esp_event handler entry: void (*)(void *arg, esp_event_base_t base,
 * int32_t id, void *data). */
void llgo_osal_call_event_handler(void *fn, void *arg, const char *base, int32_t id, void *data) {
    ((void (*)(void *, const char *, int32_t, void *))fn)(arg, base, id, data);
}

/* hexstr2bin: the blob references it directly (wpa_supplicant's converter;
 * llgo's newlib does not carry it). Returns bytes written, -1 on odd input
 * length, matching the upstream contract closely enough for the library's
 * uses (IE parsing). */
static int osal_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hexstr2bin(const char *hex, uint8_t *buf, size_t len) {
    size_t i;
    for (i = 0; i < len * 2; i += 2) {
        int a = osal_hexval(hex[i]);
        int b = osal_hexval(hex[i + 1]);
        if (a < 0 || b < 0) {
            return -1;
        }
        buf[i / 2] = (uint8_t)((a << 4) | b);
    }
    return (int)(len * 2);
}

/* Library debug printfs (lib_printf.c equivalents): straight console
 * passthrough - the wlog level filtering is the blob's own business. */
/* One shared body: vsnprintf into a stack buffer, one write. */
#include <unistd.h>
uint32_t llgo_osal_log_timestamp(void);
static int osal_lib_printf(const char *format, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), format, args);
    if (n > 0) {
        if ((size_t)n >= sizeof(buf)) {
            n = (int)sizeof(buf) - 1;
        }
        write(1, buf, (size_t)n);
    }
    return n;
}

int pp_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = osal_lib_printf(format, args);
    va_end(args);
    return n;
}

int net80211_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = osal_lib_printf(format, args);
    va_end(args);
    return n;
}

int sc_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = osal_lib_printf(format, args);
    va_end(args);
    return n;
}

int mesh_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = osal_lib_printf(format, args);
    va_end(args);
    return n;
}

/* phy_printf: libphy's logging entry (referenced by register_chipv7_phy and
 * friends once the PHY calibration member joins the link). */
int phy_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = osal_lib_printf(format, args);
    va_end(args);
    return n;
}

/* coexist_printf: libcoexist's logging entry. */
int coexist_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = osal_lib_printf(format, args);
    va_end(args);
    return n;
}

/* rtc_clk_xtal_freq_get: the hal clock query register_chipv7_phy needs.
 * Routes to the ROM's ets_clk_get_xtal_freq, which reads the real clock
 * config (the emu models those registers). */
extern unsigned int ets_clk_get_xtal_freq(void);
unsigned int rtc_clk_xtal_freq_get(void) {
    return ets_clk_get_xtal_freq();
}

/* ---- log ---- */

/* Library debug printfs, redirected to the llgo console. Everything stays
 * in C: Go exports cannot be variadic, and the output rides the device's
 * retargeted write() (fd 1) rather than stdio FILE* machinery, whose
 * defining newlib member is not pulled into this link (stdout stays weakly
 * undefined; printf would dereference it).
 */
#include <unistd.h>

static void osal_console_line(const char *tag, const char *body, unsigned ts) {
    char buf[288];
    int n = snprintf(buf, sizeof(buf), "[%u][%s] %s\n", ts, tag ? tag : "?", body);
    if (n > 0) {
        if ((size_t)n >= sizeof(buf)) {
            n = (int)sizeof(buf) - 1;
        }
        write(1, buf, (size_t)n);
    }
}

void llgo_osal_log_write(unsigned int level, const char *tag, const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    osal_console_line(tag, buf, (unsigned)llgo_osal_log_timestamp());
}

void llgo_osal_log_writev(unsigned int level, const char *tag, const char *format, va_list args) {
    char buf[256];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(buf, sizeof(buf), format, copy);
    va_end(copy);
    osal_console_line(tag, buf, (unsigned)llgo_osal_log_timestamp());
}

/* Stub observability: one console line per distinct stub name, then silent
 * counters, so bring-up sees which unimplemented paths the blob touches
 * without flooding the log.
 */
void llgo_osal_note_stub(const char *what) {
    static const char *s_seen[16];
    static int s_count[16];
    static int s_n;
    char buf[64];
    for (int i = 0; i < s_n; i++) {
        if (s_seen[i] == what) {
            s_count[i]++;
            if (s_count[i] == 100 || s_count[i] == 10000) {
                snprintf(buf, sizeof(buf), "[osal-stub] %s x%d\n", what, s_count[i]);
                write(1, buf, strlen(buf));
            }
            return;
        }
    }
    if (s_n < 16) {
        s_seen[s_n] = what;
        s_count[s_n] = 1;
        s_n++;
    }
    snprintf(buf, sizeof(buf), "[osal-stub] %s\n", what);
    write(1, buf, strlen(buf));
}

uint32_t llgo_osal_log_timestamp(void) {
    extern int64_t llgo_osal_esp_timer_get_time(void);
    return (uint32_t)(llgo_osal_esp_timer_get_time() / 1000);
}

/* ---- memory: newlib passthroughs ---- */

void *llgo_osal_malloc(size_t size) { return malloc(size); }
void llgo_osal_free(void *p) { free(p); }
void *llgo_osal_malloc_internal(size_t size) { return malloc(size); }
void *llgo_osal_realloc_internal(void *p, size_t size) { return realloc(p, size); }
void *llgo_osal_calloc_internal(size_t n, size_t size) { return calloc(n, size); }
void *llgo_osal_zalloc_internal(size_t size) { return calloc(1, size); }
void *llgo_osal_wifi_malloc(size_t size) { return malloc(size); }
void *llgo_osal_wifi_realloc(void *p, size_t size) { return realloc(p, size); }
void *llgo_osal_wifi_calloc(size_t n, size_t size) { return calloc(n, size); }
void *llgo_osal_wifi_zalloc(size_t size) { return calloc(1, size); }

/* _get_free_heap_size: newlib heap view (blob uses it for flow control). */
uint32_t llgo_osal_get_free_heap_size(void) {
    struct mallinfo mi = mallinfo();
    return (uint32_t)mi.fordblks;
}

/* ---- time ---- */

/* _esp_timer_get_time: 64-bit microseconds from the cycle CSR (240 MHz, the
 * same nominal frequency the llgo esp32s31 clock gate assumes). The 32-bit
 * cycle counter wraps every ~17.9 s; the wrap folds into a base accumulator
 * whenever this function observes the counter go backwards, so callers must
 * hit it at least once per wrap period - the blob calls it at timer and
 * poll granularity, far above that. (A call exactly between the wrap and the
 * next observation would be off by one wrap; accepted for P1.) */
int64_t llgo_osal_esp_timer_get_time(void) {
    static uint32_t s_last_lo;
    static int64_t s_base_us;
    uint32_t lo;
    __asm__ volatile ("csrr %0, cycle" : "=r"(lo));
    if (lo < s_last_lo) {
        s_base_us += (int64_t)4294967296u * 100 / 24;
    }
    s_last_lo = lo;
    return s_base_us + (int64_t)lo * 100 / 24; /* 240 MHz -> us */
}

/* _get_time: wall-clock seconds; no RTC on the baremetal target, so 0. The
 * only caller (net80211) uses it for timestamps in optional logs. */
int llgo_osal_get_time(void *t) {
    (void)t;
    return 0;
}

/* _slowclk_cal_get: RTC-slow-clock calibration (light-sleep timing). No
 * light sleep on P1: 0, the value IDF produces before the first calibration.
 */
uint32_t llgo_osal_slowclk_cal_get(void) {
    return 0;
}

/* ---- random ---- */

/* TRNG data register (DR_REG_TRNG_BASE + 0x0, pinned IDF reg_base.h
 * 0x20814000). Each read yields entropy once the TRNG clock is on; the WiFi
 * stack calls these around PHY enable, whose clock gates ride the PHY port
 * (see the phy_enable stub note). The emu synthesizes TRNG data.
 */
#define OSAL_REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

uint32_t llgo_osal_rand(void) {
    return OSAL_REG32(0x20814000u);
}

unsigned long llgo_osal_random(void) {
    return llgo_osal_rand();
}

int llgo_osal_get_random(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = llgo_osal_rand();
        for (size_t j = 0; j < 4 && i + j < len; j++) {
            buf[i + j] = (uint8_t)(w >> (8 * j));
        }
    }
    return 0;
}

/* ---- MAC ---- */

/* _read_mac: P1 uses a fixed locally-administered base MAC. The efuse read
 * (esp_efuse_mac_get_default path in IDF) needs the efuse component's block
 * machinery; the blob and the emu accept any unicast MAC, and the demo
 * prints what it uses. Types follow esp_mac_type_t: 0/1 = base/STA, 2 = AP
 * (locally-administered flip), 3+ fall back to base.
 */
static const uint8_t s_osal_base_mac[6] = {0x42, 0xf0, 0x07, 0x31, 0x00, 0x01};

int llgo_osal_read_mac(uint8_t *mac, unsigned int type) {
    if (mac == 0) {
        return -1;
    }
    memcpy(mac, s_osal_base_mac, 6);
    if (type == 2) { /* ESP_MAC_WIFI_SOFTAP */
        mac[5] |= 0x40;
    }
    return 0;
}

/* ---- PHY / modem clock / low power ---- */

/* _phy_enable drives esp_phy_enable reduced to the hardware-independent
 * core (phy_init.c): param table install + first-time full RF calibration
 * via register_chipv7_phy, then phy_wakeup_init on later enables. The emu
 * models the RF/I2C hardware the ROM PHY code drives, so the real
 * calibration flow runs against it. No PMU/REGDMA/sleep-retention branches
 * (all off in this sdkconfig), and no modem clock gating (emu clocks are
 * always on). */
extern void llgo_osal_note_stub(const char *what);

extern int register_chipv7_phy(const esp_phy_init_data_t *init_data,
                               esp_phy_calibration_data_t *cal_data,
                               esp_phy_calibration_mode_t mode);
extern void phy_wakeup_init(void);

static bool s_phy_calibrated;

void llgo_osal_phy_enable(void) {
    phy_init_param_set(1);
    if (!s_phy_calibrated) {
        /* No NVS (P1 stubs): no stored calibration data -> full calibration,
         * exactly IDF's first-boot path. The cal record is dropped instead of
         * persisted; every boot recalibrates. */
        esp_phy_calibration_data_t *cal = calloc(1, sizeof(esp_phy_calibration_data_t));
        if (cal != 0) {
            register_chipv7_phy(&phy_init_data, cal, PHY_RF_CAL_FULL);
            free(cal);
        }
        s_phy_calibrated = true;
    } else {
        phy_wakeup_init();
    }
}

void llgo_osal_phy_disable(void) {
    llgo_osal_note_stub("phy_disable");
}

int llgo_osal_phy_update_country_info(const char *country) {
    (void)country;
    llgo_osal_note_stub("phy_update_country_info");
    return 1; /* IDF: true unless the power table update failed */
}

void llgo_osal_wifi_reset_mac(void) {
    llgo_osal_note_stub("wifi_reset_mac");
}

/* wifi_clock_enable = periph_ctrl wifi_module_enable reduced to the modem
 * clock bring-up, mirrored from the working IDF firmware's idle state (gdb
 * dump, artifacts wifi-osal/logs/idf-gdb-dump.txt):
 *   - HP_SYS_CLKRST.modem_conf: apb+core clk on, out of reset;
 *   - MODEM_SYSCON.clk_conf_force_on (+0x0c): the wifi/bt/fe keep-clock
 *     bytes IDF leaves at 0x64 each;
 *   - MODEM_SYSCON.clk_conf_power_st maps (+0x14): reserved byte = 0xff,
 *     zb/fe/bt/wifi icg maps 0x7/0xe/0x7/0xf, modem_peri 0x3.
 * Without the MODEM_SYSCON gates the wifi MAC block gets no clock and never
 * asserts its interrupt (the first scan attempt hung exactly there). */
#define OSAL_MODEMSYS_BASE 0x20109C00u /* MODEM_BASE(0x20100000)+0x9c00 */

void llgo_osal_wifi_clock_enable(void) {
    uint32_t conf = OSAL_REG32(0x20587000u + 0x1e0u);
    conf |= (1u << 0) | (1u << 2); /* modem_apb_clk_en | modem_clk_en */
    conf &= ~(1u << 1);            /* modem_rst_en = not-in-reset */
    OSAL_REG32(0x20587000u + 0x1e0u) = conf;
    OSAL_REG32(OSAL_MODEMSYS_BASE + 0x0cu) |= 0x64646400u;
    OSAL_REG32(OSAL_MODEMSYS_BASE + 0x14u) |= 0x0038e7ffu;
}

void llgo_osal_wifi_clock_disable(void) {
    llgo_osal_note_stub("wifi_clock_disable");
}

void llgo_osal_wifi_rtc_enable_iso(void) {
    /* S31 IDF glue: empty function */
}

void llgo_osal_wifi_rtc_disable_iso(void) {
    /* S31 IDF glue: empty function */
}

/* _wifi_pm_sleep_lock_acquire/release: CONFIG_PM only; stubs (IDF returns
 * ESP_OK-shaped no-ops with PM off).
 */
void llgo_osal_wifi_pm_sleep_lock_acquire(void) {}
void llgo_osal_wifi_pm_sleep_lock_release(void) {}

/* ---- NVS: stubs that fail like a missing NVS partition ---- */

/* PHY calibration data is not persisted on P1: every boot recalibrates (the
 * blob treats nvs failures as "no stored data" and uses defaults). Wired to
 * the llgo NVS port only when a demo needs persisted calibration.
 */
#define OSAL_NVS_ERR 1 /* ESP_ERR_NVS_NOT_FOUND-ish: blob only checks success */

int llgo_osal_nvs_set_i8(uint32_t h, const char *k, int8_t v) { (void)h; (void)k; (void)v; return OSAL_NVS_ERR; }
int llgo_osal_nvs_get_i8(uint32_t h, const char *k, int8_t *v) { (void)h; (void)k; (void)v; return OSAL_NVS_ERR; }
int llgo_osal_nvs_set_u8(uint32_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return OSAL_NVS_ERR; }
int llgo_osal_nvs_get_u8(uint32_t h, const char *k, uint8_t *v) { (void)h; (void)k; (void)v; return OSAL_NVS_ERR; }
int llgo_osal_nvs_set_u16(uint32_t h, const char *k, uint16_t v) { (void)h; (void)k; (void)v; return OSAL_NVS_ERR; }
int llgo_osal_nvs_get_u16(uint32_t h, const char *k, uint16_t *v) { (void)h; (void)k; (void)v; return OSAL_NVS_ERR; }
/* nvs_open must SUCCEED even with no storage behind it: wifi_init_process
 * aborts its attach chain (wifi_lmac_init and the scan-object setup the
 * beacon path dereferences) when wifi_nvs_init reports failure, and the
 * vendor IDF run only works because its NVS partition opens. Reads still
 * return NOT_FOUND so the blob falls back to defaults / full calibration;
 * writes succeed into the void (P1 does not persist). */
int llgo_osal_nvs_open(const char *name, unsigned int mode, uint32_t *out) {
    (void)name;
    (void)mode;
    *out = 1; /* fake handle */
    return 0;
}
void llgo_osal_nvs_close(uint32_t h) { (void)h; }
int llgo_osal_nvs_commit(uint32_t h) { (void)h; return OSAL_NVS_ERR; }
int llgo_osal_nvs_set_blob(uint32_t h, const char *k, const void *v, size_t l) { (void)h; (void)k; (void)v; (void)l; return OSAL_NVS_ERR; }
int llgo_osal_nvs_get_blob(uint32_t h, const char *k, void *v, size_t *l) { (void)h; (void)k; (void)v; (void)l; return OSAL_NVS_ERR; }
int llgo_osal_nvs_erase_key(uint32_t h, const char *k) { (void)h; (void)k; return OSAL_NVS_ERR; }

/* ---- COEX: the blob was built with CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
 * (pinned sdkconfig.h line 603), so these forward to the real libcoexist
 * scheduler instead of stubs - the wifi pm module registers ROM callbacks
 * that dereference the coex state, and stubbing init left that state NULL
 * (crash inside pm_coex_recalculate_wifi_time_slice on the first scan). */

#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"

/* The coex os-adapter table (the ESP-IDF esp_coex_adapter.c shape, every
 * function resolved to this OSAL's primitives). llgo does not run the IDF
 * ESP_SYSTEM_INIT_FN(init_coexist) constructor that registers it, so the
 * demo's esp_wifi_init does it by hand. */
static void osal_coex_task_yield_from_isr(void) { llgo_osal_task_yield_from_isr(); }
static void *osal_coex_semphr_create(uint32_t max, uint32_t init) { return llgo_osal_semphr_create(max, init); }
static void osal_coex_semphr_delete(void *sem) { llgo_osal_semphr_delete(sem); }
static int32_t osal_coex_semphr_take_from_isr(void *sem, void *hptw) { return llgo_osal_queue_send_from_isr(sem, hptw, 0) ? 1 : 0; }
static int32_t osal_coex_semphr_give_from_isr(void *sem, void *hptw) { (void)sem; (void)hptw; return 1; }
static int32_t osal_coex_semphr_take(void *sem, uint32_t tick) { return llgo_osal_semphr_take(sem, tick); }
static int32_t osal_coex_semphr_give(void *sem) { return llgo_osal_semphr_give(sem); }
static int osal_coex_is_in_isr(void) { return llgo_osal_is_from_isr_context(); }
static void *osal_coex_malloc_internal(size_t size) { return malloc(size); }
static void osal_coex_free(void *p) { free(p); }
static int64_t osal_coex_esp_timer_get_time(void) { return llgo_osal_esp_timer_get_time(); }
static bool osal_coex_env_is_chip(void) { return llgo_osal_env_is_chip(); }
static void osal_coex_timer_disarm(void *t) { llgo_osal_timer_disarm(t); }
static void osal_coex_timer_done(void *t) { llgo_osal_timer_done(t); }
static void osal_coex_timer_setfn(void *t, void *fn, void *arg) { llgo_osal_timer_setfn(t, fn, arg); }
static void osal_coex_timer_arm_us(void *t, uint32_t us, bool repeat) { llgo_osal_timer_arm_us(t, us, repeat ? 1 : 0); }
static int osal_coex_debug_matrix_init(int event, int signal, bool rev) { (void)event; (void)signal; (void)rev; return 0; }
static int osal_coex_xtal_freq_get(void) { return (int)rtc_clk_xtal_freq_get(); }

coex_adapter_funcs_t g_coex_adapter_funcs = {
    ._version = COEX_ADAPTER_VERSION,
    ._task_yield_from_isr = osal_coex_task_yield_from_isr,
    ._semphr_create = osal_coex_semphr_create,
    ._semphr_delete = osal_coex_semphr_delete,
    ._semphr_take_from_isr = osal_coex_semphr_take_from_isr,
    ._semphr_give_from_isr = osal_coex_semphr_give_from_isr,
    ._semphr_take = osal_coex_semphr_take,
    ._semphr_give = osal_coex_semphr_give,
    ._is_in_isr = osal_coex_is_in_isr,
    ._malloc_internal = osal_coex_malloc_internal,
    ._free = osal_coex_free,
    ._esp_timer_get_time = osal_coex_esp_timer_get_time,
    ._env_is_chip = osal_coex_env_is_chip,
    ._timer_disarm = osal_coex_timer_disarm,
    ._timer_done = osal_coex_timer_done,
    ._timer_setfn = osal_coex_timer_setfn,
    ._timer_arm_us = osal_coex_timer_arm_us,
    ._debug_matrix_init = osal_coex_debug_matrix_init,
    ._xtal_freq_get = osal_coex_xtal_freq_get,
    ._magic = COEX_ADAPTER_MAGIC,
};

extern int coex_init(void);
extern void coex_disable(void);
extern int coex_enable(void);

extern void coex_condition_set(uint32_t type, bool dissatisfy);
extern int coex_wifi_request(uint32_t event, uint32_t latency, uint32_t duration);
extern int coex_wifi_release(uint32_t event);
extern int coex_wifi_channel_set(uint8_t primary, uint8_t secondary);
extern int coex_event_duration_get(uint32_t event, uint32_t *duration);
extern int coex_pti_get(uint32_t event, uint8_t *pti);
extern void coex_schm_status_bit_clear(uint32_t type, uint32_t status);
extern void coex_schm_status_bit_set(uint32_t type, uint32_t status);
extern int coex_schm_interval_set(uint32_t interval);
extern uint32_t coex_schm_interval_get(void);
extern uint8_t coex_schm_curr_period_get(void);
extern void *coex_schm_curr_phase_get(void);
extern int coex_schm_process_restart(void);

extern int coex_register_start_cb(int (*cb)(void));
extern int coex_schm_flexible_period_set(uint8_t period);
extern uint8_t coex_schm_flexible_period_get(void);
extern void *coex_schm_get_phase_by_idx(int idx);
/* coex_condition_set / coex_configure_preemption_end_cb exist only with a
 * BT side present; the WiFi-only P1 build has no caller that observes them,
 * so they stay local no-ops. */
void coex_condition_set(uint32_t type, bool dissatisfy) { (void)type; (void)dissatisfy; }
int coex_configure_preemption_end_cb(bool is_register, int (*cb)(uint32_t)) { (void)is_register; (void)cb; return 0; }

int llgo_osal_coex_init(void) { return coex_init(); }
void llgo_osal_coex_deinit(void) { coex_disable(); }
int llgo_osal_coex_enable(void) { return coex_enable(); }
void llgo_osal_coex_disable(void) { coex_disable(); }
uint32_t llgo_osal_coex_status_get(void) { return coex_status_get(1); } /* bitmap=1: wifi */
void llgo_osal_coex_condition_set(uint32_t type, bool dissatisfy) { coex_condition_set(type, dissatisfy); }
int llgo_osal_coex_wifi_request(uint32_t event, uint32_t latency, uint32_t duration) { return coex_wifi_request(event, latency, duration); }
int llgo_osal_coex_wifi_release(uint32_t event) { return coex_wifi_release(event); }
int llgo_osal_coex_wifi_channel_set(uint8_t primary, uint8_t secondary) { return coex_wifi_channel_set(primary, secondary); }
int llgo_osal_coex_event_duration_get(uint32_t event, uint32_t *duration) { return coex_event_duration_get(event, duration); }
int llgo_osal_coex_pti_get(uint32_t event, uint8_t *pti) { return coex_pti_get(event, pti); }
void llgo_osal_coex_schm_status_bit_clear(uint32_t type, uint32_t status) { coex_schm_status_bit_clear(type, status); }
void llgo_osal_coex_schm_status_bit_set(uint32_t type, uint32_t status) { coex_schm_status_bit_set(type, status); }
int llgo_osal_coex_schm_interval_set(uint32_t interval) { return coex_schm_interval_set(interval); }
uint32_t llgo_osal_coex_schm_interval_get(void) { return coex_schm_interval_get(); }
uint8_t llgo_osal_coex_schm_curr_period_get(void) { return coex_schm_curr_period_get(); }
void *llgo_osal_coex_schm_curr_phase_get(void) { return coex_schm_curr_phase_get(); }
int llgo_osal_coex_schm_process_restart(void) { return coex_schm_process_restart(); }
int llgo_osal_coex_schm_register_cb(int kind, int (*cb)(int)) { return coex_schm_register_callback((coex_schm_callback_type_t)kind, (void *)cb); }
int llgo_osal_coex_register_start_cb(int (*cb)(void)) { return coex_register_start_cb(cb); }
int llgo_osal_coex_schm_flexible_period_set(uint8_t period) { return coex_schm_flexible_period_set(period); }
uint8_t llgo_osal_coex_schm_flexible_period_get(void) { return coex_schm_flexible_period_get(); }
void *llgo_osal_coex_schm_get_phase_by_idx(int idx) { return coex_schm_get_phase_by_idx(idx); }
int llgo_osal_coex_configure_preemption_end_cb(bool is_register, int (*cb)(uint32_t)) { return coex_configure_preemption_end_cb(is_register, cb); }

/* ---- sleep retention (S31 REGDMA): modem power management; P1 keeps the
 * modem always on, so every attach/detach reports "skipped" (the IDF
 * CONFIG_MAC_BB_PD=n branch returns 1) ---- */

void llgo_osal_regdma_link_set_write_wait_content(void *a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void *llgo_osal_sleep_retention_find_link_by_id(int id) { (void)id; return 0; }
int32_t llgo_osal_wifi_bb_sleep_retention_attach(void) { return 1; }
int32_t llgo_osal_wifi_bb_sleep_retention_detach(void) { return 1; }
int32_t llgo_osal_wifi_mac_sleep_retention_attach(void) { return 1; }
int32_t llgo_osal_wifi_mac_sleep_retention_detach(void) { return 1; }

/* ---- misc ---- */

void llgo_osal_dport_start_wrap(void) {}
void llgo_osal_dport_end_wrap(void) {}

/* _wifi_disable_ac_ax: HE chips; S31 IDF glue returns false (do not
 * disable). */
bool llgo_osal_wifi_disable_ac_ax(void) { return false; }

/* ---- the table ---- */

wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = (bool (*)(void))llgo_osal_env_is_chip,
    ._set_intr = llgo_osal_set_intr,
    ._clear_intr = llgo_osal_clear_intr,
    ._set_isr = llgo_osal_set_isr,
    ._ints_on = llgo_osal_ints_on,
    ._ints_off = llgo_osal_ints_off,
    ._is_from_isr = (bool (*)(void))llgo_osal_is_from_isr_context,
    ._spin_lock_create = llgo_osal_spin_lock_create,
    ._spin_lock_delete = llgo_osal_spin_lock_delete,
    ._wifi_int_disable = llgo_osal_wifi_int_disable,
    ._wifi_int_restore = llgo_osal_wifi_int_restore,
    ._task_yield_from_isr = llgo_osal_task_yield_from_isr,
    ._semphr_create = (void *(*)(uint32_t, uint32_t))llgo_osal_semphr_create,
    ._semphr_delete = llgo_osal_semphr_delete,
    ._semphr_take = llgo_osal_semphr_take,
    ._semphr_give = llgo_osal_semphr_give,
    ._wifi_thread_semphr_get = (void *(*)(void))llgo_osal_wifi_thread_semphr_get,
    ._mutex_create = (void *(*)(void))llgo_osal_mutex_create,
    ._recursive_mutex_create = (void *(*)(void))llgo_osal_recursive_mutex_create,
    ._mutex_delete = llgo_osal_mutex_delete,
    ._mutex_lock = osal_mutex_lock_c,
    ._mutex_unlock = osal_mutex_unlock_c,
    ._queue_create = (void *(*)(uint32_t, uint32_t))llgo_osal_queue_create,
    ._queue_delete = llgo_osal_queue_delete,
    ._queue_send = llgo_osal_queue_send,
    ._queue_send_from_isr = llgo_osal_queue_send_from_isr,
    ._queue_send_to_back = llgo_osal_queue_send_to_back,
    ._queue_send_to_front = llgo_osal_queue_send_to_front,
    ._queue_recv = llgo_osal_queue_recv_wrap,
    ._queue_msg_waiting = llgo_osal_queue_msg_waiting,
    ._event_group_create = (void *(*)(void))llgo_osal_event_group_create,
    ._event_group_delete = llgo_osal_event_group_delete,
    ._event_group_set_bits = llgo_osal_event_group_set_bits,
    ._event_group_clear_bits = llgo_osal_event_group_clear_bits,
    ._event_group_wait_bits = llgo_osal_event_group_wait_bits,
    ._task_create_pinned_to_core = llgo_osal_task_create_pinned_to_core,
    ._task_create = llgo_osal_task_create,
    ._task_delete = llgo_osal_task_delete,
    ._task_delay = llgo_osal_task_delay,
    ._task_ms_to_tick = llgo_osal_task_ms_to_tick,
    ._task_get_current_task = (void *(*)(void))llgo_osal_task_get_current_task,
    ._task_get_max_priority = llgo_osal_task_get_max_priority,
    ._malloc = (void *(*)(size_t))llgo_osal_malloc,
    ._free = llgo_osal_free,
    ._event_post = (int32_t (*)(const char *, int32_t, void *, size_t, uint32_t))llgo_osal_event_post,
    ._get_free_heap_size = llgo_osal_get_free_heap_size,
    ._rand = llgo_osal_rand,
    ._dport_access_stall_other_cpu_start_wrap = llgo_osal_dport_start_wrap,
    ._dport_access_stall_other_cpu_end_wrap = llgo_osal_dport_end_wrap,
    ._wifi_pm_sleep_lock_acquire = llgo_osal_wifi_pm_sleep_lock_acquire,
    ._wifi_pm_sleep_lock_release = llgo_osal_wifi_pm_sleep_lock_release,
    ._phy_disable = llgo_osal_phy_disable,
    ._phy_enable = llgo_osal_phy_enable,
    ._phy_update_country_info = llgo_osal_phy_update_country_info,
    ._read_mac = llgo_osal_read_mac,
    ._timer_arm = osal_timer_arm_c,
    ._timer_disarm = llgo_osal_timer_disarm,
    ._timer_done = llgo_osal_timer_done,
    ._timer_setfn = llgo_osal_timer_setfn,
    ._timer_arm_us = osal_timer_arm_us_c,
    ._wifi_reset_mac = llgo_osal_wifi_reset_mac,
    ._wifi_clock_enable = llgo_osal_wifi_clock_enable,
    ._wifi_clock_disable = llgo_osal_wifi_clock_disable,
    ._wifi_rtc_enable_iso = llgo_osal_wifi_rtc_enable_iso,
    ._wifi_rtc_disable_iso = llgo_osal_wifi_rtc_disable_iso,
    ._esp_timer_get_time = llgo_osal_esp_timer_get_time,
    ._nvs_set_i8 = llgo_osal_nvs_set_i8,
    ._nvs_get_i8 = llgo_osal_nvs_get_i8,
    ._nvs_set_u8 = llgo_osal_nvs_set_u8,
    ._nvs_get_u8 = llgo_osal_nvs_get_u8,
    ._nvs_set_u16 = llgo_osal_nvs_set_u16,
    ._nvs_get_u16 = llgo_osal_nvs_get_u16,
    ._nvs_open = llgo_osal_nvs_open,
    ._nvs_close = llgo_osal_nvs_close,
    ._nvs_commit = llgo_osal_nvs_commit,
    ._nvs_set_blob = llgo_osal_nvs_set_blob,
    ._nvs_get_blob = llgo_osal_nvs_get_blob,
    ._nvs_erase_key = llgo_osal_nvs_erase_key,
    ._get_random = llgo_osal_get_random,
    ._get_time = llgo_osal_get_time,
    ._random = (unsigned long (*)(void))llgo_osal_random,
    ._slowclk_cal_get = llgo_osal_slowclk_cal_get,
    ._log_write = llgo_osal_log_write,
    ._log_writev = llgo_osal_log_writev,
    ._log_timestamp = llgo_osal_log_timestamp,
    ._malloc_internal = (void *(*)(size_t))llgo_osal_malloc_internal,
    ._realloc_internal = (void *(*)(void *, size_t))llgo_osal_realloc_internal,
    ._calloc_internal = (void *(*)(size_t, size_t))llgo_osal_calloc_internal,
    ._zalloc_internal = (void *(*)(size_t))llgo_osal_zalloc_internal,
    ._wifi_malloc = (void *(*)(size_t))llgo_osal_wifi_malloc,
    ._wifi_realloc = (void *(*)(void *, size_t))llgo_osal_wifi_realloc,
    ._wifi_calloc = (void *(*)(size_t, size_t))llgo_osal_wifi_calloc,
    ._wifi_zalloc = (void *(*)(size_t))llgo_osal_wifi_zalloc,
    ._wifi_create_queue = (void *(*)(int, int))llgo_osal_wifi_create_queue,
    ._wifi_delete_queue = llgo_osal_wifi_delete_queue,
    ._coex_init = llgo_osal_coex_init,
    ._coex_deinit = llgo_osal_coex_deinit,
    ._coex_enable = llgo_osal_coex_enable,
    ._coex_disable = llgo_osal_coex_disable,
    ._coex_status_get = llgo_osal_coex_status_get,
    ._coex_condition_set = llgo_osal_coex_condition_set,
    ._coex_wifi_request = llgo_osal_coex_wifi_request,
    ._coex_wifi_release = llgo_osal_coex_wifi_release,
    ._coex_wifi_channel_set = llgo_osal_coex_wifi_channel_set,
    ._coex_event_duration_get = llgo_osal_coex_event_duration_get,
    ._coex_pti_get = llgo_osal_coex_pti_get,
    ._coex_schm_status_bit_clear = llgo_osal_coex_schm_status_bit_clear,
    ._coex_schm_status_bit_set = llgo_osal_coex_schm_status_bit_set,
    ._coex_schm_interval_set = llgo_osal_coex_schm_interval_set,
    ._coex_schm_interval_get = llgo_osal_coex_schm_interval_get,
    ._coex_schm_curr_period_get = llgo_osal_coex_schm_curr_period_get,
    ._coex_schm_curr_phase_get = llgo_osal_coex_schm_curr_phase_get,
    ._coex_schm_process_restart = llgo_osal_coex_schm_process_restart,
    ._coex_schm_register_cb = llgo_osal_coex_schm_register_cb,
    ._coex_register_start_cb = llgo_osal_coex_register_start_cb,
    ._regdma_link_set_write_wait_content = llgo_osal_regdma_link_set_write_wait_content,
    ._sleep_retention_find_link_by_id = llgo_osal_sleep_retention_find_link_by_id,
    ._coex_schm_flexible_period_set = llgo_osal_coex_schm_flexible_period_set,
    ._coex_schm_flexible_period_get = llgo_osal_coex_schm_flexible_period_get,
    ._coex_schm_get_phase_by_idx = llgo_osal_coex_schm_get_phase_by_idx,
    ._coex_configure_preemption_end_cb = llgo_osal_coex_configure_preemption_end_cb,
    ._wifi_disable_ac_ax = (bool (*)(void))llgo_osal_wifi_disable_ac_ax,
    ._wifi_bb_sleep_retention_attach = llgo_osal_wifi_bb_sleep_retention_attach,
    ._wifi_bb_sleep_retention_detach = llgo_osal_wifi_bb_sleep_retention_detach,
    ._wifi_mac_sleep_retention_attach = llgo_osal_wifi_mac_sleep_retention_attach,
    ._wifi_mac_sleep_retention_detach = llgo_osal_wifi_mac_sleep_retention_detach,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};

/* Minimal RISC-V CLIC interrupt framework for the S31 WiFi OSAL.
 *
 * The closed-source WiFi stack installs its own interrupts:
 *
 *   ic_set_interrupt_handler()   _set_intr(cpu, 120/122, num=1, prio=1)
 *                                _set_isr(1, wDev_ProcessFiq, NULL)
 *                                _ints_on(0b10)
 *   ic_bb_check_noise_init()     _set_isr(9, bb_intr_handl, NULL)
 *                                _ints_on(0x200)
 *
 * (offsets/source ids verified in the P0 survey, SYMBOLS.md section 5.1).
 * This file provides just enough interrupt machinery for that to work on the
 * llgo baremetal runtime:
 *
 *   - an mtvec (direct mode) trap entry that saves the caller-saved integer
 *     registers, dispatches pending CLIC interrupts to the registered
 *     handler, and chains every exception to the trap handler that was
 *     installed at boot (targets/device/esp/esp32s31.c's chained exit trap,
 *     so Go's EBREAK exits keep working);
 *   - source -> CPU-int routing through the interrupt matrix;
 *   - per-interrupt CLIC enable/priority/trigger programming;
 *   - mintthresh-based interrupt masking for the OSAL critical sections.
 *
 * ISR safety rules for everything reachable from the trap entry:
 *   - integer registers only. Our OSAL C is compiled -march=rv32imc_zicsr,
 *     so it never touches the FPU; the blob's own MAC/BB ISR bodies were
 *     verified FP-free (P0 survey, SYMBOLS.md 6.2), and the ABI keeps
 *     callee-saved registers live across our dispatch. So no FP state needs
 *     saving even though the blob is an ilp32f build.
 *   - no scheduler access, no allocation, no blocking. The only bridge into
 *     the Go world is the ISR staging rings (see osi_osal.c): queue items
 *     staged from ISR context are drained by the task-side queue_recv, which
 *     parks in bounded slices so a pure ISR post still wakes the consumer
 *     within ~1 ms.
 *
 * Register constants below are transcribed from the pinned IDF (ad20ded62a2):
 *   - CLIC per-interrupt control word: soc/clic_reg.h (NLBITS=3, ctrl base
 *     0x10801000, CTL[31:24] / IE[8] / IP[0] / SHV[16] / TRIG[18:17]),
 *   - CLIC global: DR_REG_CLIC_BASE=0x10800000,
 *   - interrupt matrix: DR_REG_INTR0_BASE=0x20585000, route entry
 *     *(base + 4*source) holds the target CPU interrupt in bits [5:0],
 *   - external interrupts sit at CLIC lines 16..47 (CLIC_EXT_INTR_NUM_OFFSET
 *     = 16; the blob's _set_intr numbers are 0-based CPU interrupts),
 *   - CSRs: mtvt=0x307, mintthresh=0x347, mintstatus=0xFB1 (standard CLIC
 *     layout per csr_clic.h INTTHRESH_STANDARD).
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>
#include <stdbool.h>

#define OSAL_CLIC_CTRL_BASE   0x10801000u

/* S31 interrupt routing goes through the ROM's intr_matrix_set (the chip's
 * matrix layout is ROM-private); no local register base is needed. */
extern void intr_matrix_set(int cpu_no, uint32_t model_num, uint32_t intr_num);
extern void esprv_intc_int_set_priority(uint32_t intr_num, uint32_t prio);
extern void esprv_intc_int_set_type(uint32_t intr_num, uint32_t type);
extern void esprv_intc_int_enable(uint32_t mask);
extern void esprv_intc_int_disable(uint32_t mask);
extern void intr_handler_set(uint32_t int_num, void *fn, void *arg);
/* The ROM's common interrupt entry (dispatches the intr_handler_set table).
 * The ROM keeps it at a fixed address; its linker script exports no name,
 * so pin the address (esp32s31.rom ELF symbol _interrupt_handler). */
#define LLGO_ROM_INTERRUPT_HANDLER 0x2f80185cu
/* S31 CLIC mapping (verified against the ROM's intr_matrix_set and
 * esprv_intc_int_enable disassembly): peripheral source src routed to CPU
 * int num lands on CLIC line num+0x10 (matrix word 0x20585000+4*src holds
 * num+0x10), esprv_intc_int_enable(mask) toggles the IE byte of CLIC line
 * n+0x10 for each mask bit n, and the trap's mcause carries the CLIC line
 * number. Handlers are indexed by line-OSAL_INT_OFFSET (the blob's num). */
#define OSAL_INT_OFFSET       16u
#define OSAL_INT_COUNT        32u /* routable CPU interrupt lines */

#define OSAL_REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* mintthresh/mintstatus access (standard CLIC CSRs). */
static inline uint32_t osal_rd_mintthresh(void) {
    uint32_t v;
    __asm__ volatile ("csrr %0, 0x347" : "=r"(v));
    return v;
}
static inline void osal_wr_mintthresh(uint32_t v) {
    __asm__ volatile ("csrw 0x347, %0" :: "r"(v));
}
static inline uint32_t osal_rd_mintstatus(void) {
    uint32_t v;
    __asm__ volatile ("csrr %0, 0xfb1" : "=r"(v));
    return v;
}

/* Interrupt level to CLIC threshold value (NLBITS=3: level in bits [7:5],
 * the low 5 bits are constant-1). Matches the ROM's
 * esprv_intc_int_set_threshold exactly (level<<5 | 0x1f); writing anything
 * else in the low bits leaves the line-level comparison inconsistent with
 * the vendor setup (observed as IP-pending lines never trapping). */
static inline uint32_t osal_thresh_byte(uint32_t level) {
    return (level << 5) | 0x1fu;
}

/* Level used by the OSAL critical section (RVHAL_EXCM_LEVEL-1 = 3): blocks
 * everything the WiFi stack can raise (both of its ints sit at priority 1).
 */
#define OSAL_CRIT_THRESH osal_thresh_byte(3)
/* Blocks everything (used inside the trap entry). */
#define OSAL_ALL_THRESH 0xffu

/* ---- handler table ---- */

typedef void (*osal_isr_fn)(void *arg);

typedef struct {
    osal_isr_fn fn;
    void *arg;
} osal_isr_entry_t;

static osal_isr_entry_t s_osal_handlers[OSAL_INT_COUNT];

/* The trap handler that was in mtvec when the OSAL initialized (the device
 * C boot trap, which chains further to the ROM). Exceptions re-enter it with
 * the original register state.
 */
/* Non-static: the naked entry's `la` targets it, and the integrated
 * assembler resolves global symbols more predictably than file-static ones
 * across the LTO boundary. */
uint32_t llgo_osal_prev_mtvec;
static bool s_osal_intr_ready;

/* Interrupt nesting depth: nonzero from trap entry until mret. The OSI
 * _is_from_isr reads this through llgo_osal_is_from_isr_context.
 */
volatile uint32_t llgo_osal_isr_nest;

/* Yield requests made from ISR context (_task_yield_from_isr). The
 * cooperative runtime has no preemption to act on - the woken task wakes via
 * its bounded park - so this is a counter for observability, mirroring
 * espradio's yield-traffic accounting.
 */
volatile uint32_t llgo_osal_yield_from_isr_count;

bool llgo_osal_is_from_isr_context(void) {
    return llgo_osal_isr_nest != 0;
}

/* The C dispatcher: called from the naked entry with all caller-saved
 * integer registers saved on the trap stack. Returns the trap disposition.
 * Never returns into Go code; the handler table holds C functions only.
 */
__attribute__((used)) void llgo_osal_trap_dispatch(uint32_t mcause) {
    if ((int32_t)mcause >= 0) {
        /* Exception: chain to the boot trap handler (preserves the EBREAK
         * exit path). The naked entry jumps there instead of returning. */
        return;
    }
    uint32_t line = mcause & 0x7fffffffu;
    if (line >= OSAL_INT_OFFSET && line - OSAL_INT_OFFSET < OSAL_INT_COUNT) {
        osal_isr_entry_t e = s_osal_handlers[line - OSAL_INT_OFFSET];
        if (e.fn != 0) {
            llgo_osal_isr_nest++; /* _is_from_isr true for the handler body */
            e.fn(e.arg);
            llgo_osal_isr_nest--;
        } else {
            /* Spurious or unhandled: leave the handler unset state visible
             * in a register-visible way instead of hanging silently. */
            llgo_osal_yield_from_isr_count += 0x10000;
        }
    }
}

/* Naked trap entry: mtvec (direct mode) lands here.
 *
 * Frame: ra + a0-a7 + t0-t6 (16 words, 64 bytes) above the interrupted sp;
 * everything else is callee-saved by contract. Exceptions jump through to
 * llgo_osal_prev_mtvec with registers restored; interrupts dispatch in C and
 * mret back.
 */
__attribute__((naked, aligned(64))) void llgo_osal_trap_entry(void) {
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"          /* direct-mode entry stays uncompressed */
        "addi sp, sp, -64\n"
        "sw ra, 0(sp)\n"
        "sw a0, 4(sp)\n"
        "sw a1, 8(sp)\n"
        "sw a2, 12(sp)\n"
        "sw a3, 16(sp)\n"
        "sw a4, 20(sp)\n"
        "sw a5, 24(sp)\n"
        "sw a6, 28(sp)\n"
        "sw a7, 32(sp)\n"
        "sw t0, 36(sp)\n"
        "sw t1, 40(sp)\n"
        "sw t2, 44(sp)\n"
        "sw t3, 48(sp)\n"
        "sw t4, 52(sp)\n"
        "sw t5, 56(sp)\n"
        "sw t6, 60(sp)\n"
        "csrr a0, mcause\n"
        /* mcause bit31 set (negative) = interrupt: falls through to the
         * dispatch; bit31 clear (>= 0) = exception: chains to the boot trap
         * at label 2. */
        "bgez a0, 2f\n"
        "call llgo_osal_trap_dispatch\n"
        /* interrupt path: full restore, mret */
        "lw ra, 0(sp)\n"
        "lw a0, 4(sp)\n"
        "lw a1, 8(sp)\n"
        "lw a2, 12(sp)\n"
        "lw a3, 16(sp)\n"
        "lw a4, 20(sp)\n"
        "lw a5, 24(sp)\n"
        "lw a6, 28(sp)\n"
        "lw a7, 32(sp)\n"
        "lw t0, 36(sp)\n"
        "lw t1, 40(sp)\n"
        "lw t2, 44(sp)\n"
        "lw t3, 48(sp)\n"
        "lw t4, 52(sp)\n"
        "lw t5, 56(sp)\n"
        "lw t6, 60(sp)\n"
        "addi sp, sp, 64\n"
        "mret\n"
        /* exception path: chain to the boot trap with registers restored.
         * The chain target rides in t0, loaded after every other restore
         * (the boot trap never resumes the interrupted frame, so the
         * original t0 does not need to survive). */
        "2: la t0, llgo_osal_prev_mtvec\n"
        "lw t0, 0(t0)\n"
        "lw ra, 0(sp)\n"
        "lw a0, 4(sp)\n"
        "lw a1, 8(sp)\n"
        "lw a2, 12(sp)\n"
        "lw a3, 16(sp)\n"
        "lw a4, 20(sp)\n"
        "lw a5, 24(sp)\n"
        "lw a6, 28(sp)\n"
        "lw a7, 32(sp)\n"
        "lw t1, 40(sp)\n"
        "lw t2, 44(sp)\n"
        "lw t3, 48(sp)\n"
        "lw t4, 52(sp)\n"
        "lw t5, 56(sp)\n"
        "lw t6, 60(sp)\n"
        "addi sp, sp, 64\n"
        "jr t0\n"
        ".option pop\n"
    );
}

/* OSAL initialization: install our trap and enable global interrupts.
 * Called once before any WiFi init (Go side), after which the CLIC, matrix
 * and handler table drive the blob's interrupt setup.
 */
static uint32_t s_osal_mtvt_table[OSAL_INT_COUNT + OSAL_INT_OFFSET];

void llgo_osal_intr_init(void) {
    /* The wifi lines run through the ROM's vectored interrupt framework,
     * mirrored from the working IDF firmware: the lines carry SHV=1, so the
     * CPU vectors through mtvt[line] into the ROM _interrupt_handler, which
     * dispatches the intr_handler_set table with the registered arg in a0.
     * The OSAL's own mtvec trap stays for the synchronous exception chain
     * (EBREAK exit) only - it is installed by the device boot constructor
     * and is not touched here. */
    s_osal_intr_ready = true;
    for (uint32_t i = 0; i < OSAL_INT_COUNT + OSAL_INT_OFFSET; i++) {
        s_osal_mtvt_table[i] = LLGO_ROM_INTERRUPT_HANDLER;
    }
    __asm__ volatile ("csrw 0x307, %0" :: "r"(&s_osal_mtvt_table[0]));
    osal_wr_mintthresh(osal_thresh_byte(0)); /* admit all levels */
    uint32_t mstatus;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    __asm__ volatile ("csrs mstatus, %0" :: "r"(1u << 3)); /* MIE */
}

/* ---- the OSI interrupt entries ---- */

/* _set_intr(cpu_no, intr_source, intr_num, intr_prio): route a peripheral
 * source to a CPU interrupt line and program its CLIC attributes.
 * Level-triggered (the blob's ints are level), priority 1 for both WiFi
 * lines (IDF adapter uses LEVEL type + the passed prio).
 */
void llgo_osal_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio) {
    (void)cpu_no; /* single-hart scheduler; the matrix write targets core 0 */
    if (intr_num >= OSAL_INT_COUNT) {
        return;
    }
    /* route + attrs through the ROM intc functions (the vendor semantics:
     * matrix word = num+0x10, CLIC line = num+0x10, priority in the top
     * byte as level<<5|0x1f, type LEVEL as the vendor's set_type variant
     * leaves it). */
    intr_matrix_set(0, intr_source, intr_num);
    esprv_intc_int_set_priority(intr_num, intr_prio & 7u);
    esprv_intc_int_set_type(intr_num, 1u /* INTR_TYPE_LEVEL */);
    /* the vendor firmware runs its lines vectored (CLICINTCTL bit16 = SHV,
     * verified in its idle-state dump); mirror it, else the pending line
     * waits on a direct-mode mtvec that never services interrupts. */
    uint32_t ctrl = OSAL_REG32(OSAL_CLIC_CTRL_BASE + 4u * (intr_num + OSAL_INT_OFFSET));
    ctrl |= (1u << 16);
    OSAL_REG32(OSAL_CLIC_CTRL_BASE + 4u * (intr_num + OSAL_INT_OFFSET)) = ctrl;
    s_osal_handlers[intr_num].fn = 0;
    s_osal_handlers[intr_num].arg = 0;
}

/* _clear_intr: the S31 IDF adapter is an empty function (the matrix keeps
 * its routing; the source deasserts when the peripheral is quiet). Kept as
 * a real (no-op) entry so a stray call stays harmless.
 */
void llgo_osal_clear_intr(uint32_t intr_source, uint32_t intr_num) {
    (void)intr_source;
    (void)intr_num;
}

/* _set_isr(n, f, arg): register the handler for CPU interrupt n. */
/* ROM-frame dispatch: _global_interrupt_handler calls fn(arg) straight from
 * the intr_handler_set table, outside any OSAL bookkeeping. The thunk keeps
 * the OSAL is-from-isr contract (the blob's ISR bodies branch on it). */
typedef struct {
    osal_isr_fn fn;
    void *arg;
} osal_isr_slot_t;
static osal_isr_slot_t s_rom_slots[OSAL_INT_COUNT];

static void osal_rom_isr_thunk(void *p) {
    osal_isr_slot_t *slot = (osal_isr_slot_t *)p;
    llgo_osal_isr_nest++;
    slot->fn(slot->arg);
    llgo_osal_isr_nest--;
}

void llgo_osal_set_isr(int32_t n, void *f, void *arg) {
    if (n < 0 || (uint32_t)n >= OSAL_INT_COUNT) {
        return;
    }
    s_rom_slots[n].fn = (osal_isr_fn)f;
    s_rom_slots[n].arg = arg;
    intr_handler_set((uint32_t)n, (void *)osal_rom_isr_thunk, &s_rom_slots[n]);
    s_osal_handlers[n].fn = 0;
    s_osal_handlers[n].arg = 0;
}

/* _ints_on/_ints_off(mask): CLIC per-line IE bits, mask indexed by CPU int
 * number (the blob passes 0b10 for int#1 and 0x200 for int#9).
 */
void llgo_osal_ints_on(uint32_t mask) {
    esprv_intc_int_enable(mask);
}

void llgo_osal_ints_off(uint32_t mask) {
    esprv_intc_int_disable(mask);
}

/* _wifi_int_disable/_wifi_int_restore: the critical section pair. Single
 * hart: mask via mintthresh instead of a spinlock (RIOT's freertos layer
 * does the same). Returns the previous threshold as the restore token.
 * Task-context entries also bump the Go-side yield-suppression counter so a
 * blocking call inside the region fails loudly instead of deadlocking
 * (espradio's yieldSuppressed rule).
 */
uint32_t llgo_osal_wifi_int_disable(void *wifi_int_mux) {
    (void)wifi_int_mux;
    uint32_t prev = osal_rd_mintthresh();
    osal_wr_mintthresh(OSAL_CRIT_THRESH);
    if (llgo_osal_isr_nest == 0) {
        extern void llgo_osal_crit_enter(void);
        llgo_osal_crit_enter();
    }
    return prev;
}

void llgo_osal_wifi_int_restore(void *wifi_int_mux, uint32_t prev) {
    (void)wifi_int_mux;
    if (llgo_osal_isr_nest == 0) {
        extern void llgo_osal_crit_exit(void);
        llgo_osal_crit_exit();
    }
    osal_wr_mintthresh(prev);
}

/* _spin_lock_create/delete: single hart - the lock is implicit in the
 * mintthresh masking above. The blob stores the handle in its ROM slot
 * (g_intr_lock_mux) and pairs disable/restore with it; a non-null cookie is
 * all it needs.
 */
void *llgo_osal_spin_lock_create(void) {
    /* non-null cookie; the blob only stores and pairs it */
    return (void *)(uintptr_t)0x51a1;
}

void llgo_osal_spin_lock_delete(void *lock) {
    (void)lock;
}

/* _task_yield_from_isr: request a context switch from ISR context. Nothing
 * to switch on a cooperative runtime (the consumer wakes through its bounded
 * park); counted for observability.
 */
void llgo_osal_task_yield_from_isr(void) {
    llgo_osal_yield_from_isr_count++;
}

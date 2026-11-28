// Package osal is the WiFi OSAL (OS abstraction layer) for linking the
// ESP32-S31 closed-source WiFi libraries (libpp.a / libnet80211.a / libphy.a
// and friends from esp-wifi-raw/idfparts) onto the llgo cooperative fiber
// runtime, with no ESP-IDF and no FreeRTOS in the link.
//
// # Layout
//
//	osal.go        this file: the Go side of every blocking OSAL primitive
//	               (semaphores, recursive mutexes, queues, event groups,
//	               ets_timers, task registry) plus the goroutine-identity
//	               discipline the closed-source library relies on.
//	exported.go    //export functions the C function-pointer table points at.
//	bridge.go      //go:linkname externs for the C helpers (ISR framework,
//	               variadic log, indirect C calls, byte copies).
//	_wrap/*.c      the C side: the wifi_osi_funcs_t table itself, the
//	               RISC-V CLIC interrupt framework the library's MAC/BB ISRs
//	               need, and the esp_wifi public-API glue (compiled by
//	               LLGoFiles with the target's C compiler).
//
// # Why this shape
//
// The closed-source library never calls FreeRTOS directly; every OS primitive
// goes through the wifi_osi_funcs_t table (v0x9, idfparts MANIFEST pins the
// blob version) that esp_wifi_init passes in. Blocking entries (semaphore
// take, queue recv, mutex lock, task delay) must park the *calling
// goroutine*, so they are implemented in Go on top of the primitives the
// fiber runtime already supports (channels, select, sync.Mutex, runtime
// timers - verified by this session's probe1 harness). Pure-context entries
// (ISR handlers, interrupt masking, random, time) stay in C: the MAC/BB ISRs
// run as true hardware interrupts and must never touch the Go scheduler or
// allocate.
//
// # Handles
//
// Every OSAL object the library holds as a void* (tasks, semaphores, queues,
// mutexes, event groups) is handed out as a small integer index into a
// Go-owned registry, never as a Go pointer: the library stores handles in its
// own (C-heap and ROM-slot) memory, which the Go collector does not scan.
// Go objects stay alive through their registry entries; entries are kept for
// the life of the process (deleted objects become tombstones) - the WiFi
// stack creates a bounded number of OS objects, so the table stays tiny.
// Buffers that carry bytes through queues are copies of C memory into
// pointer-free Go []byte storage, so no scanned Go memory ever holds a C
// pointer.
//
// # Memory ownership
//
// _malloc/_free and the library's own direct malloc/free calls both resolve
// to the same newlib allocator (the llgo esp32s31(-flash) targets link
// newlib-esp32), so pointers may cross the boundary in either direction.
// The Go heap is used only for OSAL bookkeeping.
//
// # Goroutine identity (the one invariant to keep)
//
// The library keys several behaviors off _task_get_current_task (thread
// semaphores, "am I the pp task" checks). There is no goroutine ID on the
// fiber runtime, so osal maintains one package-level "current task" variable
// and keeps the invariant: whenever closed-source code is executing, curTask
// is the handle of the task that owns it.
//
//   - the pp-task trampoline sets curTask before its first library call;
//   - every osal WiFi-API wrapper sets curTask at entry and restores it on
//     exit, so user goroutines get a stable pseudo-task for the duration of
//     one call;
//   - every blocking exported entry saves curTask into its own
//     (fiber-preserved) frame before parking and restores it after resuming,
//     so a library frame that reads its identity after a blocking call still
//     sees the right value.
//
// This mirrors TinyGo espradio's task mapping (its radio.go maintains the
// same invariant around runtime.Gosched), adjusted for llgo's fiber runtime.
//
// # ISR wake-ups
//
// The MAC ISR posts queue items from true hardware interrupt context where
// calling the Go scheduler is forbidden. Receivers therefore park in bounded
// slices (queuePollSlice) and re-check the queue on every wake: the runtime's
// timer machinery drives the wake-ups, giving <=1 ms delivery latency for
// ISR-posted items without any scheduler access from interrupt context.
package osal

import (
	"runtime"
	"sync"
	"time"
	_ "unsafe"
)

// OSI_FUNCS_TIME_BLOCKING: the library's "wait forever" tick count
// (wifi_os_adapter.h). Ticks are milliseconds in this OSAL
// (_task_ms_to_tick is the identity map).
const timeBlocking = uint32(0xffffffff)

// cfatal reports an unrecoverable OSAL invariant violation and hangs with
// the message on the console (the emu/serial capture shows it). A hang, not
// a silent continue: every current caller is a "this would deadlock or
// corrupt state" condition.
func cfatal(msg string) {
	println("osal fatal: " + msg)
	for {
		runtime.Gosched()
	}
}

// ---------------------------------------------------------------------------
// task identity
// ---------------------------------------------------------------------------

// taskRecord is one registered task. user==true marks pseudo-tasks handed to
// goroutines calling the WiFi API: they never run a library task body, they
// just need a stable identity and thread semaphore for the duration of one
// API call.
type taskRecord struct {
	id        uintptr
	name      string
	user      bool
	started   chan struct{}
	threadSem uintptr // _wifi_thread_semphr_get, allocated on first use
	fn        uintptr
	arg       uintptr
}

var (
	taskMu sync.Mutex
	tasks  []*taskRecord
)

func allocTask(name string, user bool) *taskRecord {
	taskMu.Lock()
	defer taskMu.Unlock()
	t := &taskRecord{
		id:      uintptr(len(tasks) + 1),
		name:    name,
		user:    user,
		started: make(chan struct{}),
	}
	tasks = append(tasks, t)
	return t
}

func taskByHandle(h uintptr) *taskRecord {
	if h == 0 {
		return nil
	}
	taskMu.Lock()
	defer taskMu.Unlock()
	if int(h) > len(tasks) {
		return nil
	}
	return tasks[h-1]
}

func setCurTask(t uintptr) (prev uintptr) {
	return slotSet(t)
}

func curTaskValue() uintptr {
	return slotGet()
}

func currentTask() *taskRecord {
	h := curTaskValue()
	if h != 0 {
		if t := taskByHandle(h); t != nil {
			return t
		}
	}
	t := allocTask("user", true)
	slotSet(t.id)
	return t
}

// ---------------------------------------------------------------------------
// critical sections (yield suppression)
// ---------------------------------------------------------------------------

// critDepth counts wifi_int_disable regions entered on the current
// goroutine. The library uses these regions to protect shared state; inside
// one, blocking is forbidden (the goroutine that would wake us can never
// run). espradio found library bugs turning into scheduler deadlocks exactly
// here and made it an observable failure ("yieldSuppressed"); so do we.
var critDepth int

func critEnter() int {
	critDepth++
	return critDepth
}

func critExit() {
	critDepth--
}

// ---------------------------------------------------------------------------
// semaphores
// ---------------------------------------------------------------------------

type semaphore struct {
	ch chan struct{}
}

var (
	semMu   sync.Mutex
	sems    []*semaphore
	semDead = map[uintptr]bool{}
)

func allocSem(max, init uint32) uintptr {
	semMu.Lock()
	defer semMu.Unlock()
	if max == 0 {
		max = 1
	}
	s := &semaphore{ch: make(chan struct{}, max)}
	for i := uint32(0); i < init && i < max; i++ {
		s.ch <- struct{}{}
	}
	sems = append(sems, s)
	return uintptr(len(sems))
}

func semByHandle(h uintptr) *semaphore {
	semMu.Lock()
	defer semMu.Unlock()
	if h == 0 || int(h) > len(sems) || semDead[h] {
		return nil
	}
	return sems[h-1]
}

func freeSem(h uintptr) {
	semMu.Lock()
	defer semMu.Unlock()
	semDead[h] = true
}

// semTake blocks up to tickMs milliseconds. The curTask save/restore is the
// identity discipline from the package comment: another goroutine runs while
// we park and may overwrite the package variable, so the parked frame owns
// the restore.
func semTake(h uintptr, tickMs uint32) bool {
	s := semByHandle(h)
	if s == nil {
		return false
	}
	prev := curTaskValue()
	defer func() { slotSet(prev) }()
	if tickMs == timeBlocking {
		<-s.ch
		return true
	}
	if tickMs == 0 {
		select {
		case <-s.ch:
			return true
		default:
			return false
		}
	}
	timer := time.NewTimer(time.Duration(tickMs) * time.Millisecond)
	defer timer.Stop()
	select {
	case <-s.ch:
		return true
	case <-timer.C:
		return false
	}
}

func semGive(h uintptr) bool {
	s := semByHandle(h)
	if s == nil {
		return false
	}
	select {
	case s.ch <- struct{}{}:
		return true
	default:
		// FreeRTOS xSemaphoreGive on a full counting semaphore fails.
		return false
	}
}

// ---------------------------------------------------------------------------
// recursive mutexes
// ---------------------------------------------------------------------------

type mutex struct {
	mu    sync.Mutex
	owner uintptr
	count int
}

var (
	mutexMu   sync.Mutex
	mutexes   []*mutex
	mutexDead = map[uintptr]bool{}
)

func allocMutex() uintptr {
	mutexMu.Lock()
	defer mutexMu.Unlock()
	mutexes = append(mutexes, &mutex{})
	return uintptr(len(mutexes))
}

func mutexByHandle(h uintptr) *mutex {
	mutexMu.Lock()
	defer mutexMu.Unlock()
	if h == 0 || int(h) > len(mutexes) || mutexDead[h] {
		return nil
	}
	return mutexes[h-1]
}

func freeMutex(h uintptr) {
	mutexMu.Lock()
	defer mutexMu.Unlock()
	mutexDead[h] = true
}

// mutexLock implements xSemaphoreTake on a (recursive) mutex. tickMs==0 is
// the non-blocking try-lock; every other value waits indefinitely, matching
// the library's actual usage (IDF adapters see portMAX_DELAY here).
func mutexLock(h uintptr, tickMs uint32) bool {
	m := mutexByHandle(h)
	if m == nil {
		return false
	}
	self := curTaskValue()
	if m.owner == self && self != 0 {
		// Recursive re-entry by the owner never blocks.
		m.count++
		return true
	}
	if critDepth > 0 {
		// Blocking inside a wifi_int_disable region would deadlock the
		// scheduler (the lock holder can never be scheduled). Turn the
		// "impossible" state into an observable failure, like espradio's
		// safeGosched does for its yieldSuppressed counter.
		cfatal("mutex_lock would block inside critical section")
	}
	if tickMs == 0 {
		if !m.mu.TryLock() {
			return false
		}
	} else {
		m.mu.Lock()
	}
	m.owner = self
	m.count = 1
	return true
}

func mutexUnlock(h uintptr) bool {
	m := mutexByHandle(h)
	if m == nil {
		return false
	}
	if m.owner != curTaskValue() || m.count == 0 {
		// Not the owner: FreeRTOS returns failure; never unlock someone
		// else's mutex (the library has a 250 ms-starvation class of bug
		// here per espradio, which we surface instead of papering over).
		return false
	}
	m.count--
	if m.count == 0 {
		m.owner = 0
		m.mu.Unlock()
	}
	return true
}

// ---------------------------------------------------------------------------
// queues
// ---------------------------------------------------------------------------

type queue struct {
	itemSize int
	cap      int
	items    [][]byte
	// wake is closed-and-replaced whenever an item arrives; timed receivers
	// wait on it so a recv can wake early.
	wake chan struct{}
	mu   sync.Mutex
}

var (
	queueMu   sync.Mutex
	queues    []*queue
	queueDead = map[uintptr]bool{}
)

func allocQueue(len_, itemSize uint32) uintptr {
	queueMu.Lock()
	defer queueMu.Unlock()
	if itemSize == 0 {
		itemSize = 4
	}
	cap_ := int(len_)
	if cap_ == 0 {
		cap_ = 1
	}
	q := &queue{
		itemSize: int(itemSize),
		cap:      cap_,
		items:    make([][]byte, 0, cap_),
		wake:     make(chan struct{}),
	}
	queues = append(queues, q)
	return uintptr(len(queues))
}

func queueByHandle(h uintptr) *queue {
	queueMu.Lock()
	defer queueMu.Unlock()
	if h == 0 || int(h) > len(queues) || queueDead[h] {
		return nil
	}
	return queues[h-1]
}

func freeQueue(h uintptr) {
	queueMu.Lock()
	defer queueMu.Unlock()
	queueDead[h] = true
}

// queueSend copies one item to the back (front=false) or front and wakes a
// receiver. Returns false when the queue is full and the timeout expires.
func queueSend(h uintptr, item uintptr, tickMs uint32, front bool) bool {
	q := queueByHandle(h)
	if q == nil || item == 0 {
		return false
	}
	prev := curTaskValue()
	defer func() { slotSet(prev) }()
	buf := make([]byte, q.itemSize)
	queueCopy(uintptr(unsafe0(buf)), item, uintptr(q.itemSize))
	deadline := time.Now().Add(time.Duration(tickMs) * time.Millisecond)
	for {
		q.mu.Lock()
		full := len(q.items) >= q.cap
		if !full {
			if front {
				q.items = append([][]byte{buf}, q.items...)
			} else {
				q.items = append(q.items, buf)
			}
			close(q.wake)
			q.wake = make(chan struct{})
			q.mu.Unlock()
			return true
		}
		wake := q.wake
		q.mu.Unlock()

		if tickMs == 0 {
			return false
		}
		if tickMs != timeBlocking && !time.Now().Before(deadline) {
			return false
		}
		var wait <-chan time.Time
		var timer *time.Timer
		if tickMs != timeBlocking {
			d := time.Until(deadline)
			if d <= 0 {
				return false
			}
			timer = time.NewTimer(d)
			wait = timer.C
		}
		select {
		case <-wake:
		case <-wait:
		}
		if timer != nil {
			timer.Stop()
		}
	}
}

// queuePollSlice bounds one park while waiting for ISR-posted items (see
// the package comment).
const queuePollSlice = time.Millisecond

func queueRecv(h uintptr, item uintptr, tickMs uint32) bool {
	q := queueByHandle(h)
	if q == nil || item == 0 {
		return false
	}
	prev := curTaskValue()
	defer func() { slotSet(prev) }()
	deadline := time.Now().Add(time.Duration(tickMs) * time.Millisecond)
	for {
		// ISR-staged items first: the MAC ISR posts into the C staging ring
		// (it must not touch Go state), so the bounded park below re-checks
		// it on every wake.
		if isrRingGet(h, item) != 0 {
			return true
		}
		q.mu.Lock()
		if len(q.items) > 0 {
			buf := q.items[0]
			q.items = q.items[1:]
			// Broadcast a free slot: close wakes every blocked sender, each
			// re-checks capacity on its own next pass, so none are lost when
			// multiple senders queue up.
			close(q.wake)
			q.wake = make(chan struct{})
			q.mu.Unlock()
			queueCopy(item, uintptr(unsafe0(buf)), uintptr(q.itemSize))
			return true
		}
		wake := q.wake
		q.mu.Unlock()

		if tickMs == 0 {
			return false
		}
		if tickMs != timeBlocking && !time.Now().Before(deadline) {
			return false
		}
		d := queuePollSlice
		if tickMs != timeBlocking {
			if r := time.Until(deadline); r < d {
				d = r
			}
		}
		timer := time.NewTimer(d)
		select {
		case <-wake:
		case <-timer.C:
		}
		timer.Stop()
	}
}

func queueMsgWaiting(h uintptr) uint32 {
	q := queueByHandle(h)
	if q == nil {
		return 0
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	return uint32(len(q.items))
}

// ---------------------------------------------------------------------------
// event groups
// ---------------------------------------------------------------------------

type eventGroup struct {
	mu   sync.Mutex
	bits uint32
	wake chan struct{}
}

var (
	egMu   sync.Mutex
	egs    []*eventGroup
	egDead = map[uintptr]bool{}
)

func allocEventGroup() uintptr {
	egMu.Lock()
	defer egMu.Unlock()
	egs = append(egs, &eventGroup{wake: make(chan struct{})})
	return uintptr(len(egs))
}

func egByHandle(h uintptr) *eventGroup {
	egMu.Lock()
	defer egMu.Unlock()
	if h == 0 || int(h) > len(egs) || egDead[h] {
		return nil
	}
	return egs[h-1]
}

func freeEventGroup(h uintptr) {
	egMu.Lock()
	defer egMu.Unlock()
	egDead[h] = true
}

func egSetBits(h uintptr, bits uint32) uint32 {
	eg := egByHandle(h)
	if eg == nil {
		return 0
	}
	eg.mu.Lock()
	eg.bits |= bits
	close(eg.wake)
	eg.wake = make(chan struct{})
	eg.mu.Unlock()
	return eg.bits
}

func egClearBits(h uintptr, bits uint32) uint32 {
	eg := egByHandle(h)
	if eg == nil {
		return 0
	}
	eg.mu.Lock()
	eg.bits &^= bits
	eg.mu.Unlock()
	return eg.bits
}

func egWaitBits(h uintptr, bitsToWait uint32, clear bool, all bool, tickMs uint32) uint32 {
	eg := egByHandle(h)
	if eg == nil {
		return 0
	}
	prev := curTaskValue()
	defer func() { slotSet(prev) }()
	deadline := time.Now().Add(time.Duration(tickMs) * time.Millisecond)
	for {
		eg.mu.Lock()
		satisfied := eg.bits&bitsToWait != 0
		if all {
			satisfied = eg.bits&bitsToWait == bitsToWait
		}
		if satisfied {
			got := eg.bits & bitsToWait
			if clear {
				eg.bits &^= bitsToWait
			}
			eg.mu.Unlock()
			return got
		}
		wake := eg.wake
		eg.mu.Unlock()

		if tickMs == 0 {
			return 0
		}
		if tickMs != timeBlocking && !time.Now().Before(deadline) {
			return 0
		}
		var wait <-chan time.Time
		var timer *time.Timer
		if tickMs != timeBlocking {
			d := time.Until(deadline)
			if d <= 0 {
				return 0
			}
			timer = time.NewTimer(d)
			wait = timer.C
		}
		select {
		case <-wake:
		case <-wait:
		}
		if timer != nil {
			timer.Stop()
		}
	}
}

// ---------------------------------------------------------------------------
// ets timers (the library's software timers, not FreeRTOS sw timers)
// ---------------------------------------------------------------------------

// timerState mirrors one ets_timer: the library owns the handle value (the
// address of a struct embedded in its own objects) and reaches this record
// through it. Records are created lazily on first touch and dropped on
// timer_done.
type timerState struct {
	dead     bool
	fn       uintptr
	arg      uintptr
	armed    bool
	deadline time.Time
	period   time.Duration
	repeat   bool
}

var (
	timerMu  sync.Mutex
	timers   = map[uintptr]*timerState{}
	timerSrv = make(chan struct{}, 1)
	timerRun bool
)

func timerRecord(h uintptr) *timerState {
	timerMu.Lock()
	defer timerMu.Unlock()
	t := timers[h]
	if t == nil {
		t = &timerState{}
		timers[h] = t
	}
	return t
}

func timerDrop(h uintptr) {
	timerMu.Lock()
	defer timerMu.Unlock()
	delete(timers, h)
}

// timerKick pokes the timer service goroutine (coalesced send) and starts
// it on first use.
func timerKick() {
	timerMu.Lock()
	run := !timerRun
	timerRun = true
	timerMu.Unlock()
	if run {
		go timerService()
	}
	select {
	case timerSrv <- struct{}{}:
	default:
	}
}

// timerService serially fires expired ets_timer callbacks in its own task
// context. Callbacks may arm/disarm/setfn timers (that is the ets_timer
// contract), which the single service goroutine serializes by construction.
func timerService() {
	for {
		now := time.Now()
		var next time.Time
		haveNext := false
		var fire []timerState
		timerMu.Lock()
		for _, t := range timers {
			if !t.dead && t.armed && !t.deadline.After(now) {
				fire = append(fire, *t)
				if t.repeat && t.period > 0 {
					t.deadline = now.Add(t.period)
				} else {
					t.armed = false
				}
			}
			if !t.dead && t.armed && (!haveNext || t.deadline.Before(next)) {
				next = t.deadline
				haveNext = true
			}
		}
		timerMu.Unlock()
		for i := range fire {
			callVoidPtr(fire[i].fn, fire[i].arg)
		}

		var wait <-chan time.Time
		var timer *time.Timer
		if haveNext {
			timer = time.NewTimer(time.Until(next))
			wait = timer.C
		}
		select {
		case <-timerSrv:
		case <-wait:
		}
		if timer != nil {
			timer.Stop()
		}
	}
}

func timerArm(h uintptr, ms uint32, repeat bool) {
	t := timerRecord(h)
	timerMu.Lock()
	t.dead = false
	t.armed = true
	t.deadline = time.Now().Add(time.Duration(ms) * time.Millisecond)
	t.period = time.Duration(ms) * time.Millisecond
	t.repeat = repeat
	timerMu.Unlock()
	timerKick()
}

func timerArmUs(h uintptr, us uint32, repeat bool) {
	t := timerRecord(h)
	timerMu.Lock()
	t.dead = false
	t.armed = true
	t.deadline = time.Now().Add(time.Duration(us) * time.Microsecond)
	t.period = time.Duration(us) * time.Microsecond
	t.repeat = repeat
	timerMu.Unlock()
	timerKick()
}

func timerDisarm(h uintptr) {
	t := timerRecord(h)
	timerMu.Lock()
	t.armed = false
	timerMu.Unlock()
}

func timerSetFn(h uintptr, fn, arg uintptr) {
	t := timerRecord(h)
	timerMu.Lock()
	t.fn, t.arg = fn, arg
	timerMu.Unlock()
}

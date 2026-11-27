package osal

import (
	"time"
	_ "unsafe"
)

// Exported entry points: every //export here is a C-ABI function the
// wifi_osi_funcs_t table (or the C glue) references directly. Signatures use
// plain C-width types (uintptr for pointers, uint32/int32 scalars, int64 for
// 64-bit time); _wrap/osi_osal.c casts at the table initializer where the
// header's prototype differs (bool, unsigned long).

// IntrInit installs the OSAL trap handler (chaining to the boot trap) and
// enables global interrupts; must run before any WiFi API call.
func IntrInit() {
	cIntrInit()
}

//go:linkname cIntrInit C.llgo_osal_intr_init
func cIntrInit()

// ---- environment ----

// llgo_osal_env_is_chip: running on real silicon (emu counts).
//
//export llgo_osal_env_is_chip
func osalEnvIsChip() int32 { return 1 }

// crit enter/exit pair called from C (llgo_osal_wifi_int_disable): the
// Go-side half of the yield-suppression counter.
//
//export llgo_osal_crit_enter
func osalCritEnter() { critEnter() }

//export llgo_osal_crit_exit
func osalCritExit() { critExit() }

// ---- task identity ----

// llgo_osal_task_get_current_task returns the identity discipline's current
// handle; never 0.
//
//export llgo_osal_task_get_current_task
func osalTaskGetCurrent() uintptr {
	return currentTask().id
}

//export llgo_osal_task_get_max_priority
func osalTaskGetMaxPriority() int32 { return 25 } // FreeRTOS-compatible ceiling (IDF uses 25)

// llgo_osal_task_create_pinned_to_core maps a library task to a goroutine
// running the library's own entry through the C trampoline (runTask). The
// stack_depth parameter is advisory: fiber stacks grow on demand, so the
// value is recorded in the task name only. Core id is ignored (single-hart
// scheduler). The creator blocks until the goroutine has started and
// published its handle, because the library dereferences *task_handle
// immediately after this call returns (espradio radio.go's sync-start).
//
//export llgo_osal_task_create_pinned_to_core
func osalTaskCreatePinned(fn, name uintptr, stack uint32, param uintptr, prio uint32, handleOut uintptr, core uintptr) int32 {
	return osalTaskCreateCommon(fn, name, param, handleOut)
}

//export llgo_osal_task_create
func osalTaskCreate(fn, name uintptr, stack uint32, param uintptr, prio uint32, handleOut uintptr) int32 {
	return osalTaskCreateCommon(fn, name, param, handleOut)
}

func osalTaskCreateCommon(fn, name, param, handleOut uintptr) int32 {
	if fn == 0 || handleOut == 0 {
		return 0 // ESP_FAIL shape: the library only checks non-OSI_OK-ish values
	}
	t := allocTask(cstring(name), false)
	t.fn, t.arg = fn, param
	go func() {
		prev := setCurTask(t.id)
		close(t.started) // creator may proceed once identity is installed
		runTask(t.fn, t.arg)
		setCurTask(prev)
		// Library task bodies never return (they loop on a queue or
		// self-delete through _task_delete); reaching here means the task
		// body ended without deleting itself. Nothing can release this
		// goroutine's fiber from outside, so mark and park forever - the
		// task registry tombstone keeps the rest of the system consistent.
		tombstone(t)
		select {}
	}()
	<-t.started
	storeHandle(handleOut, t.id)
	return 1 // true, matching IDF's adapter return convention
}

// tombstone records a finished task so _task_get_current_task keeps working
// for late identity probes.
func tombstone(t *taskRecord) {
	_ = t
}

// storeHandle writes the 32-bit handle at a raw C address (the library's
// handle out-params live in its own memory).
func storeHandle(dst uintptr, h uintptr) {
	v := uint32(h)
	queueCopy(dst, uintptr(unsafe0ptr(&v)), 4)
}

// llgo_osal_task_delete cannot preempt a goroutine; the fiber dies when its
// body returns. For the self-delete case that is exact; for external deletes
// of a blocked task the goroutine stays parked (bounded: the library creates
// a fixed small set of tasks). Recorded as a known limitation.
//
//export llgo_osal_task_delete
func osalTaskDelete(h uintptr) {
	_ = h
}

// llgo_osal_task_delay parks the caller; identity save/restore per the
// package comment.
//
//export llgo_osal_task_delay
func osalTaskDelay(tickMs uint32) {
	prev := curTask
	defer func() { curTask = prev }()
	if tickMs == 0 {
		return
	}
	time.Sleep(time.Duration(tickMs) * time.Millisecond)
}

// ---- semaphores ----

//export llgo_osal_semphr_create
func osalSemphrCreate(max, init uint32) uintptr {
	return allocSem(max, init)
}

//export llgo_osal_semphr_delete
func osalSemphrDelete(h uintptr) {
	freeSem(h)
}

//export llgo_osal_semphr_take
func osalSemphrTake(h uintptr, tickMs uint32) int32 {
	if semTake(h, tickMs) {
		return 1 // pdTRUE
	}
	return 0
}

//export llgo_osal_semphr_give
func osalSemphrGive(h uintptr) int32 {
	if semGive(h) {
		return 1
	}
	return 0
}

// llgo_osal_wifi_thread_semphr_get returns the calling task's dedicated
// semaphore (the library's synchronous-API handshake), allocated on first
// use per task handle.
//
//export llgo_osal_wifi_thread_semphr_get
func osalThreadSemphrGet() uintptr {
	t := currentTask()
	if t.threadSem == 0 {
		t.threadSem = allocSem(1, 0)
	}
	return t.threadSem
}

// ---- mutexes ----

//export llgo_osal_mutex_create
func osalMutexCreate() uintptr { return allocMutex() }

//export llgo_osal_recursive_mutex_create
func osalRecursiveMutexCreate() uintptr { return allocMutex() }

//export llgo_osal_mutex_delete
func osalMutexDelete(h uintptr) { freeMutex(h) }

//export llgo_osal_mutex_lock
func osalMutexLock(h uintptr, tickMs uint32) int32 {
	if mutexLock(h, tickMs) {
		return 1
	}
	return 0
}

//export llgo_osal_mutex_unlock
func osalMutexUnlock(h uintptr) int32 {
	if mutexUnlock(h) {
		return 1
	}
	return 0
}

// ---- queues ----

//export llgo_osal_queue_create
func osalQueueCreate(len_, itemSize uint32) uintptr {
	return allocQueue(len_, itemSize)
}

//export llgo_osal_queue_delete
func osalQueueDelete(h uintptr) { freeQueue(h) }

//export llgo_osal_queue_send
func osalQueueSend(h, item uintptr, tickMs uint32) int32 {
	return boolToInt32(queueSend(h, item, tickMs, false))
}

//export llgo_osal_queue_send_to_back
func osalQueueSendToBack(h, item uintptr, tickMs uint32) int32 {
	return boolToInt32(queueSend(h, item, tickMs, false))
}

//export llgo_osal_queue_send_to_front
func osalQueueSendToFront(h, item uintptr, tickMs uint32) int32 {
	return boolToInt32(queueSend(h, item, tickMs, true))
}

//export llgo_osal_queue_recv
func osalQueueRecv(h, item uintptr, tickMs uint32) int32 {
	return boolToInt32(queueRecv(h, item, tickMs))
}

//export llgo_osal_queue_msg_waiting
func osalQueueMsgWaiting(h uintptr) uint32 { return queueMsgWaiting(h) }

// ---- event groups ----

//export llgo_osal_event_group_create
func osalEventGroupCreate() uintptr { return allocEventGroup() }

//export llgo_osal_event_group_delete
func osalEventGroupDelete(h uintptr) { freeEventGroup(h) }

//export llgo_osal_event_group_set_bits
func osalEventGroupSetBits(h uintptr, bits uint32) uint32 { return egSetBits(h, bits) }

//export llgo_osal_event_group_clear_bits
func osalEventGroupClearBits(h uintptr, bits uint32) uint32 { return egClearBits(h, bits) }

//export llgo_osal_event_group_wait_bits
func osalEventGroupWaitBits(h, bitsToWait uintptr, clearOnExit, waitForAllBits int32, tickMs uint32) uint32 {
	return egWaitBits(h, uint32(bitsToWait), clearOnExit != 0, waitForAllBits != 0, tickMs)
}

// ---- timers ----

//export llgo_osal_timer_arm
func osalTimerArm(h uintptr, ms uint32, repeat int32) { timerArm(h, ms, repeat != 0) }

//export llgo_osal_timer_arm_us
func osalTimerArmUs(h uintptr, us uint32, repeat int32) { timerArmUs(h, us, repeat != 0) }

//export llgo_osal_timer_disarm
func osalTimerDisarm(h uintptr) { timerDisarm(h) }

//export llgo_osal_timer_setfn
func osalTimerSetFn(h, fn, arg uintptr) { timerSetFn(h, fn, arg) }

//export llgo_osal_timer_done
func osalTimerDone(h uintptr) { timerDrop(h) }

// ---- events ----

// llgo_osal_event_post stages the event into the dispatcher goroutine and
// returns success. The library posts from task context only (net80211/pp
// paths); an ISR post would be a bug - report it loudly.
//
//export llgo_osal_event_post
func osalEventPost(base uintptr, id int32, data uintptr, size uintptr, tickMs uint32) int32 {
	if isFromISR() != 0 {
		cfatal("event_post from ISR context")
		return 0
	}
	var copyBuf uintptr
	if size > 0 && data != 0 {
		copyBuf = cMalloc(size)
		if copyBuf == 0 {
			return 0
		}
		queueCopy(copyBuf, data, size)
	}
	go dispatchEvent(base, id, copyBuf, size)
	return 1 // ESP_OK
}

// ---- misc (implemented Go-side for the identity/timer machinery they need)

//export llgo_osal_task_ms_to_tick
func osalTaskMsToTick(ms uint32) int32 { return int32(ms) } // ticks are ms here

func boolToInt32(b bool) int32 {
	if b {
		return 1
	}
	return 0
}

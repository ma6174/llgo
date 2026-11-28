package osal

import (
	"sync"
	_ "unsafe"
)

// Per-goroutine task identity.
//
// The blob's locking protocol (current_task_is_wifi_task recursion checks,
// mutex owner comparisons) keys on a stable per-thread task identity. The
// original implementation kept that identity in a package variable, which is
// wrong on a fiber runtime: every goroutine that runs a piece of library code
// overwrote the same slot, so a goroutine entering mutex_lock could observe
// another goroutine's identity and misjudge the owner (observed as the scan
// field being touched concurrently from scan_start and the pp task under a
// "held" wifi_api_lock).
//
// The llgo baremetal fiber scheduler keeps a per-goroutine context: the
// scheduler runs setg(gp) before resuming a fiber, so runtime getg() returns
// the running goroutine's own context. Link it in and key the identity map on
// it (espradio's tinygo_task_current is the same primitive on its runtime).

//go:linkname osalGetg github.com/xgo-dev/llgo/runtime/internal/runtime.TaskIdentity
func osalGetg() uintptr

// taskSlots maps goroutine identity -> the blob task handle this goroutine is
// currently masquerading as (set by the task trampoline and by apiCall).
var taskSlots = map[uintptr]uintptr{}
var taskSlotsMu sync.Mutex

func gid() uintptr {
	return osalGetg()
}

func slotSet(t uintptr) (prev uintptr) {
	k := gid()
	taskSlotsMu.Lock()
	prev = taskSlots[k]
	taskSlots[k] = t
	taskSlotsMu.Unlock()
	return prev
}

func slotGet() uintptr {
	k := gid()
	taskSlotsMu.Lock()
	v := taskSlots[k]
	taskSlotsMu.Unlock()
	return v
}

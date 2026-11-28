//go:build llgo && baremetal

package runtime

import "unsafe"

// TaskIdentity returns a value that uniquely identifies the currently running
// goroutine on the fiber runtime (its own *g, kept current by the scheduler's
// setg before each fiber resume). The WiFi OSAL uses it as the per-thread
// task identity the closed-source blob's locking protocol keys on - the same
// role tinygo_task_current plays on the TinyGo espradio runtime. //go:noinline
// keeps the symbol alive for //go:linkname consumers outside the runtime
// package (getg itself would be inlined away).
//
//go:noinline
func TaskIdentity() uintptr {
	return uintptr(unsafe.Pointer(getg()))
}

package osal

// Bridge helpers: the small bodyless-primitive implementations that the
// rest of the package uses to cross the Go/C boundary. Every helper here is
// either a //go:linkname to a C symbol (compiled from _wrap/*.c by LLGoFiles
// or provided by newlib) or a few lines of unsafe plumbing.

import (
	"unsafe"

	_ "unsafe"
)

// unsafe0ptr returns the raw address of a uint32 (handle stores).
func unsafe0ptr(p *uint32) uintptr {
	return uintptr(unsafe.Pointer(p))
}

// unsafe0 returns the address of b's first byte for passing through C ABI
// helpers.
func unsafe0(b []byte) uintptr {
	return uintptr(unsafe.Pointer(unsafe.SliceData(b)))
}

// cMemmove is newlib's memmove: the byte bridge between C memory and Go
// queue storage. Same allocator world on both ends, pointer-free bytes only.
//
//go:linkname cMemmove C.memmove
func cMemmove(dst, src uintptr, n uintptr) uintptr

func copyBytes(dst, src uintptr) {
	cMemmove(dst, src, uintptr(4))
}

// queueCopy copies exactly n bytes.
func queueCopy(dst, src uintptr, n uintptr) {
	cMemmove(dst, src, n)
}

// cStrLen is newlib's strlen.
//
//go:linkname cStrLen C.strlen
func cStrLen(p uintptr) uintptr

// cstring reads a NUL-terminated C string into a Go string (log lines,
// timer names, NVS keys - all read-only use).
func cstring(p uintptr) string {
	if p == 0 {
		return ""
	}
	n := cStrLen(p)
	if n == 0 {
		return ""
	}
	b := make([]byte, n)
	queueCopy(uintptr(unsafe0(b)), p, n)
	return string(b)
}

// cMalloc/cFree are newlib's allocator, shared with the closed-source
// library's own direct malloc/free calls (event data staging).
//
//go:linkname cMalloc C.malloc
func cMalloc(n uintptr) uintptr

//go:linkname cFree C.free
func cFree(p uintptr)

// callVoidPtr calls a C function through a raw function pointer: the ets
// timer callback shape void (*)(void *arg).
//
//go:linkname callVoidPtr C.llgo_osal_call_void_ptrptr
func callVoidPtr(fn, arg uintptr)

// callEventHandler calls a registered esp_event handler through its C
// prototype: void (*)(void *handler_arg, esp_event_base_t base,
// int32_t event_id, void *event_data).
//
//go:linkname callEventHandler C.llgo_osal_call_event_handler
func callEventHandler(fn, arg, base uintptr, id int32, data uintptr)

// runTask is the C trampoline that enters a blob task body through its raw
// C function pointer: void (*)(void *arg). This is the whole task->goroutine
// bridge (espradio's espradio_run_task shape).
//
//go:linkname runTask C.llgo_osal_run_task
func runTask(fn, arg uintptr)

// isFromISR reports interrupt context (C side reads the nesting counter set
// by the trap entry).
//
//go:linkname isFromISR C.llgo_osal_is_from_isr_context
func isFromISR() int32

// isrRingGet drains one staged ISR item into dst (the C staging rings are
// the only ISR->task bridge; see _wrap/osi_osal.c).
//
//go:linkname isrRingGet C.llgo_osal_isr_ring_get
func isrRingGet(handle, dst uintptr) int32

// ---- exported Go entry points used by wifi.go (public API wrappers) ----

// wifiInitInternal is the closed-source library's entry point
// (libnet80211.a); declared by wifi_api.c.
//
//go:linkname wifiInitInternal C.esp_wifi_init_internal
func wifiInitInternal(cfg uintptr) int32

//go:linkname wifiDeinitInternal C.esp_wifi_deinit_internal
func wifiDeinitInternal() int32

//go:linkname wifiStart C.esp_wifi_start
func wifiStart() int32

//go:linkname wifiStop C.esp_wifi_stop
func wifiStop() int32

//go:linkname wifiSetMode C.esp_wifi_set_mode
func wifiSetMode(mode uint32) int32

//go:linkname wifiSetConfig C.esp_wifi_set_config
func wifiSetConfig(ifx uint32, cfg uintptr) int32

//go:linkname wifiScanStart C.esp_wifi_scan_start
func wifiScanStart(block, showHidden uint32) int32

//go:linkname wifiScanGetApNum C.esp_wifi_scan_get_ap_num
func wifiScanGetApNum(numOut uintptr) int32

//go:linkname wifiScanGetApRecords C.esp_wifi_scan_get_ap_records
func wifiScanGetApRecords(numInOut uintptr, records uintptr) int32

//go:linkname wifiClearApList C.esp_wifi_clear_ap_list
func wifiClearApList() int32

//go:linkname wifiConnect C.esp_wifi_connect_internal
func wifiConnect() int32

//go:linkname wifiDisconnect C.esp_wifi_disconnect_internal
func wifiDisconnect() int32

//go:linkname wifiGetMac C.esp_wifi_get_mac
func wifiGetMac(ifx uint32, mac uintptr) int32

//go:linkname wifiSetStorage C.esp_wifi_set_storage
func wifiSetStorage(storage uint32) int32

// esp_wifi_scan_start's blocking parameter when the caller wants the
// synchronous flavor.
const scanBlock = true

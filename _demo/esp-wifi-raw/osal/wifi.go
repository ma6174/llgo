package osal

// Go-facing WiFi API: thin wrappers that (a) install the goroutine-identity
// discipline around every entry into closed-source code and (b) expose the
// results the demo's gates need. The C layer (wifi_api.c) owns every ABI
// detail: the init config (WIFI_INIT_CONFIG_DEFAULT shape), the struct
// layouts the blob expects (scan config, AP records), and the public-API
// shims for symbols that live in IDF source rather than the blob
// (esp_wifi_init/deinit/connect/disconnect).

import "unsafe"

// WiFiMode values from the pinned esp_wifi_types_generic.h.
const (
	ModeNULL  = 0
	ModeSTA   = 1
	ModeAP    = 2
	ModeAPSTA = 3
	ModeNAN   = 4
)

// Storage options (esp_wifi_set_storage).
const (
	StorageFlash = 0
	StorageRAM   = 1
)

// Interface ids.
const (
	IF_STA = 0
	IF_AP  = 1
)

// WifiInit calls esp_wifi_init with the adapter's default config
// (wifi_api.c's g_wifi_default_config, WIFI_INIT_CONFIG_DEFAULT shape).
func WifiInit() int32 {
	return apiCall(func() int32 { return cWifiInit() })
}

//go:linkname cWifiInit C.llgo_wifi_init
func cWifiInit() int32

func WifiDeinit() int32 { return apiCall(cWifiDeinit) }

//go:linkname cWifiDeinit C.llgo_wifi_deinit
func cWifiDeinit() int32

func WifiSetMode(mode uint32) int32 {
	return apiCall(func() int32 { return wifiSetMode(mode) })
}

func WifiStart() int32 { return apiCall(wifiStart) }
func WifiStop() int32  { return apiCall(wifiStop) }

func WifiConnect() int32 {
	return apiCall(func() int32 { return cWifiConnect() })
}

//go:linkname cWifiConnect C.llgo_wifi_connect
func cWifiConnect() int32

func WifiDisconnect() int32 {
	return apiCall(cWifiDisconnect)
}

//go:linkname cWifiDisconnect C.llgo_wifi_disconnect
func cWifiDisconnect() int32

func WifiSetStorage(s uint32) int32 {
	return apiCall(func() int32 { return wifiSetStorage(s) })
}

// WifiSetSTAConfig sets the STA config with the given SSID/password (open
// or PSK auth chosen by the C shim when password is empty).
func WifiSetSTAConfig(ssid, password string) int32 {
	return apiCall(func() int32 {
		return cWifiSetSTAConfig(cstr(ssid), cstr(password))
	})
}

//
//go:linkname cWifiSetSTAConfig C.llgo_wifi_set_sta_config
func cWifiSetSTAConfig(ssid, pass uintptr) int32

// WifiScanStart starts a scan on the given channel (0 = all channels per
// the scan config contract). block=false matches the demo's event-driven
// flow (scan done arrives as WIFI_EVENT_SCAN_DONE).
func WifiScanStart(channel uint32, showHidden, block bool) int32 {
	return apiCall(func() int32 {
		return cWifiScanStart(channel, boolToInt32(showHidden), boolToInt32(block))
	})
}

//
//go:linkname cWifiScanStart C.llgo_wifi_scan_start
func cWifiScanStart(channel uint32, showHidden, block int32) int32

func WifiScanGetApNum() (uint16, int32) {
	var n uint16
	rc := apiCall(func() int32 { return wifiScanGetApNum(uintptr(unsafe.Pointer(&n))) })
	return n, rc
}

// APRecord is the simplified per-AP view the C shim writes out
// (llgo_wifi_ap_view_t in wifi_api.c); the full wifi_ap_record_t stays in C
// where the layout is pinned by the header.
type APRecord struct {
	SSID     [33]byte
	RSSI     int8
	Channel  uint8
	Authmode uint8
}

// WifiScanGetApRecords fetches up to max simplified records.
func WifiScanGetApRecords(max uint16) ([]APRecord, int32) {
	aps := make([]APRecord, max)
	rc := apiCall(func() int32 {
		return cWifiScanRecords(uint32(max), uintptr(unsafe.Pointer(&aps[0])))
	})
	n := uint16(rc & 0xffff)
	if int(n) > len(aps) {
		n = uint16(len(aps))
	}
	return aps[:n], rc >> 16
}

//
//go:linkname cWifiScanRecords C.llgo_wifi_scan_records
func cWifiScanRecords(max uint32, out uintptr) int32

// WifiGetMac reads the interface MAC.
func WifiGetMac(ifx uint32) ([6]byte, int32) {
	var mac [6]byte
	rc := apiCall(func() int32 { return wifiGetMac(ifx, uintptr(unsafe.Pointer(&mac[0]))) })
	return mac, rc
}

// apiCall wraps one entry into the library with the task-identity
// discipline: a fresh pseudo-task identity for this call, restored on exit.
func apiCall(f func() int32) int32 {
	t := allocTask("api", true)
	prev := setCurTask(t.id)
	defer setCurTask(prev)
	return f()
}

// ---- event subscription (demo-facing) ----

// Event IDs (esp_wifi_types_generic.h wifi_event_t).
const (
	EvWiFiReady       = 0
	EvScanDone        = 1
	EvStaStart        = 2
	EvStaStop         = 3
	EvStaConnected    = 4
	EvStaDisconnected = 5
	EvApStart         = 6
	EvApStop          = 7
)

var eventCh = make(chan Event, 32)

// Event is one dispatched WiFi event (data copied out of the C buffer).
type Event struct {
	ID   int32
	Data []byte
}

// Events returns the package's event channel (buffered; drop on overflow).
func Events() <-chan Event { return eventCh }

func init() {
	bootInstall()
}

// cWifiEventBase returns the WIFI_EVENT base pointer value.
//
//go:linkname cWifiEventBase C.llgo_wifi_event_base
func cWifiEventBase() uintptr

// cstr builds a NUL-terminated C string copy (newlib malloc; freed by the
// C shim).
func cstr(s string) uintptr {
	b := append([]byte(s), 0)
	p := cMalloc(uintptr(len(b)))
	if p == 0 {
		return 0
	}
	queueCopy(p, uintptr(unsafe.Pointer(unsafe.SliceData(b))), uintptr(len(b)))
	return p
}

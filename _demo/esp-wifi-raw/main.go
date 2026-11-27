// P1b gate sequence. Each gate prints one fixed marker line; per-gate emu
// logs land in the session artifacts (wifi-osal/logs/).
//
//	gate1  esp_wifi_init returns ESP_OK with the OSAL table installed
//	gate2  set_mode(STA) + start, WIFI_EVENT_STA_START reaches Go
//	gate3  scan -> SCAN_DONE -> non-empty AP records
//	gate4  (best effort) connect to the emu gateway AP + DHCP
package main

import (
	"espwifiraw/osal"
	"time"
)

func waitEvent(id int32, timeout time.Duration) (osal.Event, bool) {
	deadline := time.After(timeout)
	for {
		select {
		case ev := <-osal.Events():
			if ev.ID == id {
				return ev, true
			}
			println("wifi-raw: event id=", int(ev.ID))
		case <-deadline:
			return osal.Event{}, false
		}
	}
}

func main() {
	osal.Boot()
	println("wifi-raw: boot")
	osal.IntrInit()
	println("wifi-raw: gate1 init")
	if rc := osal.WifiInit(); rc != 0 {
		println("wifi-raw: GATE1 FAIL rc=", rc)
		return
	}
	println("wifi-raw: GATE1 OK")

	println("wifi-raw: gate2 start sta")
	if rc := osal.WifiSetMode(osal.ModeSTA); rc != 0 {
		println("wifi-raw: GATE2 FAIL set_mode rc=", rc)
		return
	}
	if rc := osal.WifiStart(); rc != 0 {
		println("wifi-raw: GATE2 FAIL start rc=", rc)
		return
	}
	if _, ok := waitEvent(osal.EvStaStart, 15*time.Second); !ok {
		println("wifi-raw: GATE2 FAIL no STA_START event")
		return
	}
	println("wifi-raw: GATE2 OK")

	println("wifi-raw: gate3 scan")
	if rc := osal.WifiScanStart(0, false, false); rc != 0 {
		println("wifi-raw: GATE3 FAIL scan_start rc=", rc)
		return
	}
	if _, ok := waitEvent(osal.EvScanDone, 30*time.Second); !ok {
		println("wifi-raw: GATE3 FAIL no SCAN_DONE event")
		return
	}
	n, rc := osal.WifiScanGetApNum()
	if rc != 0 {
		println("wifi-raw: GATE3 FAIL get_ap_num rc=", rc)
		return
	}
	println("wifi-raw: scan ap num=", int(n))
	aps, rc := osal.WifiScanGetApRecords(n)
	if rc != 0 || len(aps) == 0 {
		println("wifi-raw: GATE3 FAIL records rc=", rc>>16)
		return
	}
	for i, ap := range aps {
		ssid := ""
		for _, c := range ap.SSID {
			if c == 0 {
				break
			}
			ssid += string(rune(c))
		}
		println("wifi-raw: ap[", i, "] ssid=", ssid, " ch=", int(ap.Channel), " rssi=", int(ap.RSSI))
	}
	println("wifi-raw: GATE3 OK")

	println("wifi-raw: gate4 connect (best effort)")
	// The emu user-net gateway AP: open/WPA2 by emu default; the S31 blob
	// ships no supplicant, so WPA handshakes stop at the 4-way exchange.
	osal.WifiSetSTAConfig("myssid", "")
	if rc := osal.WifiConnect(); rc != 0 {
		println("wifi-raw: GATE4 FAIL connect rc=", rc)
		return
	}
	if _, ok := waitEvent(osal.EvStaConnected, 20*time.Second); !ok {
		println("wifi-raw: GATE4 FAIL no STA_CONNECTED (no supplicant: expected for WPA)")
		return
	}
	println("wifi-raw: GATE4 OK (association)")

	println("wifi-raw: done")
}

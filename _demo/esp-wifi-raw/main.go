// P1b gate-1 probe: boot the fiber runtime, initialize the OSAL interrupt
// framework, then run the closed-source WiFi blob's init path. Each stage
// prints one fixed line so emu runs can be diffed gate by gate.
package main

import (
	"espwifiraw/osal"
)

func main() {
	println("wifi-raw: boot")
	osal.IntrInit()
	println("wifi-raw: intr ready")
	rc := osal.WifiInit()
	println("wifi-raw: esp_wifi_init rc=", rc)
	println("wifi-raw: done")
}

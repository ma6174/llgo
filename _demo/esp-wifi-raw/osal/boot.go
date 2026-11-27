package osal

// Boot initializes the package state the llgo dead-global-init pass prunes:
// every Go-side writer of the package maps is reached only through C
// //export trampolines (the blob's callbacks), which the Go liveness
// analysis cannot see, so the package init gets dropped and the map vars
// stay nil. main must call Boot before any WiFi call.

// Boot re-creates the pruned package variables and performs the event
// registration that used to live in the package init.
func Boot() {
	semDead = map[uintptr]bool{}
	mutexDead = map[uintptr]bool{}
	queueDead = map[uintptr]bool{}
	egDead = map[uintptr]bool{}
	timers = map[uintptr]*timerState{}
	eventHandlers = map[uintptr]map[int32][]*EventHandler{}
	eventCh = make(chan Event, 32)
	bootInstall()
	base := cWifiEventBase()
	EventRegister(base, -1, func(arg, b uintptr, id int32, data, size uintptr) {
		ev := Event{ID: id}
		if data != 0 && size > 0 {
			buf := make([]byte, size)
			queueCopy(uintptr(unsafe0(buf)), data, size)
			ev.Data = buf
		}
		select {
		case eventCh <- ev:
		default:
		}
	}, 0)
}

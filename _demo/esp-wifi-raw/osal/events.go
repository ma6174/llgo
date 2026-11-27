package osal

// Event bridge: the esp_event-shaped dispatch the library expects
// (_event_post -> registered handlers), plus the register/unregister entry
// points the C glue (esp_event_handler_register) and the demo's Go handlers
// share.

import "sync"

// EventHandler mirrors one registered handler. Exactly one of gofn / cfn is
// set: the demo registers Go closures, the C glue registers raw library
// callbacks.
type EventHandler struct {
	arg  uintptr
	cfn  uintptr
	gofn func(arg, base uintptr, id int32, data uintptr, size uintptr)
}

var (
	eventMu       sync.Mutex
	eventHandlers = map[uintptr]map[int32][]*EventHandler{}
)

// EventRegister adds a handler for (base, id); id==-1 matches any id
// (ESP_EVENT_ANY_ID).
func EventRegister(base uintptr, id int32, fn func(arg, base uintptr, id int32, data uintptr, size uintptr), arg uintptr) {
	eventMu.Lock()
	defer eventMu.Unlock()
	m := eventHandlers[base]
	if m == nil {
		m = map[int32][]*EventHandler{}
		eventHandlers[base] = m
	}
	m[id] = append(m[id], &EventHandler{arg: arg, gofn: fn})
}

// eventRegisterC is the C-glue entry (raw handler pointer).
//
//export llgo_osal_event_handler_register
func eventRegisterC(base uintptr, id int32, fn, arg uintptr) int32 {
	if fn == 0 {
		return 0
	}
	eventMu.Lock()
	defer eventMu.Unlock()
	m := eventHandlers[base]
	if m == nil {
		m = map[int32][]*EventHandler{}
		eventHandlers[base] = m
	}
	m[id] = append(m[id], &EventHandler{arg: arg, cfn: fn})
	return 0 // ESP_OK
}

//export llgo_osal_event_handler_unregister
func eventUnregister(base uintptr, id int32, fn, arg uintptr) int32 {
	eventMu.Lock()
	defer eventMu.Unlock()
	m := eventHandlers[base]
	if m == nil {
		return 0
	}
	list := m[id]
	for i, h := range list {
		if h.cfn == fn && h.arg == arg {
			m[id] = append(list[:i:i], list[i+1:]...)
			return 0
		}
	}
	return 1 // not found: ESP_ERR_NOT_FOUND shape
}

// dispatchEvent runs in its own goroutine (the library's event loop task
// equivalent). Handlers run with an "event" task identity so any library
// call they make is keyed correctly.
func dispatchEvent(base uintptr, id int32, data uintptr, size uintptr) {
	t := allocTask("event", true)
	prev := setCurTask(t.id)
	defer setCurTask(prev)

	eventMu.Lock()
	var handlers []*EventHandler
	for _, key := range []int32{id, -1} {
		handlers = append(handlers, eventHandlers[base][key]...)
	}
	eventMu.Unlock()

	for _, h := range handlers {
		if h.gofn != nil {
			h.gofn(h.arg, base, id, data, size)
		} else if h.cfn != 0 {
			callEventHandler(h.cfn, h.arg, base, id, data)
		}
	}
	if data != 0 {
		cFree(data)
	}
}

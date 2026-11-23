/*
 * Copyright (c) 2024 The XGo Authors (xgo.dev). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package runtime

import (
	"unsafe"

	"github.com/xgo-dev/llgo/runtime/abi"
	c "github.com/xgo-dev/llgo/runtime/internal/clite"
	"github.com/xgo-dev/llgo/runtime/internal/runtime/math"
)

// -----------------------------------------------------------------------------

// Slice is the runtime representation of a slice.
type Slice struct {
	data unsafe.Pointer
	len  int
	cap  int
}

// The slice bounds checks themselves live in the caller: the compiler emits the
// comparisons inline and branches to one of the entries below only when a check
// failed, which keeps the successful path free of a call. Each entry owns one
// boundsError code, so the panic text stays here instead of being spelled by the
// compiler, and each takes the failing index together with the bound it was
// compared against.
//
// The comparison is unsigned on the widened index: a negative signed index and
// an unsigned index whose high bit is set both compare above the non-negative
// bound, so one comparison covers both. signed therefore only selects the panic
// text.

// PanicSliceAcap and PanicSliceAlen report a failed upper-bound check of a
// two-index slice expression: x is the upper index and y is the slice capacity
// or, for an addressable array, its length.
func PanicSliceAcap(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSliceAcap)
}

func PanicSliceAlen(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSliceAlen)
}

// PanicSliceB reports a failed lower-bound check of a two-index slice
// expression: x is the lower index and y the upper one.
func PanicSliceB(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSliceB)
}

// PanicSlice3Acap and PanicSlice3Alen report a failed upper-bound check of a
// three-index slice expression, PanicSlice3B its failed middle check and
// PanicSlice3C its failed lower one.
func PanicSlice3Acap(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSlice3Acap)
}

func PanicSlice3Alen(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSlice3Alen)
}

func PanicSlice3B(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSlice3B)
}

func PanicSlice3C(x int64, signed bool, y int) {
	panicBounds(x, signed, y, boundsSlice3C)
}

func NewSlice3(base unsafe.Pointer, eltSize, cap, i, j, k int) (s Slice) {
	if k < 0 || k > cap {
		panic(boundsError{x: int64(k), signed: true, y: cap, code: boundsSlice3Acap})
	}
	if j < 0 || j > k {
		panic(boundsError{x: int64(j), signed: true, y: k, code: boundsSlice3B})
	}
	if i < 0 || i > j {
		panic(boundsError{x: int64(i), signed: true, y: j, code: boundsSlice3C})
	}
	return newSliceUnchecked(base, eltSize, i, j, k)
}

func newSliceUnchecked(base unsafe.Pointer, eltSize, i, j, k int) (s Slice) {
	s.len = j - i
	s.cap = k - i
	if k-i > 0 {
		s.data = c.Advance(base, i*eltSize)
	} else {
		s.data = base
	}
	return
}

// SliceAppend append elem data and returns a slice.
func SliceAppend(src Slice, data unsafe.Pointer, num, etSize int, hasPointers bool) Slice {
	return SliceAppendAligned(src, data, num, etSize, 1, hasPointers)
}

func SliceAppendAligned(src Slice, data unsafe.Pointer, num, etSize, etAlign int, hasPointers bool) Slice {
	oldLen := src.len
	src = GrowSliceAligned(src, num, etSize, etAlign, hasPointers)
	if etSize == 0 {
		return src
	}
	// An in-capacity append may take its arguments from the same backing
	// array, so the copy must stay a memmove.
	llgoMemmove(c.Advance(src.data, oldLen*etSize), data, uintptr(num*etSize))
	return src
}

// GrowSlice grows slice and returns the grown slice.
func GrowSlice(src Slice, num, etSize int, hasPointers bool) Slice {
	return GrowSliceAligned(src, num, etSize, 1, hasPointers)
}

func GrowSliceAligned(src Slice, num, etSize, etAlign int, hasPointers bool) Slice {
	oldLen := src.len
	newLen := oldLen + num
	if newLen < 0 {
		panicgrowslicelen()
	}
	if newLen > src.cap {
		newCap := nextslicecap(newLen, src.cap)
		mem, overflow := math.MulUintptr(uintptr(etSize), uintptr(newCap))
		if overflow || mem > maxAlloc {
			panicgrowslicelen()
		}
		var p unsafe.Pointer
		if hasPointers {
			// Fresh AllocZ storage is zeroed by every collector, so the newly
			// exposed slots — including the high words of widened pointer slots
			// — need no clearTypedHeaderRange pass; only copies into existing
			// memory do (Typedmemmove, typedSliceCopy, ReflectGrowSlice).
			p = AllocZAligned(mem, uintptr(etAlign))
		} else {
			p = AllocAtomicUAligned(mem, uintptr(etAlign))
			// The old prefix is overwritten by the copy below. Clear the
			// remaining capacity, including newly exposed elements: reflect.Grow
			// uses this helper without subsequently writing append arguments.
			copied := uintptr(oldLen) * uintptr(etSize)
			c.Memset(unsafe.Add(p, copied), 0, mem-copied)
		}
		if oldLen != 0 {
			llgoMemcpy(p, src.data, uintptr(oldLen*etSize))
		}
		src.data = p
		src.cap = newCap
	}
	src.len = newLen
	return src
}

// ReflectGrowSlice preserves the length and all elements in the old capacity.
// The caller has checked that len+num exceeds cap.
func ReflectGrowSlice(src Slice, num int, elem *Type) Slice {
	oldLen := src.len
	num -= src.cap - oldLen
	src.len = src.cap
	grown := GrowSliceAligned(src, num, int(elem.Size_), int(elem.Align_), elem.Pointers())
	if wideGoHeaderSlots {
		clearTypedHeaderRange(elem, grown.data, uintptr(src.len), elem.Size_)
	}
	grown.len = oldLen
	return grown
}

// nextslicecap computes the next appropriate slice length.
func nextslicecap(newLen, oldCap int) int {
	newcap := oldCap
	doublecap := newcap + newcap
	if newLen > doublecap {
		return newLen
	}

	const threshold = 256
	if oldCap < threshold {
		return doublecap
	}
	for {
		// Transition from growing 2x for small slices
		// to growing 1.25x for large slices. This formula
		// gives a smooth-ish transition between the two.
		newcap += (newcap + 3*threshold) >> 2

		// We need to check `newcap >= newLen` and whether `newcap` overflowed.
		// newLen is guaranteed to be larger than zero, hence
		// when newcap overflows then `uint(newcap) > uint(newLen)`.
		// This allows to check for both with the same comparison.
		if uint(newcap) >= uint(newLen) {
			break
		}
	}

	// Set newcap to the requested cap when
	// the newcap calculation overflowed.
	if newcap <= 0 {
		return newLen
	}
	return newcap
}

// SliceCopy copy data to slice and returns a slice.
func SliceCopy(dst Slice, data unsafe.Pointer, num int, etSize int) int {
	n := dst.len
	if n > num {
		n = num
	}
	if n > 0 {
		llgoMemmove(dst.data, data, uintptr(n*etSize))
	}
	return n
}

func MakeSlice(len, cap int, etSize int, hasPointers bool) Slice {
	mem := makeSliceMemory(len, cap, etSize)
	// This entry point requires only natural alignment. Call the raw allocator
	// directly so LLVM can inline it without the dynamic alignment dispatcher.
	var data unsafe.Pointer
	if hasPointers {
		data = AllocZ(mem)
	} else {
		data = AllocAtomicZ(mem)
	}
	return Slice{data, len, cap}
}

func MakeSliceAligned(len, cap, etSize, etAlign int, hasPointers bool) Slice {
	return Slice{allocZTyped(makeSliceMemory(len, cap, etSize), uintptr(etAlign), hasPointers), len, cap}
}

func makeSliceMemory(len, cap, etSize int) uintptr {
	mem, overflow := math.MulUintptr(uintptr(etSize), uintptr(cap))
	if overflow || mem > maxAlloc || len < 0 || len > cap {
		mem, overflow := math.MulUintptr(uintptr(etSize), uintptr(len))
		if overflow || mem > maxAlloc || len < 0 {
			panicmakeslicelen()
		}
		panicmakeslicecap()
	}
	return mem
}

// MakeSliceStackGuard is installed by scheduler backends whose goroutine
// stacks are fixed-size fibers (the baremetal fiber scheduler). MakeSliceBuffer
// runs it before trusting caller-owned stack storage, so a caller whose frame
// already crossed the fiber floor fatals deterministically instead of
// silently writing below it: the swap-point guard only checks at cooperative
// yields, and a function whose non-escaping make overflowed never yields. It
// stays nil on backends with growable or large stacks (hosted, wasm), making
// the check a single nil comparison.
var MakeSliceStackGuard func(mem uintptr, buffer unsafe.Pointer)

// MakeSliceBuffer uses caller-owned aligned storage for a valid small make.
// Fallbacks enter the original helper, preserving both its checks and the
// existing heap allocation path for callers without an escape proof.
func MakeSliceBuffer(len, cap, etSize, etAlign int, buffer unsafe.Pointer, bufferCap int, hasPointers bool) Slice {
	if buffer != nil && len >= 0 && len <= cap && cap <= bufferCap {
		mem, overflow := math.MulUintptr(uintptr(etSize), uintptr(cap))
		if !overflow && mem <= maxAlloc {
			if MakeSliceStackGuard != nil {
				MakeSliceStackGuard(mem, buffer)
			}
			c.Memset(buffer, 0, mem)
			return Slice{buffer, len, cap}
		}
	}
	return MakeSliceAligned(len, cap, etSize, etAlign, hasPointers)
}

func panicmakeslicelen() {
	panic(errorString("makeslice: len out of range"))
}

func panicmakeslicecap() {
	panic(errorString("makeslice: cap out of range"))
}

func panicgrowslicelen() {
	panic(errorString("growslice: len out of range"))
}

func SliceClear(t *abi.SliceType, s Slice) {
	c.Memset(s.data, 0, uintptr(s.len)*t.Elem.Size())
}

func unsafeslice(et *abi.Type, ptr unsafe.Pointer, len int) {
	if len < 0 {
		panic(errorString("unsafe.Slice: len out of range"))
	}

	if et.Size_ == 0 {
		if ptr == nil && len > 0 {
			panic(errorString("unsafe.Slice: ptr is nil and len is not zero"))
		}
	}

	mem, overflow := math.MulUintptr(et.Size_, uintptr(len))
	if overflow || mem > -uintptr(ptr) {
		if ptr == nil {
			panic(errorString("unsafe.Slice: ptr is nil and len is not zero"))
		}
		panic(errorString("unsafe.Slice: len out of range"))
	}
}

// -----------------------------------------------------------------------------

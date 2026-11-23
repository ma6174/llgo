// Differential fixture for slice bounds checks. Every slice expression below is
// compared against a reference that only uses integer arithmetic on the widened
// index bit patterns, so the reference cannot share a bug with the checks it
// audits: the reference decides which index pair must be reported and builds the
// expected panic text itself, including the bracket form used for negative
// signed indices.
//
// The matrix covers what the checks must agree on: two- and three-index forms,
// omitted bounds, slice, addressable-array, array-pointer and string sources,
// negative indices, indices at and past 2**31 and 2**32, unsigned indices with
// the high bit set, empty sources, and a nil array pointer (which must report
// the nil dereference, not a bounds failure). Each case folds its outcome into a
// digest, and a disagreement prints the whole case, so hosted and baremetal runs
// must print the same summary line, which expect.txt pins.
package main

import "unsafe"

// boundKind names the check that must fail first, mirroring the order the
// reference applies: upper bound, then middle, then lower.
type boundKind int

const (
	acap  boundKind = iota // two-index upper bound against capacity
	alen                   // two-index upper bound against an array length
	acap3                  // three-index upper bound against capacity
	alen3                  // three-index upper bound against an array length
	b                      // two-index lower bound against the upper index
	b3                     // three-index middle bound against the upper index
	c3                     // three-index lower bound against the middle index
)

const boundsPrefix = "runtime error: slice bounds out of range ["

// outcome is what one slice expression produced: a header, or the panic text.
// offset is the element distance of the data pointer from the source base, which
// distinguishes advancing by the low index from keeping the base.
type outcome struct {
	ok       bool
	length   int
	capacity int
	offset   int64
	text     string
}

// utoa renders the decimal digits of an unsigned index the way the runtime
// does, which prints an unsigned index without a sign even above 2**63.
func utoa(u uint64) string {
	if u == 0 {
		return "0"
	}
	var buf [20]byte
	i := len(buf)
	for u > 0 {
		i--
		buf[i] = byte('0' + u%10)
		u /= 10
	}
	return string(buf[i:])
}

// itoa renders one index: a negative signed index keeps its sign, and any other
// index prints as the unsigned value of its bits.
func itoa(x uint64, signed bool) string {
	if signed && int64(x) < 0 {
		return "-" + utoa(-x)
	}
	return utoa(x)
}

// text renders the panic text for one failed check the way the runtime does: a
// negative signed index reports only its own bracket form, without the bound.
func text(kind boundKind, x uint64, signed bool, y int64) string {
	xs, ys := itoa(x, signed), utoa(uint64(y))
	neg := signed && int64(x) < 0
	switch kind {
	case acap:
		if neg {
			return boundsPrefix + ":" + xs + "]"
		}
		return boundsPrefix + ":" + xs + "] with capacity " + ys
	case alen:
		if neg {
			return boundsPrefix + ":" + xs + "]"
		}
		return boundsPrefix + ":" + xs + "] with length " + ys
	case b:
		if neg {
			return boundsPrefix + xs + ":]"
		}
		return boundsPrefix + xs + ":" + ys + "]"
	case acap3:
		if neg {
			return boundsPrefix + "::" + xs + "]"
		}
		return boundsPrefix + "::" + xs + "] with capacity " + ys
	case alen3:
		if neg {
			return boundsPrefix + "::" + xs + "]"
		}
		return boundsPrefix + "::" + xs + "] with length " + ys
	case b3:
		if neg {
			return boundsPrefix + ":" + xs + ":]"
		}
		return boundsPrefix + ":" + xs + ":" + ys + "]"
	case c3:
		if neg {
			return boundsPrefix + xs + "::]"
		}
		return boundsPrefix + xs + ":" + ys + ":]"
	}
	return ""
}

// want2 is the reference for a two-index slice of a source with the given length
// and capacity. upperIsLen selects the "with length" text a slice of an
// addressable array reports, whose capacity is its length.
func want2(i uint64, iSigned bool, j uint64, jSigned bool, capacity int64, upperIsLen bool) outcome {
	if j > uint64(capacity) {
		kind := acap
		if upperIsLen {
			kind = alen
		}
		return outcome{text: text(kind, j, jSigned, capacity)}
	}
	if i > j {
		return outcome{text: text(b, i, iSigned, int64(j))}
	}
	o := outcome{ok: true, length: int(j - i), capacity: int(capacity - int64(i))}
	if o.capacity > 0 {
		o.offset = int64(i)
	}
	return o
}

// want3 is the reference for a three-index slice, which checks the upper bound
// first and the lower bound last.
func want3(i uint64, iSigned bool, j uint64, jSigned bool, k uint64, kSigned bool, capacity int64, upperIsLen bool) outcome {
	if k > uint64(capacity) {
		kind := acap3
		if upperIsLen {
			kind = alen3
		}
		return outcome{text: text(kind, k, kSigned, capacity)}
	}
	if j > k {
		return outcome{text: text(b3, j, jSigned, int64(k))}
	}
	if i > j {
		return outcome{text: text(c3, i, iSigned, int64(j))}
	}
	o := outcome{ok: true, length: int(j - i), capacity: int(k - i)}
	if o.capacity > 0 {
		o.offset = int64(i)
	}
	return o
}

// wantStr is the reference for a string slice, which reports a length instead of
// a capacity. Its data pointer is not audited: llgo keeps advancing an empty
// suffix past the low index while gc keeps the base, a difference this change
// does not touch.
func wantStr(i uint64, iSigned bool, j uint64, jSigned bool, length int64) outcome {
	o := want2(i, iSigned, j, jSigned, length, true)
	o.capacity = o.length
	o.offset = 0
	return o
}

// capture runs one slice expression and reports what it produced, or the panic
// text it raised.
func capture(f func() outcome) (out outcome) {
	defer func() {
		if v := recover(); v != nil {
			out = outcome{text: panicText(v)}
		}
	}()
	return f()
}

func panicText(v any) string {
	if err, ok := v.(error); ok {
		return err.Error()
	}
	if s, ok := v.(string); ok {
		return s
	}
	return "<non-error panic>"
}

func header(r []int, base uintptr) outcome {
	o := outcome{ok: true, length: len(r), capacity: cap(r)}
	o.offset = (int64(uintptr(unsafe.Pointer(unsafe.SliceData(r)))) - int64(base)) / int64(unsafe.Sizeof(int(0)))
	return o
}

func strHeader(r string) outcome {
	return outcome{ok: true, length: len(r), capacity: len(r)}
}

func intBase(s []int) uintptr { return uintptr(unsafe.Pointer(unsafe.SliceData(s))) }

// The slice expressions under test. Each one keeps its own form so that the
// omitted bounds stay omitted in the lowering.

func sliceIJ(s []int, i, j int) outcome { return header(s[i:j], intBase(s)) }
func sliceI(s []int, i int) outcome     { return header(s[i:], intBase(s)) }
func sliceJ(s []int, j int) outcome     { return header(s[:j], intBase(s)) }
func sliceFull(s []int) outcome         { return header(s[:], intBase(s)) }
func sliceIJK(s []int, i, j, k int) outcome {
	return header(s[i:j:k], intBase(s))
}
func sliceIJ64(s []int, i, j int64) outcome { return header(s[i:j], intBase(s)) }
func sliceIU(s []int, i uint64) outcome     { return header(s[i:], intBase(s)) }
func sliceIJU(s []int, i, j uint64) outcome { return header(s[i:j], intBase(s)) }
func sliceIJK64(s []int, i, j, k int64) outcome {
	return header(s[i:j:k], intBase(s))
}

func arrayIJ(a *[4]int, i, j int) outcome { return header(a[i:j], uintptr(unsafe.Pointer(a))) }
func arrayI(a *[4]int, i int) outcome     { return header(a[i:], uintptr(unsafe.Pointer(a))) }
func arrayJ(a *[4]int, j int) outcome     { return header(a[:j], uintptr(unsafe.Pointer(a))) }
func arrayFull(a *[4]int) outcome         { return header(a[:], uintptr(unsafe.Pointer(a))) }
func arrayIJK(a *[4]int, i, j, k int) outcome {
	return header(a[i:j:k], uintptr(unsafe.Pointer(a)))
}
func arrayIJ64(a *[4]int, i, j int64) outcome {
	return header(a[i:j], uintptr(unsafe.Pointer(a)))
}
func arrayIJK64(a *[4]int, i, j, k int64) outcome {
	return header(a[i:j:k], uintptr(unsafe.Pointer(a)))
}

// arrayValueIJ slices an array value, which is addressed in the caller.
func arrayValueIJ(a [4]int, i, j int) outcome {
	return header(a[i:j], uintptr(unsafe.Pointer(&a)))
}

func strIJ(s string, i, j int) outcome     { return strHeader(s[i:j]) }
func strI(s string, i int) outcome         { return strHeader(s[i:]) }
func strJ(s string, j int) outcome         { return strHeader(s[:j]) }
func strIJ64(s string, i, j int64) outcome { return strHeader(s[i:j]) }

var (
	digest uint64
	cases  int
	fails  int
)

// check compares one case against the reference and folds the outcome into the
// digest, so a silent difference cannot cancel out.
func check(label string, got, want outcome) {
	cases++
	mixed := uint64(got.length)*3 + uint64(got.capacity)*5 + uint64(got.offset)*7 + 11
	if got.ok {
		mixed += 1 << 32
	} else {
		for i := 0; i < len(got.text); i++ {
			mixed = mixed*131 + uint64(got.text[i])
		}
	}
	digest = (digest ^ mixed) * 1099511628211
	if got.ok != want.ok || got.length != want.length || got.capacity != want.capacity ||
		got.offset != want.offset || got.text != want.text {
		fails++
		println("MISMATCH", label, "got", got.ok, got.length, got.capacity, int(got.offset), got.text)
		println("MISMATCH", label, "want", want.ok, want.length, want.capacity, int(want.offset), want.text)
	}
}

func main() {
	ints := make([]int, 4, 8)
	var arr [4]int
	str := "abcdef"

	// The pair matrix: every two-index form against every source kind. The
	// values straddle both ends of each source's length and capacity.
	pairs := [...]int{-2, -1, 0, 1, 3, 4, 5, 8, 9}
	for _, i := range pairs {
		for _, j := range pairs {
			check("sliceIJ", capture(func() outcome { return sliceIJ(ints, i, j) }),
				want2(uint64(int64(i)), true, uint64(int64(j)), true, 8, false))
			check("arrayIJ", capture(func() outcome { return arrayIJ(&arr, i, j) }),
				want2(uint64(int64(i)), true, uint64(int64(j)), true, 4, true))
			check("arrayValueIJ", capture(func() outcome { return arrayValueIJ(arr, i, j) }),
				want2(uint64(int64(i)), true, uint64(int64(j)), true, 4, true))
			check("strIJ", capture(func() outcome { return strIJ(str, i, j) }),
				wantStr(uint64(int64(i)), true, uint64(int64(j)), true, 6))
		}
		check("sliceI", capture(func() outcome { return sliceI(ints, i) }),
			want2(uint64(int64(i)), true, 4, true, 8, false))
		check("sliceJ", capture(func() outcome { return sliceJ(ints, i) }),
			want2(0, true, uint64(int64(i)), true, 8, false))
		check("arrayI", capture(func() outcome { return arrayI(&arr, i) }),
			want2(uint64(int64(i)), true, 4, true, 4, true))
		check("arrayJ", capture(func() outcome { return arrayJ(&arr, i) }),
			want2(0, true, uint64(int64(i)), true, 4, true))
		check("strI", capture(func() outcome { return strI(str, i) }),
			wantStr(uint64(int64(i)), true, 6, true, 6))
		check("strJ", capture(func() outcome { return strJ(str, i) }),
			wantStr(0, true, uint64(int64(i)), true, 6))
	}
	check("sliceFull", capture(func() outcome { return sliceFull(ints) }),
		want2(0, true, 4, true, 8, false))
	check("arrayFull", capture(func() outcome { return arrayFull(&arr) }),
		want2(0, true, 4, true, 4, true))

	// Three-index forms: the upper bound is checked first, so an expression that
	// fails two checks must report the upper one.
	triples := [...][3]int{
		{-1, 0, 0}, {0, -1, 0}, {0, 0, -1},
		{0, 0, 0}, {0, 0, 4}, {0, 0, 5}, {0, 4, 4}, {0, 4, 8}, {3, 3, 3},
		{4, 4, 4}, {5, 5, 5}, {2, 1, 3}, {3, 1, 2}, {1, 3, 2}, {1, 2, 3},
		{0, 9, 8}, {9, 0, 0}, {0, 9, 0}, {2, 3, 4}, {2, 3, 9},
	}
	for _, t := range triples {
		i, j, k := t[0], t[1], t[2]
		check("sliceIJK", capture(func() outcome { return sliceIJK(ints, i, j, k) }),
			want3(uint64(int64(i)), true, uint64(int64(j)), true, uint64(int64(k)), true, 8, false))
		check("arrayIJK", capture(func() outcome { return arrayIJK(&arr, i, j, k) }),
			want3(uint64(int64(i)), true, uint64(int64(j)), true, uint64(int64(k)), true, 4, true))
	}

	// Indices wider than the 32-bit target word: the comparison must see the
	// whole value, so 2**32 cannot alias 0 and 2**31 cannot alias a negative
	// native int.
	wide := [...]int64{-1, 0, 3, 1<<31 - 1, 1 << 31, 1<<32 - 1, 1 << 32, 1<<32 + 1, 1 << 40, 1<<63 - 1}
	for _, i := range wide {
		for _, j := range [...]int64{0, 4, 1 << 32} {
			check("sliceIJ64", capture(func() outcome { return sliceIJ64(ints, i, j) }),
				want2(uint64(i), true, uint64(j), true, 8, false))
			check("arrayIJ64", capture(func() outcome { return arrayIJ64(&arr, i, j) }),
				want2(uint64(i), true, uint64(j), true, 4, true))
			check("strIJ64", capture(func() outcome { return strIJ64(str, i, j) }),
				wantStr(uint64(i), true, uint64(j), true, 6))
		}
		check("sliceIJK64", capture(func() outcome { return sliceIJK64(ints, 0, 4, i) }),
			want3(0, true, 4, true, uint64(i), true, 8, false))
		check("arrayIJK64", capture(func() outcome { return arrayIJK64(&arr, 0, 4, i) }),
			want3(0, true, 4, true, uint64(i), true, 4, true))
	}

	// Unsigned indices: a value with the high bit set must fail the same
	// comparison a negative signed index fails, and report without a sign.
	unsigned := [...]uint64{0, 1, 4, 1 << 31, 1 << 32, 1 << 40, 1<<63 + 1, 1<<64 - 1}
	for _, i := range unsigned {
		check("sliceIU", capture(func() outcome { return sliceIU(ints, i) }),
			want2(uint64(i), false, 4, true, 8, false))
		check("sliceIJU", capture(func() outcome { return sliceIJU(ints, i, 4) }),
			want2(uint64(i), false, 4, true, 8, false))
	}

	// A source with no capacity at all: only the zero indices are inside.
	var empty []int
	check("emptyIJ", capture(func() outcome { return sliceIJ(empty, 0, 0) }),
		want2(0, true, 0, true, 0, false))
	check("emptyIJK", capture(func() outcome { return sliceIJK(empty, 0, 0, 1) }),
		want3(0, true, 0, true, 1, true, 0, false))

	// A nil array pointer must report the dereference even when the bounds are
	// also invalid: the nil check stays ahead of the bounds checks.
	var nilArray *[4]int
	check("nilArray", capture(func() outcome { return arrayIJ(nilArray, 3, 1) }), outcome{
		text: "runtime error: invalid memory address or nil pointer dereference",
	})

	println("slicebounds ok cases=", cases, "digest=", digest, "fails=", fails)
}

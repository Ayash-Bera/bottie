//go:build linux && cgo

package textmask

import (
	"bytes"
	"strings"
	"testing"
	"time"
)

func native(t *testing.T) *nativeRenderer {
	t.Helper()
	r, err := newRenderer()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(r.Close)
	return r.(*nativeRenderer)
}

func TestShapingCoverageInkBoundsAndCanonicalEquivalence(t *testing.T) {
	r := native(t)
	for _, text := range []string{"ক্ষিতিজ", "শ্রদ্ধা", "কৃষ্ণ", "চাঁদের আলো", "কৌতূহল", "夜に駆ける", "カタカナ・ひらがな", "東京 / orbit 2026", "音楽（ライブ）", "パーティー", "ক্‌ষ ক্‍ষ"} {
		for _, slot := range Slots {
			data, info, err := r.renderLine(text, slot)
			if err != nil || len(data) != slot.Width*slot.Height/2 {
				t.Fatalf("%q: %v", text, err)
			}
			occupied := 0
			minX, maxX, minY, maxY := slot.Width, -1, slot.Height, -1
			for y := range slot.Height {
				for x := range slot.Width {
					a := data[(y*slot.Width+x)/2]
					if x%2 == 0 {
						a >>= 4
					} else {
						a &= 15
					}
					if a != 0 {
						occupied++
						minX = min(minX, x)
						maxX = max(maxX, x)
						minY = min(minY, y)
						maxY = max(maxY, y)
					}
				}
			}
			if occupied == 0 || minX < 0 || maxX >= slot.Width || minY < 0 || maxY >= slot.Height || abs(minX+maxX-(slot.Width-1)) > 3 || info.FontPixels < 11 {
				t.Fatalf("ink bounds/centering %q %v: %d,%d,%d,%d font%d", text, slot, minX, maxX, minY, maxY, info.FontPixels)
			}
		}
	}
	for _, pair := range [][2]string{{"কো", "কো"}, {"が", "が"}, {"東京 ORBIT", "東京 orbit"}, {"বাংলা ARTIST", "বাংলা artist"}} {
		a, err := r.Render(pair[0], Slots[2])
		if err != nil {
			t.Fatal(err)
		}
		b, err := r.Render(pair[1], Slots[2])
		if err != nil || !bytes.Equal(a, b) {
			t.Fatalf("equivalent display %q/%q", pair[0], pair[1])
		}
	}
	if _, err := r.Render("unmapped\U0010ffff", Slots[0]); err != ErrCoverage {
		t.Fatalf("unsupported glyph: %v", err)
	}
	joined, _ := r.Render("ক্ষ", Slots[0])
	separated, err := r.Render("ক্‌ষ", Slots[0])
	if err != nil || bytes.Equal(joined, separated) {
		t.Fatal("ZWNJ lost during shaping")
	}
}

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}

func TestEllipsisKeepsConjunctClustersAndHandlesProducerBoundary(t *testing.T) {
	r := native(t)
	for width := 20; width <= 100; width += 2 {
		_, info, err := r.renderLine("ক্ষিতিজ", Slot{Width: width, Height: 18})
		if err != nil {
			t.Fatal(err)
		}
		if info.RetainedBytes != 0 && info.RetainedBytes != 12 && info.RetainedBytes != 18 && info.RetainedBytes != 21 {
			t.Fatalf("split conjunct at %d", info.RetainedBytes)
		}
	}
	for _, n := range []int{252, 253, 254, 255, 256} {
		// Use a valid exact byte length ending in a complete conjunct or a possible cut suffix.
		value := strings.Repeat(" ", n-len("ক্")) + "ক্"
		data, info, err := r.renderLine(value, Slots[0])
		if err != nil || len(data) != 1908 {
			t.Fatalf("boundary %d: %v", n, err)
		}
		if n >= 253 && info.RetainedBytes >= n {
			t.Fatal("potential clipped terminal cluster retained")
		}
	}
	for _, value := range []string{strings.Repeat("a", 257), string([]byte{0xff}), "a\x00b"} {
		if _, err := r.Render(value, Slots[0]); err == nil {
			t.Fatal("invalid renderer input accepted")
		}
	}
}

func TestBoundedColdAndWarmRendering(t *testing.T) {
	start := time.Now()
	r := native(t)
	cold := time.Since(start)
	start = time.Now()
	for range 20 {
		for _, text := range []string{"ক্ষিতিজ শ্রদ্ধা কৃষ্ণ চাঁদের আলো কৌতূহল", "夜に駆ける カタカナ・ひらがな 東京音楽"} {
			if _, err := r.Render(text, Slots[0]); err != nil {
				t.Fatal(err)
			}
		}
	}
	t.Logf("startup=%s warm40=%s", cold, time.Since(start))
	if time.Since(start) > 5*time.Second {
		t.Fatal("bounded text batch exceeded five seconds")
	}
}

func TestTallTerminalClusterKeepsEarlierFittingPrefix(t *testing.T) {
	r := native(t)
	value := "....東京"
	_, info, err := r.renderLine(value, Slot{Width: 216, Height: 8})
	if err != nil || info.RetainedBytes == 0 || info.RetainedBytes > len("....") {
		t.Fatalf("earlier fitting prefix lost: retained=%d err=%v", info.RetainedBytes, err)
	}
}

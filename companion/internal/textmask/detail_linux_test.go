//go:build linux && cgo

package textmask

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

func TestDetailRealShapingAndTailPolicy(t *testing.T) {
	r := native(t)
	for _, value := range []string{"ক্ষিতিজ শ্রদ্ধা কৃষ্ণ চাঁদের আলো কৌতূহল", "夜に駆ける カタカナ・ひらがな 東京 / ORBIT 2026", strings.Repeat("M", 252), strings.Repeat("শ্রদ্ধা ", 10), strings.Repeat("音楽（ライブ）", 6)} {
		planes, flags, err := r.RenderDetail(context.Background(), value)
		if err != nil || len(planes) < 1 || len(planes) > 4 || flags & ^3 != 0 {
			t.Fatalf("detail %q: pages%d flags%d %v", value, len(planes), flags, err)
		}
		if value == strings.Repeat("M", 252) && (len(planes) != 4 || flags != 2) {
			t.Fatal("four-page limit did not report truncation")
		}
		for _, plane := range planes {
			if len(plane) != DetailPayload || bytes.Equal(plane, make([]byte, DetailPayload)) {
				t.Fatal("empty or oversized page")
			}
		}
	}
	for _, pair := range [][2]string{{"কো", "কো"}, {"が", "が"}, {"東京 ORBIT", "東京 orbit"}, {"বাংলা ARTIST", "বাংলা artist"}} {
		a, _, e := r.RenderDetail(context.Background(), pair[0])
		if e != nil {
			t.Fatal(e)
		}
		b, _, e := r.RenderDetail(context.Background(), pair[1])
		if e != nil || !bytes.Equal(bytes.Join(a, nil), bytes.Join(b, nil)) {
			t.Fatal("canonical/case detail changed")
		}
	}
	a, _, _ := r.RenderDetail(context.Background(), "ক্ষ")
	b, _, _ := r.RenderDetail(context.Background(), "ক্‌ষ")
	if bytes.Equal(bytes.Join(a, nil), bytes.Join(b, nil)) {
		t.Fatal("joiner lost")
	}
	for _, length := range []int{252, 253, 254, 255, 256} {
		value := strings.Repeat(" ", length-len("ক্")) + "ক্"
		_, flags, e := r.RenderDetail(context.Background(), value)
		if e != nil || (flags&1 != 0) != (length >= 253) {
			t.Fatalf("tail%d flags%d %v", length, flags, e)
		}
	}
	for _, value := range []string{"", strings.Repeat("a", 257), "nul\x00", string([]byte{255})} {
		if _, _, e := r.RenderDetail(context.Background(), value); e == nil {
			t.Fatal("invalid detail input")
		}
	}
	if _, _, e := r.RenderDetail(context.Background(), "unmapped\U0010ffff"); e != ErrCoverage {
		t.Fatal("coverage")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, e := r.RenderDetail(ctx, "unchanged"); e == nil {
		t.Fatal("canceled detail rendered")
	}
}

func BenchmarkDetailLayout(b *testing.B) {
	base, err := newRenderer()
	if err != nil {
		b.Fatal(err)
	}
	r := base.(*nativeRenderer)
	defer r.Close()
	for _, tc := range []struct{ name, value string }{
		{"bengali", "ক্ষিতিজ শ্রদ্ধা কৃষ্ণ চাঁদের আলো কৌতূহল"},
		{"japanese", "夜に駆ける カタカナ・ひらがな 東京 / ORBIT 2026"},
		{"four_pages", strings.Repeat("M", 252)},
	} {
		b.Run(tc.name, func(b *testing.B) {
			b.ReportAllocs()
			for b.Loop() {
				if _, _, err := r.RenderDetail(context.Background(), tc.value); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}

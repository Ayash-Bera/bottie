package textmask

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"bottie/companion/internal/models"
)

type fakeRenderer struct{ calls int }

func (r *fakeRenderer) Identity() string { return "test-font-layout" }
func (r *fakeRenderer) Close()           {}
func (r *fakeRenderer) Render(value string, s Slot) ([]byte, error) {
	r.calls++
	if value == "unsupported🦋" {
		return nil, ErrCoverage
	}
	return bytes.Repeat([]byte{byte(len(value))}, s.Width*s.Height/2), nil
}

func example() models.Playback {
	p := models.Empty("playing")
	p.TrackID, p.Title, p.Artist, p.Album = "fixture", "ক্ষিতিজ", "夜に駆ける", "বাংলা"
	p.Context = models.Context{Type: "playlist", Name: "日本語"}
	return p
}

func TestClockDoesNotChangeTextIdentity(t *testing.T) {
	r := &fakeRenderer{}
	s := newStore(r)
	p := example()
	d, err := s.Prepare(context.Background(), p)
	if err != nil || d == nil {
		t.Fatal(err)
	}
	calls := r.calls
	for _, offset := range []int{-840, 0, 330, 840} {
		p.Clock = &models.Clock{UnixMS: models.MinClockMS + int64(offset+840)*60000, UTCOffsetMin: offset}
		next, err := s.Prepare(context.Background(), p)
		if err != nil || next == nil || next.URL != d.URL || r.calls != calls {
			t.Fatal("clock regenerated text bundle")
		}
	}
}

func TestNativePredicate(t *testing.T) {
	for _, tt := range []struct {
		text string
		want bool
	}{
		{"ASCII caps 2026", false}, {"‘song’ “album” ‐‑‒–—", false},
		{"বাংলা", true}, {"日本語", true}, {"ellipsis…", true}, {"é", true}, {"", false},
	} {
		if NeedsMask(tt.text) != tt.want {
			t.Errorf("predicate %q", tt.text)
		}
	}
}

func TestBundleFlagsLengthHashAndOwnership(t *testing.T) {
	r := &fakeRenderer{}
	s := newStore(r)
	p := example()
	d, err := s.Prepare(context.Background(), p)
	if err != nil || d == nil {
		t.Fatalf("prepare: %v", err)
	}
	b, ok := s.Get(d.URL)
	if !ok || len(b) != MaxBytes || string(b[:6]) != "BTXT\x01\x01" || b[6] != 15 || b[7] != 1 || binary.LittleEndian.Uint16(b[8:10]) != 240 || binary.LittleEndian.Uint16(b[10:12]) != 296 || binary.LittleEndian.Uint32(b[12:16]) != 8478 {
		t.Fatal("invalid full bundle")
	}
	if d.URL != fmt.Sprintf("/v1/text/%x", sha256.Sum256(b)) || d.Layout != 1 {
		t.Fatal("content identity")
	}
	b[16] ^= 255
	again, _ := s.Get(d.URL)
	if bytes.Equal(b, again) {
		t.Fatal("cache exposed mutable storage")
	}
	p.Context = models.Context{Type: "album", Name: "ignored"}
	p.Title, p.Artist = "ASCII", "ASCII"
	d, err = s.Prepare(context.Background(), p)
	if err != nil || d == nil {
		t.Fatal(err)
	}
	b, _ = s.Get(d.URL)
	if b[6] != 1 || len(b) != 16+1908 {
		t.Fatal("album-only first-plane semantics")
	}
	p.Album = "ASCII"
	if d, err = s.Prepare(context.Background(), p); err != nil || d != nil {
		t.Fatal("ASCII descriptor")
	}
	p = example()
	p.State = "unavailable"
	if d, err = s.Prepare(context.Background(), p); err != nil || d != nil {
		t.Fatal("unavailable descriptor")
	}
}

func TestCacheMetadataBindingConcurrencyExpiryAndEviction(t *testing.T) {
	r := &fakeRenderer{}
	s := newStore(r)
	now := time.Unix(1000, 0)
	s.now = func() time.Time { return now }
	p := example()
	var wg sync.WaitGroup
	for range 8 {
		wg.Go(func() {
			if _, err := s.Prepare(context.Background(), p); err != nil {
				t.Error(err)
			}
		})
	}
	wg.Wait()
	if r.calls != 4 {
		t.Fatalf("duplicate render count %d", r.calls)
	}
	d, _ := s.Prepare(context.Background(), p)
	p.ProgressMS, p.UpdatedAtMS = 100, 200
	s.Prepare(context.Background(), p)
	if r.calls != 4 {
		t.Fatal("progress invalidated text")
	}
	for i, field := range []*string{&p.TrackID, &p.Title, &p.Artist, &p.Album, &p.Context.Name, &p.Context.Type} {
		*field += "a"
		if _, err := s.Prepare(context.Background(), p); err != nil {
			t.Fatal(err)
		}
		if len(s.items) != i+2 {
			t.Fatal("metadata identity not invalidated")
		}
	}
	now = now.Add(TTL)
	if _, ok := s.Get(d.URL); ok {
		t.Fatal("expired text returned")
	}
	for i := range Capacity + 3 {
		p.TrackID = fmt.Sprint(i)
		s.Prepare(context.Background(), p)
	}
	if len(s.items) != Capacity {
		t.Fatal("unbounded cache")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := s.Prepare(ctx, p); err == nil {
		t.Fatal("ignored cancellation")
	}
	for _, path := range []string{"/v1/text/", "/v1/text/" + strings.Repeat("A", 64), "/v1/text/" + strings.Repeat("a", 65), "/v1/text/../control", "https://example.invalid/v1/text/" + strings.Repeat("a", 64)} {
		if _, ok := s.Get(path); ok {
			t.Fatal("invalid path accepted")
		}
	}
}

func TestUnsupportedFieldCannotAdvertiseAFalseMask(t *testing.T) {
	r := &fakeRenderer{}
	s := newStore(r)
	p := models.Empty("playing")
	p.Title = "unsupported🦋"
	for range 2 {
		if d, err := s.Prepare(context.Background(), p); err != nil || d != nil {
			t.Fatal("unsupported descriptor")
		}
	}
	if r.calls != 1 {
		t.Fatal("negative result not cached")
	}
}

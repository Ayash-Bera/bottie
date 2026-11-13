package textmask

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"bottie/companion/internal/models"
)

type fakeDetailRenderer struct {
	calls   atomic.Int32
	started chan struct{}
	release chan struct{}
}

func (r *fakeDetailRenderer) Identity() string { return "test-detail" }
func (r *fakeDetailRenderer) Close()           {}
func (r *fakeDetailRenderer) RenderDetail(ctx context.Context, value string) ([][]byte, int, error) {
	r.calls.Add(1)
	if value == "held" || value == "late" {
		select {
		case r.started <- struct{}{}:
		default:
		}
		if value == "late" {
			<-r.release
		} else {
			select {
			case <-ctx.Done():
				return nil, 0, ctx.Err()
			case <-r.release:
			}
		}
	}
	if value == "bad" {
		return nil, 0, fmt.Errorf("synthetic failure")
	}
	planes := make([][]byte, 4)
	for i := range planes {
		planes[i] = bytes.Repeat([]byte{byte(len(value) + i)}, DetailPayload)
	}
	return planes, 0, nil
}

func TestDetailLateNativeReturnAndIdentityChurn(t *testing.T) {
	r := &fakeDetailRenderer{started: make(chan struct{}, 1), release: make(chan struct{})}
	s := newDetails(context.Background(), r)
	defer s.Close()
	defer func() {
		select {
		case <-r.release:
		default:
			close(r.release)
		}
	}()
	old := models.Playback{State: "playing", TrackID: "old", Title: "late"}
	s.Observe(old)
	<-r.started
	p := old
	for i := range 1000 {
		p.Title = fmt.Sprintf("latest%d", i)
		if s.Observe(p) != nil {
			t.Fatal("unprepared descriptor")
		}
	}
	if r.calls.Load() != 1 {
		t.Fatal("unbounded concurrent rendering")
	}
	close(r.release)
	d := waitDetail(t, s, p)
	if d.ID != s.identity(p.Bounded()) || r.calls.Load() != 2 {
		t.Fatal("late native work replaced the newest pending identity")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, entry := range s.items {
		if entry.id == s.identity(old.Bounded()) {
			t.Fatal("canceled native result cached")
		}
	}
}
func detailSnapshot(title string) models.Playback {
	return models.Playback{State: "playing", TrackID: "test", Title: title, Album: "album", Artist: "artist", Context: models.Context{Type: "playlist", Name: "playlist"}}
}
func waitDetail(t *testing.T, s *DetailStore, p models.Playback) *models.Details {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if d := s.Observe(p); d != nil {
			return d
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("detail job did not complete")
	return nil
}

func TestDetailAsyncCancellationIdentityAndBounds(t *testing.T) {
	r := &fakeDetailRenderer{started: make(chan struct{}, 1), release: make(chan struct{})}
	s := newDetails(context.Background(), r)
	defer s.Close()
	old := detailSnapshot("held")
	if s.Observe(old) != nil {
		t.Fatal("uncached descriptor")
	}
	<-r.started
	start := time.Now()
	if s.Observe(old) != nil || time.Since(start) > 50*time.Millisecond {
		t.Fatal("held rendering blocked metadata")
	}
	p := detailSnapshot("new")
	d := waitDetail(t, s, p)
	if d.ID == s.identity(old) {
		t.Fatal("late old descriptor published")
	}
	for field, f := range d.Fields() {
		if f == nil || len(f.Pages) != 4 {
			t.Fatal("field missing")
		}
		for page, id := range f.Pages {
			data, ok := s.Get("/v1/detail/" + id)
			if !ok || len(data) != DetailMaxBytes || string(data[:7]) != "BTDT\x01\x01\x01" || data[7] != byte(field) || data[8] != byte(page) || data[9] != 4 || data[10] != 0 || data[11] != 0 || binary.LittleEndian.Uint32(data[12:]) != DetailPayload || fmt.Sprintf("%x", sha256.Sum256(data)) != id {
				t.Fatal("detail wire identity")
			}
			data[16] ^= 255
			next, _ := s.Get("/v1/detail/" + id)
			if bytes.Equal(data, next) {
				t.Fatal("response aliases cache")
			}
		}
	}
	calls := r.calls.Load()
	p.Clock = &models.Clock{UnixMS: 1700000000000, UTCOffsetMin: 330}
	p.ControlTarget = strings.Repeat("a", 32)
	p.ProgressMS = 100
	if next := s.Observe(p); next == nil || next.ID != d.ID || r.calls.Load() != calls {
		t.Fatal("cosmetic update rerendered")
	}
	for i := range 8 {
		p.Title = fmt.Sprint("title", i)
		waitDetail(t, s, p)
	}
	s.mu.Lock()
	total := 0
	for _, item := range s.items {
		for _, data := range item.pages {
			total += len(data)
		}
	}
	count := len(s.items)
	s.mu.Unlock()
	if count > 4 || total > 4*16*DetailMaxBytes {
		t.Fatal("cache bound")
	}
	if _, ok := s.Get("/v1/detail/" + d.Title.Pages[0]); ok {
		t.Fatal("evicted identity retained")
	}
	if s.Observe(models.Empty("unavailable")) != nil {
		t.Fatal("offline descriptor")
	}
}

func TestDetailTTLFailureAndShutdown(t *testing.T) {
	r := &fakeDetailRenderer{started: make(chan struct{}, 1), release: make(chan struct{})}
	s := newDetails(context.Background(), r)
	p := detailSnapshot("held")
	s.Observe(p)
	<-r.started
	done := make(chan struct{})
	go func() { s.Close(); close(done) }()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("close did not cancel work")
	}
	if s.Observe(detailSnapshot("next")) != nil {
		t.Fatal("closed store restarted")
	}
	r = &fakeDetailRenderer{}
	s = newDetails(context.Background(), r)
	defer s.Close()
	p = detailSnapshot("bad")
	s.Observe(p)
	deadline := time.Now().Add(time.Second)
	for {
		s.mu.Lock()
		finished := s.latest == nil
		s.mu.Unlock()
		if finished {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("failure did not finish")
		}
		time.Sleep(time.Millisecond)
	}
	before := r.calls.Load()
	for range 10 {
		s.Observe(p)
	}
	if r.calls.Load() != before {
		t.Fatal("failed field retried without backoff")
	}
	s.mu.Lock()
	for i := range s.items {
		s.items[i].expires = time.Now().Add(-time.Second)
	}
	s.mu.Unlock()
	p.Title = "recovered"
	waitDetail(t, s, p)
}

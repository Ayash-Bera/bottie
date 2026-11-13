package textmask

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"

	"bottie/companion/internal/models"
)

const DetailWidth, DetailHeight = 216, 96
const DetailPayload = DetailWidth * DetailHeight / 2
const DetailMaxBytes = 16 + DetailPayload
const DetailCapacity = 4

type detailRenderer interface {
	RenderDetail(context.Context, string) ([][]byte, int, error)
	Identity() string
	Close()
}

type detailEntry struct {
	id         string
	descriptor *models.Details
	pages      map[string][]byte
	expires    time.Time
}

type detailJob struct {
	id     string
	labels [4]string
	ctx    context.Context
	cancel context.CancelFunc
}

type DetailStore struct {
	mu      sync.Mutex
	render  detailRenderer
	items   []detailEntry
	latest  *detailJob
	current string
	wake    chan struct{}
	ctx     context.Context
	cancel  context.CancelFunc
	workers sync.WaitGroup
	now     func() time.Time
}

func NewDetails(ctx context.Context) (*DetailStore, error) {
	r, err := newRenderer()
	if err != nil {
		return nil, err
	}
	d, ok := r.(detailRenderer)
	if !ok {
		r.Close()
		return nil, fmt.Errorf("detail shaping is unavailable on this platform")
	}
	return newDetails(ctx, d), nil
}

func newDetails(ctx context.Context, r detailRenderer) *DetailStore {
	ctx, cancel := context.WithCancel(ctx)
	s := &DetailStore{render: r, ctx: ctx, cancel: cancel, wake: make(chan struct{}, 1), now: time.Now}
	s.workers.Go(s.run)
	return s
}

func (s *DetailStore) Close() {
	s.cancel()
	s.workers.Wait()
	s.render.Close()
}

func DetailLabels(p models.Playback) [4]string {
	labels := [4]string{p.Title, p.Artist, p.Album, ""}
	if p.Context.Type == "playlist" {
		labels[3] = p.Context.Name
	}
	return labels
}

func (s *DetailStore) identity(p models.Playback) string {
	data, _ := json.Marshal([]string{"detail1/216x96/4pages/near253-tail1", s.render.Identity(), p.TrackID, p.Title, p.Artist, p.Album, p.Context.Type, p.Context.Name})
	id := sha256.Sum256(data)
	return hex.EncodeToString(id[:])
}

func (s *DetailStore) expireLocked() {
	items := s.items[:0]
	for _, item := range s.items {
		if s.now().Before(item.expires) {
			items = append(items, item)
		}
	}
	clear(s.items[len(items):])
	s.items = items
}

// A dedicated renderer keeps optional pagination out of the line-mask request path.
func (s *DetailStore) Observe(p models.Playback) *models.Details {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ctx.Err() != nil {
		return nil
	}
	if p.State != "playing" && p.State != "paused" {
		s.current = ""
		if s.latest != nil {
			s.latest.cancel()
			s.latest = nil
		}
		return nil
	}
	p = p.Bounded()
	id := s.identity(p)
	if s.current != id && s.latest != nil {
		s.latest.cancel()
		s.latest = nil
	}
	s.current = id
	s.expireLocked()
	for _, item := range s.items {
		if item.id == id {
			if item.descriptor == nil {
				return nil
			}
			return item.descriptor.Bounded()
		}
	}
	if s.latest != nil && s.latest.id == id && s.latest.ctx.Err() == nil {
		return nil
	}
	ctx, cancel := context.WithCancel(s.ctx)
	s.latest = &detailJob{id: id, labels: DetailLabels(p), ctx: ctx, cancel: cancel}
	select {
	case s.wake <- struct{}{}:
	default:
	}
	return nil
}

func (s *DetailStore) run() {
	for {
		select {
		case <-s.ctx.Done():
			return
		case <-s.wake:
		}
		s.mu.Lock()
		job := s.latest
		s.mu.Unlock()
		if job == nil || job.ctx.Err() != nil {
			continue
		}
		entry, err := s.prepare(job)
		s.mu.Lock()
		if s.latest == job && s.current == job.id && job.ctx.Err() == nil {
			s.expireLocked()
			if len(s.items) == DetailCapacity {
				s.items[0] = detailEntry{}
				s.items = s.items[1:]
			}
			if err != nil {
				entry = detailEntry{id: job.id, expires: s.now().Add(30 * time.Second)}
			}
			s.items = append(s.items, entry)
			s.latest = nil
		}
		job.cancel()
		s.mu.Unlock()
	}
}

func (s *DetailStore) prepare(job *detailJob) (detailEntry, error) {
	entry := detailEntry{id: job.id, descriptor: &models.Details{Layout: 1, ID: job.id}, pages: make(map[string][]byte), expires: s.now().Add(TTL)}
	for index, label := range job.labels {
		if label == "" {
			continue
		}
		if err := job.ctx.Err(); err != nil {
			return detailEntry{}, err
		}
		planes, flags, err := s.render.RenderDetail(job.ctx, label)
		if err == ErrCoverage {
			continue
		}
		if err != nil {
			return detailEntry{}, fmt.Errorf("shape detail field: %w", err)
		}
		if len(planes) == 0 || len(planes) > 4 || flags < 0 || flags > 3 {
			return detailEntry{}, fmt.Errorf("invalid detail renderer bounds")
		}
		field := &models.DetailField{Flags: flags}
		for page, plane := range planes {
			if len(plane) != DetailPayload {
				return detailEntry{}, fmt.Errorf("invalid detail plane size")
			}
			data := make([]byte, DetailMaxBytes)
			copy(data, "BTDT\x01\x01\x01")
			data[7], data[8], data[9], data[10] = byte(index), byte(page), byte(len(planes)), byte(flags)
			binary.LittleEndian.PutUint32(data[12:16], DetailPayload)
			copy(data[16:], plane)
			digest := sha256.Sum256(data)
			id := hex.EncodeToString(digest[:])
			entry.pages[id] = data
			field.Pages = append(field.Pages, id)
		}
		entry.descriptor.SetField(index, field)
	}
	if len(entry.pages) == 0 {
		entry.descriptor = nil
	}
	return entry, nil
}

func (s *DetailStore) Get(path string) ([]byte, bool) {
	id, ok := strings.CutPrefix(path, "/v1/detail/")
	if !ok || !models.ValidDigest(id) {
		return nil, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expireLocked()
	for _, entry := range s.items {
		if data, ok := entry.pages[id]; ok {
			return append([]byte(nil), data...), true
		}
	}
	return nil, false
}

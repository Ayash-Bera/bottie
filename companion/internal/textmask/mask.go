package textmask

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"bottie/companion/internal/models"
)

const MaxBytes = 8494
const Capacity = 8
const TTL = 5 * time.Minute

var ErrCoverage = errors.New("text has unsupported glyphs")

type Slot struct{ X, Y, Width, Height int }

var Slots = [4]Slot{{14, 6, 212, 18}, {19, 27, 202, 18}, {12, 184, 216, 26}, {12, 212, 216, 18}}

type renderer interface {
	Render(string, Slot) ([]byte, error)
	Identity() string
	Close()
}

type entry struct {
	key  [32]byte
	id   string
	data []byte
	at   time.Time
}

type Store struct {
	render renderer
	work   chan struct{}
	mu     sync.Mutex
	items  []entry
	now    func() time.Time
}

func New() (*Store, error) {
	r, err := newRenderer()
	if err != nil {
		return nil, fmt.Errorf("initialize metadata text: %w", err)
	}
	return newStore(r), nil
}

func newStore(r renderer) *Store {
	return &Store{render: r, work: make(chan struct{}, 1), now: time.Now}
}

func (s *Store) Close() { s.render.Close() }

// NeedsMask mirrors the device's punctuation normalization before its ASCII font.
func NeedsMask(value string) bool {
	for _, r := range value {
		if r < 128 || r == 0xa0 || r == 0x2018 || r == 0x2019 || r == 0x201c || r == 0x201d || r >= 0x2010 && r <= 0x2014 {
			continue
		}
		return true
	}
	return false
}

func Labels(p models.Playback) [4]string {
	labels := [4]string{p.Album, "", p.Title, p.Artist}
	if p.Context.Type == "playlist" && p.Context.Name != "" {
		labels[0], labels[1] = p.Context.Name, p.Album
	}
	return labels
}

func PathID(path string) (string, bool) {
	id, ok := strings.CutPrefix(path, "/v1/text/")
	if !ok || len(id) != 64 || strings.IndexFunc(id, func(r rune) bool { return !(r >= '0' && r <= '9' || r >= 'a' && r <= 'f') }) >= 0 {
		return "", false
	}
	return id, true
}

func (s *Store) Prepare(ctx context.Context, p models.Playback) (*models.TextMask, error) {
	if p.State != "playing" && p.State != "paused" {
		return nil, nil
	}
	labels := Labels(p)
	needed := false
	for _, label := range labels {
		needed = needed || NeedsMask(label)
	}
	if !needed {
		return nil, nil
	}
	keyBytes, _ := json.Marshal([]string{"layout1", s.render.Identity(), p.TrackID, p.Title, p.Artist, p.Album, p.Context.Type, p.Context.Name})
	key := sha256.Sum256(keyBytes)
	select {
	case s.work <- struct{}{}:
		defer func() { <-s.work }()
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	s.mu.Lock()
	for _, item := range s.items {
		if item.key == key && s.now().Sub(item.at) < TTL {
			s.mu.Unlock()
			return descriptor(item.id), nil
		}
	}
	s.mu.Unlock()
	data := make([]byte, 16, MaxBytes)
	copy(data, "BTXT\x01\x01\x00\x01")
	binary.LittleEndian.PutUint16(data[8:10], 240)
	binary.LittleEndian.PutUint16(data[10:12], 296)
	for i, label := range labels {
		if !NeedsMask(label) {
			continue
		}
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		plane, err := s.render.Render(label, Slots[i])
		if errors.Is(err, ErrCoverage) {
			continue
		}
		if err != nil {
			return nil, fmt.Errorf("shape metadata line: %w", err)
		}
		if len(plane) != Slots[i].Width*Slots[i].Height/2 {
			return nil, fmt.Errorf("text renderer returned invalid plane size")
		}
		data[6] |= 1 << i
		data = append(data, plane...)
	}
	id := ""
	if data[6] != 0 {
		binary.LittleEndian.PutUint32(data[12:16], uint32(len(data)-16))
		digest := sha256.Sum256(data)
		id = hex.EncodeToString(digest[:])
	} else {
		data = nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.items) == Capacity {
		s.items[0] = entry{}
		s.items = s.items[1:]
	}
	s.items = append(s.items, entry{key: key, id: id, data: data, at: s.now()})
	return descriptor(id), nil
}

func descriptor(id string) *models.TextMask {
	if id == "" {
		return nil
	}
	return &models.TextMask{URL: "/v1/text/" + id, Layout: 1}
}

func (s *Store) Get(path string) ([]byte, bool) {
	id, valid := PathID(path)
	if !valid {
		return nil, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, item := range s.items {
		if item.id == id && s.now().Sub(item.at) < TTL {
			return append([]byte(nil), item.data...), true
		}
	}
	return nil, false
}

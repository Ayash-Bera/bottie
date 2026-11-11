package artwork

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"bottie/companion/internal/models"
)

const MaxDownload = 4 << 20
const MaxPixels = 4194304
const MaxCacheBytes = 2 << 20

func Encode(src image.Image, size int) ([]byte, error) {
	b := src.Bounds()
	w, h := b.Dx(), b.Dy()
	if size < 16 || size > 240 || w < 1 || h < 1 || w > 4096 || h > 4096 || int64(w)*int64(h) > MaxPixels {
		return nil, fmt.Errorf("artwork dimensions exceed limits")
	}
	out := make([]byte, 16+size*size*2)
	copy(out, "BTAR")
	out[4] = 1
	out[5] = 1
	binary.LittleEndian.PutUint16(out[6:], uint16(size))
	binary.LittleEndian.PutUint16(out[8:], uint16(size))
	binary.LittleEndian.PutUint16(out[10:], uint16(size*2))
	binary.LittleEndian.PutUint32(out[12:], uint32(size*size*2))
	dw, dh := size, size
	if w > h {
		dh = max(1, h*size/w)
	} else {
		dw = max(1, w*size/h)
	}
	ox, oy := (size-dw)/2, (size-dh)/2
	for y := 0; y < dh; y++ {
		for x := 0; x < dw; x++ {
			r, g, b, _ := src.At(b.Min.X+x*w/dw, b.Min.Y+y*h/dh).RGBA()
			pixel := uint16((r>>11)<<11 | (g>>10)<<5 | b>>11)
			binary.LittleEndian.PutUint16(out[16+((y+oy)*size+x+ox)*2:], pixel)
		}
	}
	return out, nil
}

func Decode(data []byte, size int) ([]byte, error) {
	if len(data) > MaxDownload {
		return nil, fmt.Errorf("artwork download exceeds limit")
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("inspect artwork: %w", err)
	}
	if cfg.Width < 1 || cfg.Height < 1 || cfg.Width > 4096 || cfg.Height > 4096 || int64(cfg.Width)*int64(cfg.Height) > MaxPixels {
		return nil, fmt.Errorf("artwork pixels exceed limit")
	}
	im, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("decode artwork: %w", err)
	}
	return Encode(im, size)
}

func Original(variant int) image.Image {
	im := image.NewRGBA(image.Rect(0, 0, 96, 96))
	for y := 0; y < 96; y++ {
		for x := 0; x < 96; x++ {
			c := color.RGBA{14, 22, 48, 255}
			if (x-48)*(x-48)+(y-48)*(y-48) < 900 {
				c = color.RGBA{uint8(52 + x), uint8(100 + y), 180, 255}
			}
			// Floor division matches the portable fixture generator for negative offsets.
			slope := (x - 48) / 3
			if x < 48 && (x-48)%3 != 0 {
				slope--
			}
			d := y - 48 - slope
			if d > -3 && d < 3 && x > 8 && x < 88 {
				c = color.RGBA{238, 188, 102, 255}
			}
			if variant == 1 {
				c.R, c.B = c.B, c.R
			}
			im.SetRGBA(x, y, c)
		}
	}
	return im
}

type entry struct {
	data  []byte
	retry time.Time
}
type Store struct {
	mu      sync.Mutex
	size    int
	client  *http.Client
	entries map[string]entry
	order   []string
	bytes   int
}

func New(size int, client *http.Client) *Store {
	if client == nil {
		client = &http.Client{Timeout: 8 * time.Second}
	}
	c := *client
	c.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return &Store{size: size, client: &c, entries: make(map[string]entry)}
}

func (s *Store) put(id string, e entry) {
	if old, ok := s.entries[id]; ok {
		s.bytes -= len(old.data)
		for i, k := range s.order {
			if k == id {
				s.order = append(s.order[:i], s.order[i+1:]...)
				break
			}
		}
	}
	s.entries[id] = e
	s.order = append(s.order, id)
	s.bytes += len(e.data)
	for len(s.order) > 8 || s.bytes > MaxCacheBytes {
		k := s.order[0]
		s.order = s.order[1:]
		s.bytes -= len(s.entries[k].data)
		delete(s.entries, k)
	}
}

func (s *Store) AddOriginal(id string, variant int) (models.Artwork, error) {
	b, err := Encode(Original(variant), s.size)
	if err != nil {
		return models.Artwork{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.put(id, entry{data: b})
	return s.ref(id), nil
}
func (s *Store) ref(id string) models.Artwork {
	return models.Artwork{URL: "/v1/artwork/" + id, Width: s.size, Height: s.size}
}
func (s *Store) Get(id string) ([]byte, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	e, ok := s.entries[id]
	if !ok || len(e.data) == 0 {
		return nil, false
	}
	return bytes.Clone(e.data), true
}

func Trusted(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && len(raw) <= 2048 && u.Scheme == "https" && u.Host == "i.scdn.co" && u.User == nil && u.Fragment == "" && u.RawQuery == "" && strings.HasPrefix(u.Path, "/image/")
}

func (s *Store) FromSpotify(ctx context.Context, raw string) (models.Artwork, error) {
	if !Trusted(raw) {
		return models.Artwork{}, fmt.Errorf("untrusted artwork origin")
	}
	id := fmt.Sprintf("%x", sha256.Sum256([]byte(raw)))[:32]
	s.mu.Lock()
	if e, ok := s.entries[id]; ok {
		if len(e.data) > 0 {
			s.mu.Unlock()
			return s.ref(id), nil
		}
		if time.Now().Before(e.retry) {
			s.mu.Unlock()
			return models.Artwork{}, fmt.Errorf("artwork retry deferred")
		}
	}
	s.put(id, entry{retry: time.Now().Add(time.Minute)})
	s.mu.Unlock()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, raw, nil)
	if err != nil {
		return models.Artwork{}, fmt.Errorf("create artwork request: %w", err)
	}
	res, err := s.client.Do(req)
	if err != nil {
		return models.Artwork{}, fmt.Errorf("artwork download unavailable")
	}
	defer res.Body.Close()
	if res.StatusCode != 200 || res.ContentLength > MaxDownload {
		return models.Artwork{}, fmt.Errorf("artwork response rejected")
	}
	data, err := io.ReadAll(io.LimitReader(res.Body, MaxDownload+1))
	if err != nil {
		return models.Artwork{}, fmt.Errorf("read artwork: %w", err)
	}
	b, err := Decode(data, s.size)
	if err != nil {
		return models.Artwork{}, err
	}
	s.mu.Lock()
	s.put(id, entry{data: b})
	s.mu.Unlock()
	return s.ref(id), nil
}

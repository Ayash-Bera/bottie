package spotify

import (
	"bytes"
	"context"
	"image/png"
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/models"
)

func finishTestAssets(p *Player) {
	p.mu.RLock()
	job := p.assetJob
	p.mu.RUnlock()
	if job != nil {
		p.loadAssets(job)
		p.mu.Lock()
		job.done = true
		job.cancel()
		p.mu.Unlock()
	}
}

func startTestAssets(t *testing.T, p *Player) {
	t.Helper()
	ctx, stop := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { p.runAssets(ctx); close(done) }()
	t.Cleanup(func() {
		stop()
		p.mu.Lock()
		p.cancelAssetsLocked()
		p.mu.Unlock()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("asset worker did not stop")
		}
	})
}

func awaitAsset(t *testing.T, ready <-chan struct{}) {
	t.Helper()
	select {
	case <-ready:
	case <-time.After(time.Second):
		t.Fatal("asset request not observed")
	}
}

func TestAsyncMetadataAndGuardedControlBeforeAsset(t *testing.T) {
	for _, kind := range []string{"context", "artwork"} {
		t.Run(kind, func(t *testing.T) {
			entered, release, posted := make(chan struct{}), make(chan struct{}), make(chan struct{}, 1)
			var once sync.Once
			var reads atomic.Int32
			body := playbackJSON
			if kind == "artwork" {
				body = strings.Replace(body, `,"context":{"type":"playlist","uri":"spotify:playlist:1234567890123456789012"}`, "", 1)
				body = strings.Replace(body, `"images":[]`, `"images":[{"url":"https://i.scdn.co/image/held"}]`, 1)
			}
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/me/player":
					reads.Add(1)
					io.WriteString(w, body)
				case "/me/player/pause":
					if r.Method != "PUT" || r.URL.Query().Get("device_id") != "private-device-id" {
						t.Error("unguarded command")
					}
					posted <- struct{}{}
					w.WriteHeader(204)
				default:
					once.Do(func() { close(entered) })
					select {
					case <-release:
					case <-r.Context().Done():
						return
					}
					io.WriteString(w, `{"name":"Late playlist"}`)
				}
			})
			p.art = artwork.New(96, &http.Client{Transport: efficiencyTransport{p.apiURL, p.client.Transport}})
			authorize(p)
			startTestAssets(t, p)
			p.Poll(context.Background())
			awaitAsset(t, entered)
			s, e := p.Snapshot()
			if e != nil || s.Title != "Example" || s.Artist != "Artist" || s.Album != "Album" || s.Context.Type != "album" || s.Artwork.URL != "" || !s.Capabilities.Pause || s.ControlTarget == "" {
				t.Fatal("core metadata withheld or stale asset exposed")
			}
			done := make(chan *models.ServiceError, 1)
			go func() {
				done <- p.Control(context.Background(), models.Control{Action: "pause", ControlTarget: s.ControlTarget})
			}()
			select {
			case e := <-done:
				if e != nil {
					t.Fatal(e)
				}
			case <-time.After(time.Second):
				t.Fatal("control blocked by optional IO")
			}
			awaitAsset(t, posted)
			if reads.Load() != 2 {
				t.Fatal("fresh target check missing")
			}
			close(release)
		})
	}
}

func TestAssetIdentityCancellationAndCoalescing(t *testing.T) {
	for _, change := range []string{"track", "title", "artist", "album", "playlist", "cover", "target", "offline"} {
		t.Run(change, func(t *testing.T) {
			var body atomic.Value
			body.Store(playbackJSON)
			entered, cancelled := make(chan struct{}), make(chan struct{})
			var once sync.Once
			var cancelOnce sync.Once
			var calls atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/me/player" {
					io.WriteString(w, body.Load().(string))
					return
				}
				calls.Add(1)
				once.Do(func() { close(entered) })
				<-r.Context().Done()
				cancelOnce.Do(func() { close(cancelled) })
			})
			authorize(p)
			startTestAssets(t, p)
			p.Poll(context.Background())
			awaitAsset(t, entered)
			for i := 0; i < 10; i++ {
				p.Poll(context.Background())
			}
			if calls.Load() != 1 {
				t.Fatal("same identity duplicated in-flight work")
			}
			p.mu.RLock()
			old := p.assetJob
			p.mu.RUnlock()
			next := playbackJSON
			switch change {
			case "track":
				next = strings.Replace(next, `"track1"`, `"track2"`, 1)
			case "title":
				next = strings.Replace(next, `"Example"`, `"Changed"`, 1)
			case "artist":
				next = strings.Replace(next, `"Artist"`, `"Changed"`, 1)
			case "album":
				next = strings.Replace(next, `"Album"`, `"Changed"`, 1)
			case "playlist":
				next = strings.Replace(next, "1234567890123456789012", "2234567890123456789012", 1)
			case "cover":
				next = strings.Replace(next, `"images":[]`, `"images":[{"url":"https://i.scdn.co/image/changed"}]`, 1)
			case "target":
				next = strings.Replace(next, "private-device-id", "new-device-id", 1)
			case "offline":
				next = "broken"
			}
			body.Store(next)
			p.Poll(context.Background())
			awaitAsset(t, cancelled)
			p.mu.RLock()
			stillValid := p.currentAssetLocked(old)
			p.mu.RUnlock()
			if stillValid || old.ctx.Err() == nil {
				t.Fatal("old identity remained publishable")
			}
			s, _ := p.Snapshot()
			if s.Context.Name == "Late playlist" || s.Artwork.URL != "" {
				t.Fatal("late asset crossed identity")
			}
		})
	}
}

func TestAssetExpiredQueueCanRetryOnFreshObservation(t *testing.T) {
	requested := make(chan struct{}, 1)
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/me/player" {
			io.WriteString(w, playbackJSON)
			return
		}
		requested <- struct{}{}
		io.WriteString(w, `{"name":"Fresh context"}`)
	})
	var now atomic.Int64
	now.Store(1700000000000)
	p.now = func() time.Time { return time.UnixMilli(now.Load()) }
	authorize(p)
	p.Poll(context.Background())
	p.mu.RLock()
	old := p.assetJob
	p.mu.RUnlock()
	now.Add(15001)
	startTestAssets(t, p)
	awaitAsset(t, old.ctx.Done())
	select {
	case <-requested:
		t.Fatal("expired queued asset was fetched")
	default:
	}
	p.Poll(context.Background())
	awaitAsset(t, requested)
}

func TestAssetErrorsPreserveMetadataAndBackoff(t *testing.T) {
	for _, status := range []int{200, 301, 401, 403, 404, 429, 500} {
		t.Run(http.StatusText(status), func(t *testing.T) {
			var optional, writes atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/me/player" {
					io.WriteString(w, playbackJSON)
					return
				}
				if r.Method != "GET" {
					writes.Add(1)
					w.WriteHeader(204)
					return
				}
				optional.Add(1)
				w.Header().Set("Retry-After", "120")
				w.Header().Set("Location", "/forbidden-redirect")
				w.WriteHeader(status)
				io.WriteString(w, "malformed")
			})
			authorize(p)
			p.Poll(context.Background())
			s, _ := p.Snapshot()
			stamp, guard := s.UpdatedAtMS, s.ControlTarget
			finishTestAssets(p)
			s, e := p.Snapshot()
			if e != nil || s.State != "playing" || s.Title != "Example" || s.Context.Type != "album" || s.UpdatedAtMS != stamp || s.ControlTarget != guard {
				t.Fatal("optional failure changed core observation")
			}
			if status == 429 {
				e = p.Control(context.Background(), models.Control{Action: "pause", ControlTarget: guard})
				if e == nil || e.Code != "rate_limited" || e.RetryAfter != 120 || writes.Load() != 0 {
					t.Fatal("optional API rate limit bypassed")
				}
			} else {
				p.Poll(context.Background())
				finishTestAssets(p)
			}
			if optional.Load() != 1 {
				t.Fatal("optional failure retried or redirected")
			}
		})
	}
}

func TestAsyncAssetSuccessKeepsObservationAndDropsOldCover(t *testing.T) {
	var image bytes.Buffer
	_ = png.Encode(&image, artwork.Original(0))
	var response atomic.Value
	body := strings.Replace(playbackJSON, `"images":[]`, `"images":[{"url":"https://i.scdn.co/image/original"}]`, 1)
	response.Store(body)
	var covers atomic.Int32
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/me/player":
			io.WriteString(w, response.Load().(string))
		case strings.HasPrefix(r.URL.Path, "/playlists/"):
			io.WriteString(w, `{"name":"চাঁদের আলো / 夜"}`)
		default:
			covers.Add(1)
			w.Write(image.Bytes())
		}
	})
	p.art = artwork.New(96, &http.Client{Transport: efficiencyTransport{p.apiURL, p.client.Transport}})
	authorize(p)
	p.Poll(context.Background())
	before, _ := p.Snapshot()
	finishTestAssets(p)
	after, _ := p.Snapshot()
	if after.Artwork.URL == "" || after.Context.Name != "চাঁদের আলো / 夜" || after.UpdatedAtMS != before.UpdatedAtMS || after.ControlTarget != before.ControlTarget {
		t.Fatal("asset publish lost metadata/observation identity")
	}
	p.Poll(context.Background())
	cached, _ := p.Snapshot()
	finishTestAssets(p)
	if cached.Artwork != after.Artwork || cached.Context != after.Context || covers.Load() != 1 {
		t.Fatal("unchanged metadata lost cached assets")
	}
	response.Store(strings.Replace(body, `"track1"`, `"track2"`, 1))
	p.Poll(context.Background())
	fresh, _ := p.Snapshot()
	if fresh.Artwork.URL != "" || fresh.Context.Type != "album" {
		t.Fatal("previous track artwork/context displayed before new identity result")
	}
	finishTestAssets(p)
	if covers.Load() != 1 {
		t.Fatal("same original URL unnecessarily redownloaded")
	}
}

func TestAsyncRunCancelsHeldAsset(t *testing.T) {
	entered := make(chan struct{})
	var once sync.Once
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/me/player" {
			io.WriteString(w, playbackJSON)
			return
		}
		once.Do(func() { close(entered) })
		<-r.Context().Done()
	})
	authorize(p)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { p.Run(ctx, time.Hour); close(done) }()
	awaitAsset(t, entered)
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("shutdown left optional IO alive")
	}
}

func TestAssetLateResultAndFreshness(t *testing.T) {
	for _, change := range []string{"metadata", "stale", "monotonic-wall-reversal", "reauthorization"} {
		t.Run(change, func(t *testing.T) {
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, playbackJSON) })
			var now atomic.Int64
			now.Store(1700000000000)
			p.now = func() time.Time { return time.UnixMilli(now.Load()) }
			authorize(p)
			p.Poll(context.Background())
			p.mu.RLock()
			job := p.assetJob
			p.mu.RUnlock()
			p.client = &http.Client{Transport: assetRoundTrip(func(r *http.Request) (*http.Response, error) {
				switch change {
				case "metadata":
					p.mu.Lock()
					p.cached.Title = "Changed while loading"
					p.mu.Unlock()
				case "stale":
					now.Add(15001)
				case "monotonic-wall-reversal":
					now.Add(-1)
				case "reauthorization":
					p.save(models.Empty("unavailable"), loginRequired())
				}
				return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(strings.NewReader(`{"name":"Must not publish"}`))}, nil
			})}
			p.loadAssets(job)
			p.mu.RLock()
			s := p.cached
			count := len(p.names)
			p.mu.RUnlock()
			if s.Context.Name == "Must not publish" || count != 0 {
				t.Fatal("late or stale result published/cached")
			}
			job.cancel()
		})
	}
}

type assetRoundTrip func(*http.Request) (*http.Response, error)

func (f assetRoundTrip) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

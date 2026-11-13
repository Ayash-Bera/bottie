package spotify

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"image/png"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/models"
)

type efficiencyTransport struct {
	base string
	next http.RoundTripper
}

func (t efficiencyTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	c := r.Clone(r.Context())
	u := *r.URL
	c.URL = &u
	if u.Host == "i.scdn.co" {
		c.URL.Scheme = "http"
		c.URL.Host = strings.TrimPrefix(t.base, "http://")
	}
	return t.next.RoundTrip(c)
}

func TestEfficiencyMeasurement(t *testing.T) {
	for _, scenario := range []string{"context", "artwork", "both", "fresh-check"} {
		for repeat := 0; repeat < 5; repeat++ {
			var active atomic.Bool
			var controlStartNS, arrivalNS atomic.Int64
			var playerReads atomic.Int32
			freshDelay := time.Duration(0)
			if scenario == "fresh-check" {
				freshDelay = 75 * time.Millisecond
			}
			entered := make(chan struct{})
			var once sync.Once
			var image bytes.Buffer
			if err := png.Encode(&image, artwork.Original(0)); err != nil {
				t.Fatal(err)
			}
			body := playbackJSON
			if scenario == "artwork" {
				body = strings.Replace(body, `,"context":{"type":"playlist","uri":"spotify:playlist:1234567890123456789012"}`, "", 1)
			}
			if scenario != "context" {
				body = strings.Replace(body, `"images":[]`, `"images":[{"url":"https://i.scdn.co/image/test"}]`, 1)
			}
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/me/player/pause" {
					arrivalNS.Store(time.Now().UnixNano() - controlStartNS.Load())
					w.WriteHeader(204)
					return
				}
				if r.URL.Path == "/me/player" {
					if playerReads.Add(1) >= 3 && freshDelay != 0 {
						select {
						case <-time.After(freshDelay):
						case <-r.Context().Done():
							return
						}
					}
					if active.Load() {
						io.WriteString(w, body)
					} else {
						io.WriteString(w, strings.Replace(strings.Replace(playbackJSON, `"Example"`, `"Old"`, 1), `,"context":{"type":"playlist","uri":"spotify:playlist:1234567890123456789012"}`, "", 1))
					}
					return
				}
				once.Do(func() { close(entered) })
				select {
				case <-time.After(200 * time.Millisecond):
				case <-r.Context().Done():
					return
				}
				if strings.HasPrefix(r.URL.Path, "/playlists/") {
					io.WriteString(w, `{"name":"Measured context"}`)
				} else {
					w.Write(image.Bytes())
				}
			}))
			client := srv.Client()
			client.Transport = efficiencyTransport{srv.URL, client.Transport}
			p := New(strings.Repeat("a", 32), "http://127.0.0.1:8787/auth/callback", artwork.New(96, client), client)
			p.apiURL = srv.URL
			authorize(p)
			p.Poll(context.Background())
			active.Store(true)
			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			start := time.Now()
			go func() { p.Run(ctx, time.Hour); close(done) }()
			select {
			case <-entered:
			case <-time.After(3 * time.Second):
				t.Fatal("optional request absent")
			}
			controlStart := time.Now()
			controlStartNS.Store(controlStart.UnixNano())
			controlDone := make(chan time.Duration, 1)
			snapshot, _ := p.Snapshot()
			go func() {
				e := p.Control(ctx, models.Control{Action: "pause", ControlTarget: snapshot.ControlTarget})
				if e != nil {
					t.Error(e)
				}
				controlDone <- time.Since(controlStart)
			}()
			var metadata time.Duration
			for deadline := time.Now().Add(3 * time.Second); time.Now().Before(deadline); {
				s, e := p.Snapshot()
				if e == nil && s.Title == "Example" {
					metadata = time.Since(start)
					break
				}
				time.Sleep(time.Millisecond)
			}
			if metadata == 0 {
				t.Fatal("metadata absent")
			}
			var control time.Duration
			select {
			case control = <-controlDone:
			case <-time.After(3 * time.Second):
				t.Fatal("control stalled")
			}
			cancel()
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				t.Fatal("shutdown stalled")
			}
			srv.Close()
			if arrivalNS.Load() <= 0 {
				t.Fatal("synthetic control did not reach upstream")
			}
			b, _ := json.Marshal(map[string]any{"scenario": scenario, "repeat": repeat, "delay_ms": 200, "fresh_check_delay_ms": freshDelay.Milliseconds(), "metadata_ms": float64(metadata) / float64(time.Millisecond), "guarded_control_ms": float64(control) / float64(time.Millisecond), "control_arrival_ms": float64(arrivalNS.Load()) / float64(time.Millisecond)})
			fmt.Println("EFFICIENCY_MEASUREMENT " + string(b))
		}
	}
}

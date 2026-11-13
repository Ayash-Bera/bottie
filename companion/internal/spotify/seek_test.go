package spotify

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"bottie/companion/internal/models"
)

func TestSeekFreshIdentityRestrictionClampAndNoReplay(t *testing.T) {
	for _, scenario := range []string{"success", "zero", "track", "duration", "target", "restricted", "disallowed", "absent", "stale", "guard", "403", "429", "401"} {
		t.Run(scenario, func(t *testing.T) {
			var reads, writes atomic.Int32
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method == "GET" {
					body := withDevice(supportedDevice)
					if reads.Add(1) > 1 {
						switch scenario {
						case "track":
							body = strings.Replace(body, "track1", "track2", 1)
						case "duration":
							body = strings.Replace(body, "60000", "70000", 1)
						case "target":
							body = strings.Replace(body, "private-device-id", "other-device", 1)
						case "restricted":
							body = strings.Replace(body, `"is_restricted":false`, `"is_restricted":true`, 1)
						case "disallowed":
							body = strings.Replace(body, `"skipping_next":true`, `"skipping_next":true,"seeking":true`, 1)
						case "absent":
							w.WriteHeader(204)
							return
						}
					}
					_, _ = io.WriteString(w, body)
					return
				}
				writes.Add(1)
				position := "59999"
				if scenario == "zero" {
					position = "0"
				}
				if r.Method != "PUT" || r.URL.Path != "/me/player/seek" || r.URL.Query().Get("device_id") != "private-device-id" || r.URL.Query().Get("position_ms") != position {
					t.Error("seek target or clamp")
				}
				if code, err := strconv.Atoi(scenario); err == nil {
					w.Header().Set("Retry-After", "3")
					w.WriteHeader(code)
				} else {
					w.WriteHeader(204)
				}
			})
			authorize(p)
			p.Poll(context.Background())
			s, _ := p.Snapshot()
			if !s.Capabilities.Seek || !models.ValidControlTarget(s.ControlTrack) {
				t.Fatal("seek not advertised")
			}
			c := models.Control{Action: "seek", Value: json.RawMessage("86400000"), ControlTarget: s.ControlTarget, ControlTrack: s.ControlTrack}
			if scenario == "zero" {
				c.Value = json.RawMessage("0")
			}
			if scenario == "guard" {
				c.ControlTrack = strings.Repeat("f", 32)
			}
			if scenario == "stale" {
				*now = now.Add(16 * time.Second)
			}
			result, err := p.ControlWithResult(context.Background(), c)
			wantWrite := scenario == "success" || scenario == "zero" || scenario == "403" || scenario == "429" || scenario == "401"
			if (writes.Load() == 1) != wantWrite || writes.Load() > 1 {
				t.Fatalf("write count %d", writes.Load())
			}
			if scenario == "success" || scenario == "zero" {
				want := int64(59999)
				if scenario == "zero" {
					want = 0
				}
				if err != nil || !result.OK || result.Result != "accepted" || result.PositionMS == nil || *result.PositionMS != want {
					t.Fatalf("result %+v %v", result, err)
				}
				if e := p.Control(context.Background(), c); e == nil || e.Status != 429 || e.RetryAfter != 1 {
					t.Fatal("seek pacing not enforced")
				}
				if writes.Load() != 1 {
					t.Fatal("rate rejection wrote again")
				}
			} else if err == nil || result.OK {
				t.Fatal("rejected seek marked accepted")
			}
			if scenario == "stale" || scenario == "guard" {
				if reads.Load() != 1 {
					t.Fatal("invalid local guard reached upstream")
				}
			} else if reads.Load() != 2 {
				t.Fatal("missing fresh check")
			}
		})
	}
}

func TestTrackGenerationAndUnknownDuration(t *testing.T) {
	var response atomic.Value
	response.Store(withDevice(supportedDevice))
	p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, response.Load().(string)) })
	authorize(p)
	p.Poll(context.Background())
	previous, _ := p.Snapshot()
	for _, tc := range []struct {
		old, new string
		rotate   bool
	}{{"Example", "Renamed", false}, {"track1", "track2", true}, {"60000", "70000", true}, {"Artist", "Different", false}, {"private-device-id", "other-device", true}} {
		response.Store(strings.Replace(response.Load().(string), tc.old, tc.new, 1))
		p.Poll(context.Background())
		current, _ := p.Snapshot()
		if (current.ControlTrack != previous.ControlTrack) != tc.rotate {
			t.Fatal("track generation identity")
		}
		previous = current
	}
	*now = now.Add(16 * time.Second)
	p.Poll(context.Background())
	current, _ := p.Snapshot()
	if current.ControlTrack == previous.ControlTrack {
		t.Fatal("expired track guard resurrected")
	}
	response.Store(strings.Replace(response.Load().(string), "70000", "0", 1))
	p.Poll(context.Background())
	current, _ = p.Snapshot()
	if current.Capabilities.Seek || current.ControlTrack != "" {
		t.Fatal("unknown duration enabled seek")
	}
	p.fail(loginRequired())
	current, _ = p.Snapshot()
	if current.OfflineReason != "login_required" {
		t.Fatal("authorization reason")
	}
}

func TestSeekHeldFreshCheckAndAttemptSpacing(t *testing.T) {
	for _, scenario := range []string{"same", "track", "duration", "target", "restricted", "authorization", "cancel"} {
		t.Run(scenario, func(t *testing.T) {
			entered, release := make(chan struct{}), make(chan struct{})
			var reads, writes atomic.Int32
			var response atomic.Value
			response.Store(withDevice(supportedDevice))
			defer func() {
				select {
				case <-release:
				default:
					close(release)
				}
			}()
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method != "GET" {
					writes.Add(1)
					w.WriteHeader(204)
					return
				}
				if reads.Add(1) == 2 {
					close(entered)
					select {
					case <-release:
					case <-r.Context().Done():
						return
					}
					if scenario == "authorization" {
						w.WriteHeader(403)
						return
					}
				}
				_, _ = io.WriteString(w, response.Load().(string))
			})
			authorize(p)
			p.Poll(context.Background())
			s, _ := p.Snapshot()
			c := models.Control{Action: "seek", Value: json.RawMessage("1"), ControlTarget: s.ControlTarget, ControlTrack: s.ControlTrack}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan *models.ServiceError, 1)
			go func() { done <- p.Control(ctx, c) }()
			select {
			case <-entered:
			case <-time.After(time.Second):
				t.Fatal("fresh GET not held")
			}
			if writes.Load() != 0 {
				t.Fatal("write before mandatory observation")
			}
			body := response.Load().(string)
			switch scenario {
			case "track":
				body = strings.Replace(body, "track1", "track2", 1)
			case "duration":
				body = strings.Replace(body, "60000", "60001", 1)
			case "target":
				body = strings.Replace(body, "private-device-id", "other-device", 1)
			case "restricted":
				body = strings.Replace(body, `"is_restricted":false`, `"is_restricted":true`, 1)
			case "cancel":
				cancel()
			}
			response.Store(body)
			close(release)
			err := <-done
			if scenario != "same" {
				if err == nil || writes.Load() != 0 {
					t.Fatal("invalid held observation reached PUT")
				}
				return
			}
			if err != nil || writes.Load() != 1 {
				t.Fatal("fresh positive control failed")
			}
			*now = now.Add(999 * time.Millisecond)
			if err := p.Control(context.Background(), c); err == nil || err.Status != 429 || reads.Load() != 2 || writes.Load() != 1 {
				t.Fatal("999ms spacing")
			}
			*now = now.Add(time.Millisecond)
			if err := p.Control(context.Background(), c); err != nil || reads.Load() != 3 || writes.Load() != 2 {
				t.Fatal("1000ms spacing")
			}
		})
	}
}

func TestSeekOneMillisecondDurationClampsToZero(t *testing.T) {
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			_, _ = io.WriteString(w, strings.Replace(withDevice(supportedDevice), "60000", "1", 1))
			return
		}
		if r.URL.Query().Get("position_ms") != "0" {
			t.Error("one-millisecond track advanced")
		}
		w.WriteHeader(204)
	})
	authorize(p)
	p.Poll(context.Background())
	s, _ := p.Snapshot()
	if err := p.Control(context.Background(), models.Control{Action: "seek", Value: json.RawMessage("86400000"), ControlTarget: s.ControlTarget, ControlTrack: s.ControlTrack}); err != nil {
		t.Fatal(err)
	}
}

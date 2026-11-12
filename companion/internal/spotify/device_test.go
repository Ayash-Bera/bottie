package spotify

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"bottie/companion/internal/models"
)

const supportedDevice = `{"id":"private-device-id","is_active":true,"is_restricted":false,"supports_volume":true,"volume_percent":40}`

func withDevice(device string) string {
	var v map[string]json.RawMessage
	_ = json.Unmarshal([]byte(playbackJSON), &v)
	v["device"], v["context"] = json.RawMessage(device), json.RawMessage("null")
	b, _ := json.Marshal(v)
	return string(b)
}

func TestVolumeObservation(t *testing.T) {
	for _, tt := range []struct {
		name, device                 string
		control, level, known, fresh bool
		value                        int
	}{
		{"supported", supportedDevice, true, true, true, true, 40},
		{"mute", strings.Replace(supportedDevice, ":40", ":0", 1), true, true, true, true, 0},
		{"maximum", strings.Replace(supportedDevice, ":40", ":100", 1), true, true, true, true, 100},
		{"null level", strings.Replace(supportedDevice, ":40", ":null", 1), true, false, true, true, 0},
		{"missing level", strings.Replace(supportedDevice, `,"volume_percent":40`, "", 1), true, false, true, true, 0},
		{"unsupported", strings.Replace(supportedDevice, `"supports_volume":true`, `"supports_volume":false`, 1), false, true, true, true, 40},
		{"unknown support", strings.Replace(supportedDevice, `,"supports_volume":true`, "", 1), false, true, false, true, 40},
		{"null support", strings.Replace(supportedDevice, `"supports_volume":true`, `"supports_volume":null`, 1), false, true, false, true, 40},
		{"restricted", strings.Replace(supportedDevice, `"is_restricted":false`, `"is_restricted":true`, 1), false, true, true, true, 40},
		{"missing restriction", strings.Replace(supportedDevice, `,"is_restricted":false`, "", 1), false, true, true, true, 40},
		{"inactive", strings.Replace(supportedDevice, `"is_active":true`, `"is_active":false`, 1), false, true, true, true, 40},
		{"unknown id", strings.Replace(supportedDevice, `"id":"private-device-id"`, `"id":null`, 1), false, true, true, true, 40},
		{"negative level", strings.Replace(supportedDevice, ":40", ":-1", 1), true, false, true, true, 0},
		{"excess level", strings.Replace(supportedDevice, ":40", ":101", 1), true, false, true, true, 0},
		{"fractional level", strings.Replace(supportedDevice, ":40", ":1.5", 1), false, false, false, false, 0},
		{"no device", "null", false, false, false, true, 0},
	} {
		t.Run(tt.name, func(t *testing.T) {
			var reads atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method != "GET" || r.URL.Path != "/me/player" {
					t.Error("unexpected request")
				}
				reads.Add(1)
				_, _ = io.WriteString(w, withDevice(tt.device))
			})
			authorize(p)
			p.Poll(context.Background())
			s, _ := p.Snapshot()
			d := p.DeviceStatus()
			if s.Capabilities.Volume != tt.control || (s.VolumePercent != nil) != tt.level || (d.SupportsVolume != nil) != tt.known || d.Fresh != tt.fresh {
				t.Fatalf("unexpected flags: %+v %+v", s, d)
			}
			if tt.level && *s.VolumePercent != tt.value {
				t.Fatal("level changed")
			}
			if tt.control && !models.ValidControlTarget(s.ControlTarget) {
				t.Fatal("missing target guard")
			}
			if s.VolumePercent != nil {
				*s.VolumePercent = 99
			}
			if d.VolumePercent != nil {
				*d.VolumePercent = 99
			}
			if tt.level && *p.DeviceStatus().VolumePercent != tt.value {
				t.Fatal("returned pointer mutated cache")
			}
			b, _ := json.Marshal(struct {
				Playback models.Playback
				Device   DeviceStatus
			}{s, d})
			if strings.Contains(string(b), "private-device-id") || strings.Contains(string(b), "secret") {
				t.Fatal("private data escaped")
			}
			if reads.Load() != 1 {
				t.Fatal("status performed an upstream query")
			}
		})
	}
}

func TestTargetGenerationAndFreshness(t *testing.T) {
	body := withDevice(supportedDevice)
	var response atomic.Value
	response.Store(body)
	p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, response.Load().(string)) })
	authorize(p)
	p.Poll(context.Background())
	a, _ := p.Snapshot()
	if !models.ValidControlTarget(a.ControlTarget) {
		t.Fatal("missing initial target")
	}
	body = strings.Replace(body, "track1", "track2", 1)
	response.Store(body)
	p.Poll(context.Background())
	b, _ := p.Snapshot()
	if a.ControlTarget != b.ControlTarget {
		t.Fatal("track change rotated device target")
	}
	body = strings.Replace(body, "private-device-id", "second-device", 1)
	response.Store(body)
	p.Poll(context.Background())
	b, _ = p.Snapshot()
	if a.ControlTarget == b.ControlTarget {
		t.Fatal("device switch reused target")
	}
	if e := p.Control(context.Background(), models.Control{Action: "pause", ControlTarget: a.ControlTarget}); e == nil || e.Code != "target_changed" {
		t.Fatal("old generation accepted")
	}
	*now = now.Add(16 * time.Second)
	stale, _ := p.Snapshot()
	if stale.ControlTarget != "" || stale.Capabilities.Volume || stale.VolumePercent != nil || p.DeviceStatus().Fresh {
		t.Fatal("stale target enabled")
	}
	p.Poll(context.Background())
	c, _ := p.Snapshot()
	if c.ControlTarget == b.ControlTarget {
		t.Fatal("stale generation resurrected")
	}
	p.fail(unavailable())
	p.Poll(context.Background())
	d, _ := p.Snapshot()
	if d.ControlTarget == c.ControlTarget {
		t.Fatal("failed generation resurrected")
	}
	*now = now.Add(-time.Second)
	if s, _ := p.Snapshot(); s.ControlTarget != "" || p.DeviceStatus().Fresh {
		t.Fatal("future observation accepted")
	}
}

func TestGuardedVolumeRequests(t *testing.T) {
	for _, scenario := range []string{"success", "switch", "lost_support", "restricted", "no_device", "stale", "scope", "403", "429", "401", "malformed"} {
		t.Run(scenario, func(t *testing.T) {
			var reads, writes, tokens atomic.Int32
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/token" {
					tokens.Add(1)
					t.Error("unexpected token replay")
					return
				}
				if r.Method == "GET" {
					n := reads.Add(1)
					device := supportedDevice
					if n > 1 {
						switch scenario {
						case "switch":
							device = strings.Replace(device, "private-device-id", "second-device", 1)
						case "lost_support":
							device = strings.Replace(device, `"supports_volume":true`, `"supports_volume":false`, 1)
						case "restricted":
							device = strings.Replace(device, `"is_restricted":false`, `"is_restricted":true`, 1)
						case "no_device":
							w.WriteHeader(204)
							return
						case "malformed":
							_, _ = io.WriteString(w, "{")
							return
						}
					}
					_, _ = io.WriteString(w, withDevice(device))
					return
				}
				writes.Add(1)
				if r.Method != "PUT" || r.URL.Path != "/me/player/volume" || r.URL.Query().Get("volume_percent") != "35" || r.URL.Query().Get("device_id") != "private-device-id" {
					t.Error("unbounded or unpinned volume request")
				}
				switch scenario {
				case "403":
					w.WriteHeader(403)
				case "429":
					w.Header().Set("Retry-After", "7")
					w.WriteHeader(429)
				case "401":
					w.WriteHeader(401)
				default:
					w.WriteHeader(204)
				}
			})
			authorize(p)
			if scenario == "scope" {
				p.tokens.canControl = false
			}
			p.Poll(context.Background())
			s, _ := p.Snapshot()
			if scenario == "scope" {
				if s.Capabilities.Volume || s.ControlTarget != "" {
					t.Fatal("missing scope enabled controls")
				}
				return
			}
			if scenario == "stale" {
				*now = now.Add(16 * time.Second)
			}
			c := models.Control{Action: "volume", Value: json.RawMessage("35"), ControlTarget: s.ControlTarget}
			e := p.Control(context.Background(), c)
			if scenario == "success" {
				if e != nil || writes.Load() != 1 || reads.Load() != 2 {
					t.Fatalf("successful request failed: %v", e)
				}
				if s, _ := p.Snapshot(); *s.VolumePercent != 40 {
					t.Fatal("volume optimistically reported")
				}
				e = p.Control(context.Background(), c)
				if e == nil || e.Code != "rate_limited" || writes.Load() != 1 || reads.Load() != 2 {
					t.Fatal("slider burst not bounded")
				}
				*now = now.Add(time.Second)
				if e = p.Control(context.Background(), c); e != nil || writes.Load() != 2 {
					t.Fatal("bounded dispatch never resumed")
				}
				return
			}
			if e == nil {
				t.Fatal("unsafe volume accepted")
			}
			if scenario == "429" && (e.Code != "rate_limited" || e.RetryAfter != 7) {
				t.Fatal("Retry-After lost")
			}
			wantWrites := int32(0)
			if scenario == "403" || scenario == "429" || scenario == "401" {
				wantWrites = 1
			}
			_ = p.Control(context.Background(), c)
			if writes.Load() != wantWrites || tokens.Load() != 0 {
				t.Fatal("unexpected command or replay")
			}
		})
	}
}

func TestDeviceStatusConcurrentReaders(t *testing.T) {
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, withDevice(supportedDevice)) })
	authorize(p)
	var workers sync.WaitGroup
	for i := 0; i < 3; i++ {
		workers.Go(func() {
			for j := 0; j < 100; j++ {
				p.Snapshot()
				p.DeviceStatus()
			}
		})
	}
	for i := 0; i < 10; i++ {
		p.Poll(context.Background())
	}
	workers.Wait()
}

func TestGuardedButtonsPinActivePlayer(t *testing.T) {
	for _, tt := range []struct{ action, value, query string }{
		{"play", "", ""}, {"pause", "", ""}, {"previous", "", ""}, {"next", "", ""},
		{"shuffle", "true", "true"}, {"repeat", `"context"`, "context"},
	} {
		t.Run(tt.action, func(t *testing.T) {
			var reads, writes atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method == "GET" {
					reads.Add(1)
					body := withDevice(supportedDevice)
					body = strings.Replace(body, `"skipping_next":true`, `"skipping_next":false`, 1)
					if tt.action == "play" {
						body = strings.Replace(body, `"is_playing":true`, `"is_playing":false`, 1)
					}
					_, _ = io.WriteString(w, body)
					return
				}
				writes.Add(1)
				method := "PUT"
				if tt.action == "next" || tt.action == "previous" {
					method = "POST"
				}
				if r.Method != method || r.URL.Path != "/me/player/"+tt.action || r.URL.Query().Get("device_id") != "private-device-id" || r.URL.Query().Get("state") != tt.query {
					t.Error("button command target, method or value changed")
				}
				w.WriteHeader(204)
			})
			authorize(p)
			p.Poll(context.Background())
			s, _ := p.Snapshot()
			if tt.action == "play" && (s.State != "paused" || !s.Capabilities.Play) {
				t.Fatal("paused track cannot resume")
			}
			c := models.Control{Action: tt.action, Value: json.RawMessage(tt.value), ControlTarget: s.ControlTarget}
			if err := p.Control(context.Background(), c); err != nil || reads.Load() != 2 || writes.Load() != 1 {
				t.Fatalf("guarded button failed: %v reads=%d writes=%d", err, reads.Load(), writes.Load())
			}
		})
	}
}

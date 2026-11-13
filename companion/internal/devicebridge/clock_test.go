package devicebridge

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"bottie/companion/internal/models"
)

func TestGatewayClockOwnedAndIndependent(t *testing.T) {
	for _, state := range []string{"playing", "paused", "idle", "unavailable"} {
		t.Run(state, func(t *testing.T) {
			p := models.Empty(state)
			p.Title = "ক্ষিতিজ / 夜"
			p.Album = "Album"
			p.UpdatedAtMS = 1700000000000
			p.ControlTarget = strings.Repeat("a", 32)
			p.Clock = &models.Clock{UnixMS: models.MinClockMS, UTCOffsetMin: -840}
			p = p.Bounded()
			body, _ := json.Marshal(p)
			transport := roundTripFunc(func(r *http.Request) (*http.Response, error) {
				return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": []string{"application/json"}}, Body: io.NopCloser(strings.NewReader(string(body)))}, nil
			})
			now := time.Date(2026, 11, 13, 18, 29, 59, 0, time.UTC)
			cfg := testConfig
			cfg.ClockZone = time.FixedZone("test", 330*60)
			h := bridgeWithClock(cfg, quietLog(), transport, nil, func() time.Time { return now })
			for _, delta := range []time.Duration{0, time.Minute} {
				now = now.Add(delta)
				w := httptest.NewRecorder()
				h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
				var got models.Playback
				if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &got) != nil || got.Clock == nil {
					t.Fatal("clock absent")
				}
				if got.Clock.UnixMS != now.UnixMilli() || got.Clock.UTCOffsetMin != 330 || got.UpdatedAtMS != p.UpdatedAtMS || got.ControlTarget != p.ControlTarget || got.Title != p.Title {
					t.Fatal("clock changed playback identity or trusted upstream time")
				}
			}
			now = time.UnixMilli(models.MaxClockMS + 1)
			w := httptest.NewRecorder()
			h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
			var got models.Playback
			if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &got) != nil || got.Clock != nil {
				t.Fatal("invalid host time exposed")
			}
		})
	}
}

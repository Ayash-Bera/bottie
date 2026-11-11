package fixture

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/models"
)

func TestPlaybackControls(t *testing.T) {
	now := time.Unix(1700000000, 0)
	p, err := New(artwork.New(96, nil), func() time.Time { return now })
	if err != nil {
		t.Fatal(err)
	}
	for _, tt := range []struct {
		action, value, state, id string
		advance                  time.Duration
		progress                 int64
	}{
		{"", "", "paused", "fixture-orbit", 0, 0},
		{"play", "", "playing", "fixture-orbit", 5 * time.Second, 5000},
		{"pause", "", "paused", "fixture-orbit", 5 * time.Second, 5000},
		{"next", "", "paused", "fixture-tide", 0, 0},
		{"previous", "", "paused", "fixture-orbit", 0, 0},
		{"shuffle", "true", "paused", "fixture-orbit", 0, 0},
		{"next", "", "paused", "fixture-signal", 0, 0},
		{"repeat", `"track"`, "paused", "fixture-signal", 0, 0},
		{"play", "", "playing", "fixture-signal", 151 * time.Second, 1000},
	} {
		t.Run(tt.action+tt.id, func(t *testing.T) {
			if tt.action != "" {
				if e := p.Control(context.Background(), models.Control{Action: tt.action, Value: json.RawMessage(tt.value)}); e != nil {
					t.Fatal(e)
				}
			}
			now = now.Add(tt.advance)
			s, e := p.Snapshot()
			if e != nil || s.State != tt.state || s.TrackID != tt.id || s.ProgressMS != tt.progress {
				t.Fatalf("got %+v %v", s, e)
			}
			if s.TrackID == "fixture-tide" && s.Context.Type != "album" {
				t.Fatal("album fallback")
			}
			if s.TrackID == "fixture-signal" && (s.Context.Type != "unknown" || s.Artwork.URL != "") {
				t.Fatal("missing fallback")
			}
		})
	}
}

func TestFixtureBoundaries(t *testing.T) {
	now := time.Unix(1700000000, 0)
	p, _ := New(artwork.New(96, nil), func() time.Time { return now })
	_ = p.Control(context.Background(), models.Control{Action: "previous"})
	_ = p.Control(context.Background(), models.Control{Action: "play"})
	now = now.Add(1000 * time.Hour)
	s, _ := p.Snapshot()
	if s.State != "paused" || s.ProgressMS != s.DurationMS {
		t.Fatalf("end: %+v", s)
	}
	_ = p.Control(context.Background(), models.Control{Action: "play"})
	s, _ = p.Snapshot()
	if s.ProgressMS != 0 || s.State != "playing" {
		t.Fatal("resume at end")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if p.Control(ctx, models.Control{Action: "next"}) == nil {
		t.Fatal("canceled control accepted")
	}
}

func TestElapsedAcrossTracks(t *testing.T) {
	for _, tt := range []struct {
		repeat   string
		elapsed  time.Duration
		id       string
		progress int64
		state    string
	}{
		{"off", 181 * time.Second, "fixture-tide", 1000, "playing"},
		{"off", 600 * time.Second, "fixture-signal", 150000, "paused"},
		{"context", 541 * time.Second, "fixture-orbit", 1000, "playing"},
		{"context", 1000*540*time.Second + 181*time.Second, "fixture-tide", 1000, "playing"},
	} {
		t.Run(tt.repeat+tt.elapsed.String(), func(t *testing.T) {
			now := time.Unix(1700000000, 0)
			p, _ := New(artwork.New(96, nil), func() time.Time { return now })
			value, _ := json.Marshal(tt.repeat)
			_ = p.Control(context.Background(), models.Control{Action: "repeat", Value: value})
			_ = p.Control(context.Background(), models.Control{Action: "play"})
			now = now.Add(tt.elapsed)
			s, _ := p.Snapshot()
			if s.TrackID != tt.id || s.ProgressMS != tt.progress || s.State != tt.state {
				t.Fatalf("elapsed mismatch: %+v", s)
			}
		})
	}
}

func TestShuffleTraversal(t *testing.T) {
	now := time.Unix(1700000000, 0)
	p, _ := New(artwork.New(96, nil), func() time.Time { return now })
	_ = p.Control(context.Background(), models.Control{Action: "shuffle", Value: json.RawMessage("true")})
	for _, tt := range []struct{ action, id string }{{"next", "fixture-signal"}, {"previous", "fixture-orbit"}, {"next", "fixture-signal"}, {"next", "fixture-tide"}} {
		if e := p.Control(context.Background(), models.Control{Action: tt.action}); e != nil {
			t.Fatal(e)
		}
		s, _ := p.Snapshot()
		if s.TrackID != tt.id {
			t.Fatalf("shuffle %s: %s", tt.action, s.TrackID)
		}
	}
	_ = p.Control(context.Background(), models.Control{Action: "play"})
	now = now.Add(time.Hour)
	s, _ := p.Snapshot()
	if s.State != "paused" || s.TrackID != "fixture-tide" || s.ProgressMS != s.DurationMS {
		t.Fatal("shuffled end did not stop")
	}
}

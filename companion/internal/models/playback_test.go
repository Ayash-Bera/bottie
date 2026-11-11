package models

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf8"
)

func TestControlValidation(t *testing.T) {
	for _, tt := range []struct {
		action, value string
		valid         bool
	}{
		{"play", "", true}, {"pause", "", true}, {"previous", "", true}, {"next", "", true}, {"shuffle", "true", true}, {"shuffle", "false", true}, {"repeat", `"off"`, true}, {"repeat", `"context"`, true}, {"repeat", `"track"`, true},
		{"volume", "", false}, {"play", "null", false}, {"pause", "false", false}, {"shuffle", "", false}, {"shuffle", "null", false}, {"shuffle", `"true"`, false}, {"repeat", "", false}, {"repeat", `"all"`, false}, {"repeat", "false", false},
	} {
		t.Run(tt.action+tt.value, func(t *testing.T) {
			err := (Control{Action: tt.action, Value: json.RawMessage(tt.value)}).Validate()
			if (err == nil) != tt.valid {
				t.Fatalf("valid=%v err=%v", tt.valid, err)
			}
		})
	}
}

func TestBoundsAndFallbacks(t *testing.T) {
	for _, tt := range []struct {
		name     string
		p        Playback
		context  string
		progress int64
	}{
		{"album", Playback{State: "playing", Album: "Small Worlds", ProgressMS: 900, DurationMS: 100}, "album", 100},
		{"unknown", Playback{State: "paused", ProgressMS: -1, DurationMS: -1}, "unknown", 0},
		{"playlist", Playback{State: "playing", Album: "Small Worlds", Context: Context{Type: "playlist", Name: "Orbits"}}, "playlist", 0},
	} {
		t.Run(tt.name, func(t *testing.T) {
			p := tt.p.Bounded()
			if p.Context.Type != tt.context || p.ProgressMS != tt.progress {
				t.Fatalf("unexpected snapshot: %+v", p)
			}
		})
	}
	p := Playback{State: "broken", Title: strings.Repeat("星", 300), Artist: strings.Repeat("x", 600), Album: "A", DurationMS: 1 << 60, UpdatedAtMS: -1, Repeat: "invalid", Artwork: Artwork{URL: "https://evil.invalid/", Width: 96, Height: 96}, Capabilities: Capabilities{Play: true}}.Bounded()
	if !utf8.ValidString(p.Title) || len(p.Title) > 256 || len(p.Artist) > 256 || p.DurationMS != MaxDuration || p.UpdatedAtMS != 0 || p.State != "unavailable" || p.Capabilities.Play || p.Artwork.URL != "" || p.Repeat != "off" {
		t.Fatalf("bounds: %+v", p)
	}
}

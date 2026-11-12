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

func TestVolumeAndTargetValidation(t *testing.T) {
	target := strings.Repeat("a", 32)
	for _, value := range []string{"0", "1", "50", "100", " 25 "} {
		if e := (Control{Action: "volume", Value: json.RawMessage(value), ControlTarget: target}).Validate(); e != nil {
			t.Fatalf("valid percent rejected: %s", value)
		}
	}
	for _, value := range []string{"", "null", "true", `"50"`, "-1", "101", "1.5", "1.0", "1e2", "01", "+1", "{}", "[]", "999999999999999999999999"} {
		if e := (Control{Action: "volume", Value: json.RawMessage(value), ControlTarget: target}).Validate(); e == nil {
			t.Fatalf("invalid percent accepted: %s", value)
		}
	}
	for _, guard := range []string{"", strings.Repeat("A", 32), strings.Repeat("a", 31), strings.Repeat("a", 33), strings.Repeat("g", 32)} {
		if e := (Control{Action: "volume", Value: json.RawMessage("50"), ControlTarget: guard}).Validate(); e == nil {
			t.Fatal("invalid generation accepted")
		}
	}
	if e := (Control{Action: "pause", ControlTarget: target}).Validate(); e != nil {
		t.Fatal("guarded legacy action rejected")
	}
	for _, state := range []string{"playing", "paused", "idle", "unavailable"} {
		zero := 0
		p := Playback{State: state, VolumePercent: &zero, ControlTarget: target, Capabilities: Capabilities{Volume: true}}.Bounded()
		active := state == "playing" || state == "paused"
		if (p.VolumePercent != nil) != active || p.Capabilities.Volume != active || (p.ControlTarget != "") != active {
			t.Fatal("state bounds", state)
		}
	}
}

func TestOriginalMetadataUTF8RoundTrip(t *testing.T) {
	for _, value := range []string{"ক্ষিতিজ শ্রদ্ধা ক্‌ষ ক্‍ষ", "夜に駆ける が が", "বাংলা ORBIT 東京", "কো কো"} {
		p := Playback{State: "playing", Title: value, Artist: value, Album: value, Context: Context{Type: "playlist", Name: value}}.Bounded()
		data, err := json.Marshal(p)
		if err != nil {
			t.Fatal(err)
		}
		var decoded Playback
		if err := json.Unmarshal(data, &decoded); err != nil {
			t.Fatal(err)
		}
		if decoded.Title != value || decoded.Artist != value || decoded.Album != value || decoded.Context.Name != value {
			t.Fatal("original Unicode metadata changed")
		}
	}
	// The producer preserves codepoints; shaping must handle a possibly incomplete final cluster.
	clipped := Text(strings.Repeat("a", 253)+"ক্ষ", 256)
	if !utf8.ValidString(clipped) || len(clipped) != 256 || !strings.HasSuffix(clipped, "ক") {
		t.Fatal("producer UTF-8 boundary changed")
	}
}

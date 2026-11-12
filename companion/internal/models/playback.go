package models

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"unicode/utf8"
)

const MaxDuration int64 = 86400000
const MaxTimestamp int64 = 9007199254740991

type Context struct {
	Type string `json:"type"`
	Name string `json:"name"`
}

type Artwork struct {
	URL    string `json:"url"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
}

type Capabilities struct {
	Play     bool `json:"play"`
	Pause    bool `json:"pause"`
	Previous bool `json:"previous"`
	Next     bool `json:"next"`
	Shuffle  bool `json:"shuffle"`
	Repeat   bool `json:"repeat"`
	Volume   bool `json:"volume"`
}

func (c Capabilities) Allows(action string) bool {
	switch action {
	case "play":
		return c.Play
	case "pause":
		return c.Pause
	case "previous":
		return c.Previous
	case "next":
		return c.Next
	case "shuffle":
		return c.Shuffle
	case "repeat":
		return c.Repeat
	case "volume":
		return c.Volume
	}
	return false
}

type Playback struct {
	SchemaVersion int          `json:"schema_version"`
	State         string       `json:"state"`
	TrackID       string       `json:"track_id"`
	Title         string       `json:"title"`
	Artist        string       `json:"artist"`
	Album         string       `json:"album"`
	Context       Context      `json:"context"`
	ProgressMS    int64        `json:"progress_ms"`
	DurationMS    int64        `json:"duration_ms"`
	UpdatedAtMS   int64        `json:"updated_at_ms"`
	Shuffle       bool         `json:"shuffle"`
	Repeat        string       `json:"repeat"`
	Artwork       Artwork      `json:"artwork"`
	Capabilities  Capabilities `json:"capabilities"`
	VolumePercent *int         `json:"volume_percent"`
	ControlTarget string       `json:"control_target"`
}

func Empty(state string) Playback {
	return Playback{SchemaVersion: 1, State: state, Context: Context{Type: "unknown"}, Repeat: "off"}
}

func Text(s string, limit int) string {
	s = strings.ToValidUTF8(s, "")
	s = strings.Map(func(r rune) rune {
		if r < 32 || r == 127 {
			return ' '
		}
		return r
	}, s)
	if len(s) <= limit {
		return s
	}
	s = s[:limit]
	for !utf8.ValidString(s) {
		s = s[:len(s)-1]
	}
	return s
}

func (p Playback) Bounded() Playback {
	p.SchemaVersion = 1
	switch p.State {
	case "playing", "paused", "idle", "unavailable":
	default:
		p.State = "unavailable"
	}
	p.TrackID = Text(p.TrackID, 128)
	p.Title, p.Artist, p.Album = Text(p.Title, 256), Text(p.Artist, 256), Text(p.Album, 256)
	p.Context.Name = Text(p.Context.Name, 256)
	if (p.Context.Type != "playlist" && p.Context.Type != "album") || p.Context.Name == "" {
		p.Context = Context{Type: "unknown"}
		if p.Album != "" {
			p.Context = Context{Type: "album", Name: p.Album}
		}
	}
	p.DurationMS = max(0, min(MaxDuration, p.DurationMS))
	p.ProgressMS = max(0, min(p.DurationMS, p.ProgressMS))
	p.UpdatedAtMS = max(0, min(MaxTimestamp, p.UpdatedAtMS))
	if p.Repeat != "context" && p.Repeat != "track" {
		p.Repeat = "off"
	}
	if !strings.HasPrefix(p.Artwork.URL, "/v1/artwork/") || len(p.Artwork.URL) > 256 || p.Artwork.Width < 16 || p.Artwork.Width > 240 || p.Artwork.Height < 16 || p.Artwork.Height > 240 {
		p.Artwork = Artwork{}
	}
	if p.State == "idle" || p.State == "unavailable" {
		p.Capabilities = Capabilities{}
		p.VolumePercent = nil
		p.ControlTarget = ""
	}
	if p.VolumePercent != nil {
		value := *p.VolumePercent
		p.VolumePercent = nil
		if value >= 0 && value <= 100 {
			p.VolumePercent = &value
		}
	}
	if !ValidControlTarget(p.ControlTarget) {
		p.ControlTarget = ""
		p.Capabilities.Volume = false
	}
	return p
}

type Control struct {
	Action        string          `json:"action"`
	Value         json.RawMessage `json:"value,omitempty"`
	ControlTarget string          `json:"control_target,omitempty"`
}

func ValidControlTarget(value string) bool {
	return len(value) == 32 && strings.IndexFunc(value, func(r rune) bool {
		return !strings.ContainsRune("0123456789abcdef", r)
	}) == -1
}

func (c Control) Validate() error {
	if c.ControlTarget != "" && !ValidControlTarget(c.ControlTarget) {
		return fmt.Errorf("invalid control target")
	}
	switch c.Action {
	case "play", "pause", "previous", "next":
		if len(c.Value) == 0 {
			return nil
		}
	case "shuffle":
		if string(c.Value) == "true" || string(c.Value) == "false" {
			return nil
		}
	case "repeat":
		var s string
		if json.Unmarshal(c.Value, &s) == nil && (s == "off" || s == "context" || s == "track") {
			return nil
		}
	case "volume":
		var value int
		raw := strings.TrimSpace(string(c.Value))
		if ValidControlTarget(c.ControlTarget) && raw != "" && raw[0] >= '0' && raw[0] <= '9' && json.Unmarshal(c.Value, &value) == nil && value >= 0 && value <= 100 {
			return nil
		}
	}
	return fmt.Errorf("invalid action or value")
}

func (c Control) Bool() bool     { return string(c.Value) == "true" }
func (c Control) String() string { var s string; _ = json.Unmarshal(c.Value, &s); return s }
func (c Control) Int() int       { var n int; _ = json.Unmarshal(c.Value, &n); return n }

type ServiceError struct {
	Code       string
	Message    string
	Status     int
	RetryAfter int
}

func (e *ServiceError) Error() string { return e.Code + ": " + e.Message }
func Problem(code string, status int, message string) *ServiceError {
	return &ServiceError{Code: code, Status: status, Message: message}
}

type Player interface {
	Snapshot() (Playback, *ServiceError)
	Control(context.Context, Control) *ServiceError
}

package models

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestSeekStrictBoundsAndGuards(t *testing.T) {
	guard := strings.Repeat("a", 32)
	for _, tc := range []struct {
		value string
		ok    bool
	}{
		{"0", true}, {"86400000", true}, {"1", true}, {"86400001", false}, {"-1", false}, {"-0", false}, {"1.0", false}, {"1e3", false}, {"null", false}, {`"1"`, false}, {"true", false}, {"", false},
	} {
		c := Control{Action: "seek", Value: json.RawMessage(tc.value), ControlTarget: guard, ControlTrack: guard}
		if (c.Validate() == nil) != tc.ok {
			t.Errorf("seek %q validation", tc.value)
		}
	}
	for _, missing := range []string{"target", "track", "invalid_track", "other_action"} {
		c := Control{Action: "seek", Value: json.RawMessage("1"), ControlTarget: guard, ControlTrack: guard}
		switch missing {
		case "target":
			c.ControlTarget = ""
		case "track":
			c.ControlTrack = ""
		case "invalid_track":
			c.ControlTrack = strings.Repeat("A", 32)
		case "other_action":
			c.Action = "volume"
		}
		if c.Validate() == nil {
			t.Errorf("accepted %s", missing)
		}
	}
}

func TestOptionalUsabilityFieldsDegrade(t *testing.T) {
	for _, value := range []string{`null`, `[]`, `true`, `"wrong"`, `{"layout":2}`, `{"layout":1,"id":"bad"}`, `{"layout":1,"id":"` + strings.Repeat("a", 64) + `","title":{"pages":["bad"],"flags":0}}`} {
		var p Playback
		if err := json.Unmarshal([]byte(`{"state":"playing","title":"kept","details":`+value+`,"offline_reason":`+value+`}`), &p); err != nil {
			t.Fatal(err)
		}
		p = p.Bounded()
		if p.State != "playing" || p.Title != "kept" || p.Details != nil || p.OfflineReason != "" {
			t.Fatalf("optional %s discarded playback or accepted invalid field", value)
		}
	}
	d := Details{Layout: 1, ID: strings.Repeat("a", 64), Title: &DetailField{Pages: []string{strings.Repeat("b", 64)}, Flags: 3}}
	copy := d.Bounded()
	if copy == nil {
		t.Fatal("valid detail rejected")
	}
	copy.Title.Pages[0] = "changed"
	if !ValidDigest(d.Title.Pages[0]) {
		t.Fatal("detail clone aliases source")
	}
	for _, count := range []int{4, 5} {
		d.Title.Pages = make([]string, count)
		for i := range d.Title.Pages {
			d.Title.Pages[i] = strings.Repeat("c", 64)
		}
		if (d.Bounded() != nil) != (count == 4) {
			t.Fatal("page bound")
		}
	}
	for _, reason := range []string{"login_required", "no_active_player", "wallet_locked", "stale", "new_reason", ""} {
		var r OfflineReason
		if json.Unmarshal([]byte(`"`+reason+`"`), &r) != nil {
			t.Fatal("reason decode")
		}
		if (r != "") != OfflineReason(reason).Valid() {
			t.Fatal("reason compatibility")
		}
	}
}

func TestDetailFieldPresenceAndFlags(t *testing.T) {
	for _, tc := range []struct {
		field string
		valid bool
	}{
		{`{"pages":[],"flags":0}`, true},
		{`{"pages":[],"flags":1}`, false},
		{`{"pages":[],"flags":null}`, false},
		{`{"pages":null,"flags":0}`, false},
		{`{"pages":[]}`, false},
		{`{"flags":0}`, false},
		{`{"pages":[],"flags":4}`, false},
		{`{"pages":[],"flags":0.0}`, false},
	} {
		var p Playback
		if json.Unmarshal([]byte(`{"state":"playing","details":{"layout":1,"id":"`+strings.Repeat("a", 64)+`","title":`+tc.field+`}}`), &p) != nil {
			t.Fatal("optional detail rejected base")
		}
		if (p.Bounded().Details != nil) != tc.valid {
			t.Errorf("field %s", tc.field)
		}
	}
}

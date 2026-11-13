package models

import (
	"encoding/json"
	"testing"
)

func TestOptionalClockBounds(t *testing.T) {
	for _, tc := range []struct {
		name, value string
		valid       bool
	}{
		{"minimum", `{"unix_ms":946684800000,"utc_offset_min":-840}`, true},
		{"maximum", `{"unix_ms":4102444799999,"utc_offset_min":840}`, true},
		{"kolkata", `{"unix_ms":1794515400000,"utc_offset_min":330}`, true},
		{"null", `null`, false}, {"array", `[]`, false}, {"string", `"clock"`, false},
		{"missing", `{}`, false}, {"missing_offset", `{"unix_ms":946684800000}`, false},
		{"null_offset", `{"unix_ms":946684800000,"utc_offset_min":null}`, false},
		{"early", `{"unix_ms":946684799999,"utc_offset_min":0}`, false},
		{"late", `{"unix_ms":4102444800000,"utc_offset_min":0}`, false},
		{"offset_low", `{"unix_ms":946684800000,"utc_offset_min":-841}`, false},
		{"offset_high", `{"unix_ms":946684800000,"utc_offset_min":841}`, false},
		{"fraction", `{"unix_ms":946684800000.0,"utc_offset_min":0}`, false},
		{"exponent", `{"unix_ms":1e12,"utc_offset_min":0}`, false},
		{"offset_fraction", `{"unix_ms":946684800000,"utc_offset_min":0.5}`, false},
		{"numeric_string", `{"unix_ms":"946684800000","utc_offset_min":0}`, false},
		{"unknown_field", `{"unix_ms":946684800000,"utc_offset_min":0,"other":1}`, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var p Playback
			if err := json.Unmarshal([]byte(`{"schema_version":1,"state":"playing","title":"ক্ষিতিজ / 夜","clock":`+tc.value+`}`), &p); err != nil {
				t.Fatal("optional clock discarded playback", err)
			}
			p = p.Bounded()
			if (p.Clock != nil) != tc.valid || p.Title != "ক্ষিতিজ / 夜" || p.State != "playing" {
				t.Fatal("clock validation or playback preservation")
			}
		})
	}
	p := Empty("idle")
	p.Clock = &Clock{UnixMS: MinClockMS, UTCOffsetMin: 330}
	bounded := p.Bounded()
	bounded.Clock.UTCOffsetMin = 0
	if p.Clock.UTCOffsetMin != 330 {
		t.Fatal("clock pointer alias")
	}
}

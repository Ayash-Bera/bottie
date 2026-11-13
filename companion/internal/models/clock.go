package models

import (
	"bytes"
	"encoding/json"
)

const MinClockMS int64 = 946684800000
const MaxClockMS int64 = 4102444799999

type Clock struct {
	UnixMS       int64 `json:"unix_ms"`
	UTCOffsetMin int   `json:"utc_offset_min"`
}

func (c Clock) Valid() bool {
	return c.UnixMS >= MinClockMS && c.UnixMS <= MaxClockMS && c.UTCOffsetMin >= -840 && c.UTCOffsetMin <= 840
}

// A malformed optional clock must not discard an otherwise usable playback snapshot.
func (c *Clock) UnmarshalJSON(data []byte) error {
	*c = Clock{}
	var fields struct {
		UnixMS *int64 `json:"unix_ms"`
		Offset *int   `json:"utc_offset_min"`
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if d.Decode(&fields) != nil || fields.UnixMS == nil || fields.Offset == nil {
		return nil
	}
	value := Clock{UnixMS: *fields.UnixMS, UTCOffsetMin: *fields.Offset}
	if value.Valid() {
		*c = value
	}
	return nil
}

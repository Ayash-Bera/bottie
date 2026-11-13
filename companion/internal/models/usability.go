package models

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"
)

type ControlResult struct {
	OK         bool   `json:"ok"`
	Result     string `json:"result,omitempty"`
	PositionMS *int64 `json:"position_ms,omitempty"`
}

func Accepted() ControlResult { return ControlResult{OK: true, Result: "accepted"} }

type OfflineReason string

func (r OfflineReason) Valid() bool {
	switch r {
	case "login_required", "no_active_player", "restricted", "rate_limited", "upstream_unavailable", "wallet_locked", "wallet_unavailable", "stale":
		return true
	}
	return false
}

func (r *OfflineReason) UnmarshalJSON(data []byte) error {
	*r = ""
	var value string
	if json.Unmarshal(data, &value) == nil && OfflineReason(value).Valid() {
		*r = OfflineReason(value)
	}
	return nil
}

func ReasonFor(code string) OfflineReason {
	switch code {
	case "not_authorized":
		return "login_required"
	case "no_device":
		return "no_active_player"
	case "restricted":
		return "restricted"
	case "rate_limited":
		return "rate_limited"
	case "keyring_locked":
		return "wallet_locked"
	case "keyring_invalid", "keyring_unavailable":
		return "wallet_unavailable"
	case "stale":
		return "stale"
	case "":
		return ""
	default:
		return "upstream_unavailable"
	}
}

type DetailField struct {
	Pages []string `json:"pages"`
	Flags int      `json:"flags"`
}

func (f *DetailField) UnmarshalJSON(data []byte) error {
	var value struct {
		Pages *[]string `json:"pages"`
		Flags *int      `json:"flags"`
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if d.Decode(&value) != nil || value.Pages == nil || value.Flags == nil || len(*value.Pages) > 4 || *value.Flags < 0 || *value.Flags > 3 {
		return fmt.Errorf("invalid detail field")
	}
	f.Pages, f.Flags = *value.Pages, *value.Flags
	return nil
}

type Details struct {
	Layout  int          `json:"layout"`
	ID      string       `json:"id"`
	Title   *DetailField `json:"title,omitempty"`
	Artist  *DetailField `json:"artist,omitempty"`
	Album   *DetailField `json:"album,omitempty"`
	Context *DetailField `json:"context,omitempty"`
}

func ValidDigest(s string) bool {
	return len(s) == 64 && strings.IndexFunc(s, func(r rune) bool { return !(r >= '0' && r <= '9' || r >= 'a' && r <= 'f') }) < 0
}

func (d Details) Fields() [4]*DetailField {
	return [4]*DetailField{d.Title, d.Artist, d.Album, d.Context}
}

func (d *Details) SetField(index int, f *DetailField) {
	switch index {
	case 0:
		d.Title = f
	case 1:
		d.Artist = f
	case 2:
		d.Album = f
	case 3:
		d.Context = f
	}
}

func (d Details) Bounded() *Details {
	if d.Layout != 1 || !ValidDigest(d.ID) {
		return nil
	}
	copy := Details{Layout: d.Layout, ID: d.ID}
	for index, field := range d.Fields() {
		if field == nil {
			continue
		}
		if field.Flags < 0 || field.Flags > 3 || len(field.Pages) > 4 || len(field.Pages) == 0 && field.Flags != 0 {
			return nil
		}
		for _, page := range field.Pages {
			if !ValidDigest(page) {
				return nil
			}
		}
		if len(field.Pages) > 0 {
			copy.SetField(index, &DetailField{Pages: append([]string(nil), field.Pages...), Flags: field.Flags})
		}
	}
	return &copy
}

func (d *Details) UnmarshalJSON(data []byte) error {
	*d = Details{}
	type raw Details
	var value raw
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&value) == nil {
		if valid := Details(value).Bounded(); valid != nil {
			*d = *valid
		}
	}
	return nil
}

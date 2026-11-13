package spotify

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"strings"

	"bottie/companion/internal/models"
)

type upstreamDevice struct {
	ID             string `json:"id"`
	Active         *bool  `json:"is_active"`
	Restricted     *bool  `json:"is_restricted"`
	SupportsVolume *bool  `json:"supports_volume"`
	VolumePercent  *int   `json:"volume_percent"`
}

func (d *upstreamDevice) usable() bool {
	return d != nil && d.Active != nil && *d.Active && d.Restricted != nil && !*d.Restricted
}
func (d *upstreamDevice) identified() bool {
	return d.usable() && len(d.ID) > 0 && len(d.ID) <= 256 && strings.IndexFunc(d.ID, func(r rune) bool { return r < 33 || r > 126 }) == -1
}

// DeviceStatus exposes capability evidence only on the loopback status route.
type DeviceStatus struct {
	Active         *bool `json:"active"`
	Restricted     *bool `json:"restricted"`
	SupportsVolume *bool `json:"supports_volume"`
	VolumePercent  *int  `json:"volume_percent"`
	UpdatedAtMS    int64 `json:"updated_at_ms"`
	Fresh          bool  `json:"fresh"`
}

func copyValue[T any](value *T) *T {
	if value == nil {
		return nil
	}
	n := *value
	return &n
}

func (p *Player) DeviceStatus() DeviceStatus {
	p.mu.RLock()
	defer p.mu.RUnlock()
	d := p.device
	age := p.now().UnixMilli() - d.UpdatedAtMS
	d.Fresh = p.problem == nil && d.UpdatedAtMS > 0 && age >= 0 && age <= 15000
	d.Active, d.Restricted = copyValue(d.Active), copyValue(d.Restricted)
	d.SupportsVolume, d.VolumePercent = copyValue(d.SupportsVolume), copyValue(d.VolumePercent)
	return d
}

func (p *Player) saveObservation(s models.Playback, d *upstreamDevice) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.saveObservationLocked(s, d)
}

func (p *Player) saveObservationLocked(s models.Playback, d *upstreamDevice) {
	p.device = DeviceStatus{UpdatedAtMS: s.UpdatedAtMS}
	if d != nil {
		p.device.Active, p.device.Restricted = copyValue(d.Active), copyValue(d.Restricted)
		p.device.SupportsVolume = copyValue(d.SupportsVolume)
		if d.VolumePercent != nil && *d.VolumePercent >= 0 && *d.VolumePercent <= 100 {
			p.device.VolumePercent = copyValue(d.VolumePercent)
			s.VolumePercent = copyValue(d.VolumePercent)
		}
	}
	if d.identified() && p.tokens.canControl && (s.State == "playing" || s.State == "paused") {
		age := s.UpdatedAtMS - p.cached.UpdatedAtMS
		if p.targetID == d.ID && age >= 0 && age <= 15000 {
			s.ControlTarget = p.cached.ControlTarget
		}
		if s.ControlTarget == "" {
			var nonce [16]byte
			if _, err := rand.Read(nonce[:]); err != nil {
				s.State = "unavailable"
				p.saveLocked(s, models.Problem("internal_error", 500, "cannot identify the playback target"))
				return
			}
			s.ControlTarget = hex.EncodeToString(nonce[:])
		}
		p.targetID = d.ID
		s.Capabilities.Volume = d.SupportsVolume != nil && *d.SupportsVolume
	} else {
		s.Capabilities = models.Capabilities{}
	}
	p.saveLocked(s, nil)
}

func targetChanged() *models.ServiceError {
	return models.Problem("target_changed", 409, "playback device changed; fetch playback before controlling it")
}

func (p *Player) checkTarget(ctx context.Context, action string) (string, *models.ServiceError) {
	p.mu.RLock()
	expected := p.targetID
	p.mu.RUnlock()
	b, status, e := p.request(ctx, http.MethodGet, "/me/player")
	if e != nil {
		p.fail(e)
		return "", e
	}
	var u upstream
	if status != 204 && json.Unmarshal(b, &u) != nil {
		e = p.backoff("upstream_error", "")
	} else if status == 204 || u.Device == nil || u.Device.ID != expected || expected == "" {
		e = targetChanged()
	} else {
		caps := u.capabilities(p.tokens.canControl)
		caps.Volume = u.Device.identified() && p.tokens.canControl && u.Device.SupportsVolume != nil && *u.Device.SupportsVolume && u.Item != nil && u.Item.Type == "track" && (u.Type == "" || u.Type == "track")
		if !caps.Allows(action) {
			e = models.Problem("unsupported", 409, "this control is unavailable for the active playback device")
		}
	}
	if e != nil {
		p.fail(e)
		return "", e
	}
	return expected, nil
}

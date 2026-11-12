package fixture

import (
	"context"
	"sync"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/models"
)

type Player struct {
	mu     sync.Mutex
	now    func() time.Time
	last   time.Time
	index  int
	state  models.Playback
	tracks []models.Playback
	volume int
}

const target = "00000000000000000000000000000001"

func New(store *artwork.Store, now func() time.Time) (*Player, error) {
	if now == nil {
		now = time.Now
	}
	first, err := store.AddOriginal("fixture-orbit", 0)
	if err != nil {
		return nil, err
	}
	second, err := store.AddOriginal("fixture-tide", 1)
	if err != nil {
		return nil, err
	}
	p := &Player{now: now, last: now(), volume: 40, tracks: []models.Playback{
		{TrackID: "fixture-orbit", Title: "Orbital Sunrise", Artist: "Bottie Ensemble", Album: "Small Worlds", Context: models.Context{Type: "playlist", Name: "Desk Orbits"}, DurationMS: 180000, Artwork: first},
		{TrackID: "fixture-tide", Title: "Quiet Tides", Artist: "Bottie Ensemble", Album: "Moonwater", DurationMS: 210000, Artwork: second},
		{TrackID: "fixture-signal", Title: "Distant Signal", Artist: "Bottie Ensemble", DurationMS: 150000},
	}}
	p.state = p.tracks[0]
	p.state.State = "paused"
	p.state.Repeat = "off"
	return p, nil
}

func (p *Player) selectTrack(index int) {
	state, shuffle, repeat := p.state.State, p.state.Shuffle, p.state.Repeat
	p.index = (index + len(p.tracks)) % len(p.tracks)
	p.state = p.tracks[p.index]
	p.state.State = state
	p.state.Shuffle = shuffle
	p.state.Repeat = repeat
}

func (p *Player) tick() {
	now := p.now()
	elapsed := max(int64(0), now.Sub(p.last).Milliseconds())
	p.last = now
	if p.state.State == "playing" {
		if p.state.Repeat == "track" {
			p.state.ProgressMS = (p.state.ProgressMS + elapsed) % p.state.DurationMS
		} else {
			if p.state.Repeat == "context" {
				var cycle int64
				for _, track := range p.tracks {
					cycle += track.DurationMS
				}
				elapsed %= cycle
			}
			for elapsed >= p.state.DurationMS-p.state.ProgressMS {
				elapsed -= p.state.DurationMS - p.state.ProgressMS
				last := len(p.tracks) - 1
				if p.state.Shuffle {
					last = 1
				}
				if p.state.Repeat != "context" && p.index == last {
					p.state.State = "paused"
					p.state.ProgressMS = p.state.DurationMS
					elapsed = 0
					break
				}
				step := 1
				if p.state.Shuffle {
					step = 2
				}
				p.selectTrack(p.index + step)
			}
			p.state.ProgressMS += elapsed
		}
	}
	p.state.UpdatedAtMS = now.UnixMilli()
	p.state.Capabilities = models.Capabilities{Play: p.state.State != "playing", Pause: p.state.State == "playing", Previous: true, Next: true, Shuffle: true, Repeat: true, Volume: true}
	p.state.ControlTarget = target
	p.state.VolumePercent = &p.volume
	p.state = p.state.Bounded()
}

func (p *Player) Snapshot() (models.Playback, *models.ServiceError) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.tick()
	return p.state.Bounded(), nil
}
func (p *Player) Control(ctx context.Context, c models.Control) *models.ServiceError {
	if err := c.Validate(); err != nil {
		return models.Problem("invalid_control", 400, "check action and value")
	}
	if ctx.Err() != nil {
		return models.Problem("upstream_error", 502, "request canceled")
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	p.tick()
	if c.ControlTarget != "" && c.ControlTarget != target {
		return models.Problem("target_changed", 409, "fixture target changed; fetch playback again")
	}
	switch c.Action {
	case "play":
		if p.state.ProgressMS == p.state.DurationMS {
			p.state.ProgressMS = 0
		}
		p.state.State = "playing"
	case "pause":
		p.state.State = "paused"
	case "previous":
		step := 1
		if p.state.Shuffle {
			step = 2
		}
		p.selectTrack(p.index - step)
	case "next":
		step := 1
		if p.state.Shuffle {
			step = 2
		}
		p.selectTrack(p.index + step)
	case "shuffle":
		p.state.Shuffle = c.Bool()
	case "repeat":
		p.state.Repeat = c.String()
	case "volume":
		p.volume = c.Int()
	}
	return nil
}

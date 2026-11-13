package spotify

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"time"

	"bottie/companion/internal/models"
)

type assetIdentity struct {
	track, title, artist, album, playlist, cover, target string
}

type assetJob struct {
	identity assetIdentity
	access   string
	ctx      context.Context
	cancel   context.CancelFunc
	done     bool
}

func (p *Player) cancelAssetsLocked() {
	if p.assetJob != nil {
		p.assetJob.cancel()
		p.assetJob = nil
	}
}

func (p *Player) publishMetadata(ctx context.Context, s models.Playback, d *upstreamDevice, playlist, cover string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	previous := p.cached
	p.saveObservationLocked(s, d)
	if p.problem != nil || p.cached.State == "unavailable" {
		return
	}
	key := assetIdentity{s.TrackID, s.Title, s.Artist, s.Album, playlist, cover, p.cached.ControlTarget}
	if p.assetJob != nil && p.assetJob.identity == key {
		p.cached.Artwork, p.cached.Context = previous.Artwork, previous.Context
		if !p.assetJob.done && p.assetJob.ctx.Err() == nil {
			return
		}
	}
	p.cancelAssetsLocked()
	if playlist == "" && cover == "" {
		return
	}
	jobCtx, cancel := context.WithCancel(ctx)
	p.assetJob = &assetJob{identity: key, access: p.tokens.access, ctx: jobCtx, cancel: cancel}
	select {
	case p.assetWake <- struct{}{}:
	default:
	}
}

func (p *Player) currentAssetLocked(job *assetJob) bool {
	s := p.cached
	k := job.identity
	age := p.now().UnixMilli() - s.UpdatedAtMS
	return p.assetJob == job && job.ctx.Err() == nil && p.problem == nil &&
		age >= 0 && age <= 15000 &&
		(s.State == "playing" || s.State == "paused") && s.TrackID == k.track &&
		s.Title == k.title && s.Artist == k.artist && s.Album == k.album && s.ControlTarget == k.target
}

// One worker and one replaceable job bound work even during rapid track changes.
func (p *Player) runAssets(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case <-p.assetWake:
		}
		p.mu.Lock()
		job := p.assetJob
		ready := job != nil && !job.done && p.currentAssetLocked(job)
		if job != nil && !ready {
			job.done = true
			job.cancel()
		}
		p.mu.Unlock()
		if !ready || ctx.Err() != nil {
			continue
		}
		p.loadAssets(job)
		p.mu.Lock()
		job.done = true
		job.cancel()
		p.mu.Unlock()
	}
}

func (p *Player) loadAssets(job *assetJob) {
	if job.identity.playlist != "" {
		name := p.loadPlaylist(job)
		p.mu.Lock()
		if p.currentAssetLocked(job) {
			if name != "" {
				p.cached.Context = models.Context{Type: "playlist", Name: name}
			} else {
				p.cached.Context = models.Context{Type: "album", Name: p.cached.Album}
				p.cached = p.cached.Bounded()
			}
		}
		p.mu.Unlock()
	}
	if job.ctx.Err() != nil || job.identity.cover == "" {
		return
	}
	ctx, cancel := context.WithTimeout(job.ctx, 8*time.Second)
	defer cancel()
	art, err := p.art.FromSpotify(ctx, job.identity.cover)
	if err != nil {
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.currentAssetLocked(job) {
		p.cached.Artwork = art
	}
}

func (p *Player) loadPlaylist(job *assetJob) string {
	id := job.identity.playlist
	p.mu.RLock()
	cached, ok := p.names[id]
	blocked := p.now().Before(p.assetBackoff)
	p.mu.RUnlock()
	if ok && p.now().Before(cached.expires) {
		return cached.name
	}
	if blocked || job.ctx.Err() != nil {
		return ""
	}
	if !p.acquire(job.ctx) {
		return ""
	}
	deferred := p.deferred()
	p.release()
	if deferred != nil {
		return ""
	}
	ctx, cancel := context.WithTimeout(job.ctx, 8*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.apiURL+"/playlists/"+id+"?fields=name", nil)
	if err != nil {
		return ""
	}
	// Optional reads use the captured grant without refreshing or serializing controls.
	req.Header.Set("Authorization", "Bearer "+job.access)
	e := nameEntry{expires: p.now().Add(time.Minute)}
	res, err := p.client.Do(req)
	if err == nil {
		defer res.Body.Close()
		switch res.StatusCode {
		case 429:
			until := p.now().Add(time.Duration(retrySeconds(res.Header.Get("Retry-After"), p.now(), 2)) * time.Second)
			p.mu.Lock()
			if until.After(p.assetBackoff) {
				p.assetBackoff = until
			}
			p.mu.Unlock()
			e.expires = until
		case 403, 404:
			e.expires = p.now().Add(5 * time.Minute)
		case 200:
			if res.ContentLength <= maxResponse {
				b, readErr := io.ReadAll(io.LimitReader(res.Body, maxResponse+1))
				var v struct {
					Name string `json:"name"`
				}
				if readErr == nil && len(b) <= maxResponse && json.Unmarshal(b, &v) == nil {
					e.name = models.Text(v.Name, 256)
					e.expires = p.now().Add(5 * time.Minute)
				}
			}
		}
	}
	if job.ctx.Err() != nil {
		return ""
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if !p.currentAssetLocked(job) {
		return ""
	}
	if len(p.names) >= 32 {
		p.names = make(map[string]nameEntry)
	}
	p.names[id] = e
	return e.name
}

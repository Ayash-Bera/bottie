package spotify

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/models"
)

const maxResponse = 256 << 10
const scopes = "user-read-playback-state user-modify-playback-state"

type flow struct {
	state, verifier string
	expires         time.Time
}
type token struct {
	access, refresh     string
	expires             time.Time
	canRead, canControl bool
}
type nameEntry struct {
	name    string
	expires time.Time
}

type Player struct {
	mu                             sync.RWMutex
	cached                         models.Playback
	problem                        *models.ServiceError
	op                             chan struct{}
	client                         *http.Client
	art                            *artwork.Store
	clientID, redirectURI          string
	apiURL, tokenURL, authorizeURL string
	now                            func() time.Time
	pending                        flow
	tokens                         token
	nextAttempt                    time.Time
	backoffCode                    string
	failures                       int
	names                          map[string]nameEntry
	nudge                          chan struct{}
}

func New(clientID, redirectURI string, art *artwork.Store, client *http.Client) *Player {
	if client == nil {
		client = &http.Client{Timeout: 8 * time.Second}
	}
	c := *client
	c.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return &Player{cached: models.Empty("unavailable"), problem: loginRequired(), op: make(chan struct{}, 1), client: &c, art: art, clientID: clientID, redirectURI: redirectURI, apiURL: "https://api.spotify.com/v1", tokenURL: "https://accounts.spotify.com/api/token", authorizeURL: "https://accounts.spotify.com/authorize", now: time.Now, names: make(map[string]nameEntry), nudge: make(chan struct{}, 1)}
}

func loginRequired() *models.ServiceError {
	return models.Problem("not_authorized", 503, "open the companion on this computer and connect Spotify")
}
func unavailable() *models.ServiceError {
	return models.Problem("upstream_error", 502, "Spotify is unavailable; retry after the backoff period")
}
func (p *Player) acquire(ctx context.Context) bool {
	select {
	case p.op <- struct{}{}:
		return true
	case <-ctx.Done():
		return false
	}
}
func (p *Player) release() { <-p.op }

func (p *Player) Snapshot() (models.Playback, *models.ServiceError) {
	p.mu.RLock()
	defer p.mu.RUnlock()
	s := p.cached
	e := p.problem
	if e != nil {
		copy := *e
		e = &copy
	}
	if s.State != "unavailable" && p.now().UnixMilli()-s.UpdatedAtMS > 15000 {
		s.State = "unavailable"
		s.Capabilities = models.Capabilities{}
		e = unavailable()
	}
	return s, e
}
func (p *Player) save(s models.Playback, e *models.ServiceError) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.cached = s.Bounded()
	p.problem = e
}
func (p *Player) fail(e *models.ServiceError) {
	s, _ := p.Snapshot()
	s.State = "unavailable"
	s.Capabilities = models.Capabilities{}
	p.save(s, e)
}

func randomString() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}
func (p *Player) Begin(ctx context.Context) (string, string, *models.ServiceError) {
	if !p.acquire(ctx) {
		return "", "", unavailable()
	}
	defer p.release()
	state, err := randomString()
	if err != nil {
		return "", "", unavailable()
	}
	verifier, err := randomString()
	if err != nil {
		return "", "", unavailable()
	}
	p.pending = flow{state: state, verifier: verifier, expires: p.now().Add(10 * time.Minute)}
	hash := sha256.Sum256([]byte(verifier))
	q := url.Values{"response_type": {"code"}, "client_id": {p.clientID}, "redirect_uri": {p.redirectURI}, "scope": {scopes}, "state": {state}, "code_challenge_method": {"S256"}, "code_challenge": {base64.RawURLEncoding.EncodeToString(hash[:])}}
	return p.authorizeURL + "?" + q.Encode(), state, nil
}

func (p *Player) Complete(ctx context.Context, state, cookie, code, providerError string) *models.ServiceError {
	if !p.acquire(ctx) {
		return unavailable()
	}
	defer p.release()
	if state == "" || len(state) > 128 || subtle.ConstantTimeCompare([]byte(state), []byte(cookie)) != 1 || subtle.ConstantTimeCompare([]byte(state), []byte(p.pending.state)) != 1 {
		return models.Problem("invalid_state", 400, "authorization state does not match; start connecting again")
	}
	f := p.pending
	p.pending = flow{}
	if !p.now().Before(f.expires) {
		return models.Problem("expired_state", 400, "authorization expired; start connecting again")
	}
	if providerError != "" {
		return models.Problem("authorization_denied", 400, "authorization was declined; start connecting again when ready")
	}
	if code == "" || len(code) > 4096 {
		return models.Problem("invalid_code", 400, "authorization code is missing or too long")
	}
	if e := p.exchange(ctx, url.Values{"grant_type": {"authorization_code"}, "code": {code}, "redirect_uri": {p.redirectURI}, "code_verifier": {f.verifier}}, false); e != nil {
		p.fail(e)
		return e
	}
	p.names = make(map[string]nameEntry)
	p.save(models.Empty("unavailable"), models.Problem("upstream_error", 502, "connected; waiting for the first playback poll"))
	select {
	case p.nudge <- struct{}{}:
	default:
	}
	return nil
}

func (p *Player) deferred() *models.ServiceError {
	if !p.now().Before(p.nextAttempt) {
		return nil
	}
	e := unavailable()
	if p.backoffCode == "rate_limited" {
		e = models.Problem("rate_limited", 429, "Spotify rate limit reached; wait before retrying")
	}
	e.RetryAfter = max(1, int((p.nextAttempt.Sub(p.now())+time.Second-1)/time.Second))
	return e
}
func retrySeconds(raw string, now time.Time, fallback int) int {
	if n, err := strconv.ParseInt(raw, 10, 64); err == nil && n >= 0 {
		return int(max(1, min(n, 315360000)))
	}
	if t, err := http.ParseTime(raw); err == nil && t.After(now) {
		return max(1, int(min(t.Sub(now), 10*365*24*time.Hour).Seconds()+1))
	}
	return fallback
}
func (p *Player) backoff(code string, header string) *models.ServiceError {
	p.failures = min(p.failures+1, 6)
	seconds := min(60, 1<<p.failures)
	if code == "rate_limited" {
		seconds = retrySeconds(header, p.now(), seconds)
	}
	p.nextAttempt = p.now().Add(time.Duration(seconds) * time.Second)
	p.backoffCode = code
	return p.deferred()
}

func (p *Player) send(ctx context.Context, method, target string, form url.Values, access string) ([]byte, int, *models.ServiceError) {
	if e := p.deferred(); e != nil {
		return nil, 0, e
	}
	var body io.Reader
	if form != nil {
		body = strings.NewReader(form.Encode())
	}
	req, err := http.NewRequestWithContext(ctx, method, target, body)
	if err != nil {
		return nil, 0, unavailable()
	}
	if form != nil {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if access != "" {
		req.Header.Set("Authorization", "Bearer "+access)
	}
	res, err := p.client.Do(req)
	if err != nil {
		return nil, 0, p.backoff("upstream_error", "")
	}
	defer res.Body.Close()
	if res.StatusCode == 429 {
		return nil, 429, p.backoff("rate_limited", res.Header.Get("Retry-After"))
	}
	if res.StatusCode >= 500 {
		return nil, res.StatusCode, p.backoff("upstream_error", "")
	}
	if res.ContentLength > maxResponse {
		return nil, res.StatusCode, p.backoff("upstream_error", "")
	}
	b, err := io.ReadAll(io.LimitReader(res.Body, maxResponse+1))
	if err != nil || len(b) > maxResponse {
		return nil, res.StatusCode, p.backoff("upstream_error", "")
	}
	return b, res.StatusCode, nil
}

func (p *Player) exchange(ctx context.Context, form url.Values, refresh bool) *models.ServiceError {
	form.Set("client_id", p.clientID)
	b, status, e := p.send(ctx, http.MethodPost, p.tokenURL, form, "")
	if e != nil {
		return e
	}
	if status != 200 {
		if status == 400 || status == 401 {
			p.tokens = token{}
			return loginRequired()
		}
		return p.backoff("upstream_error", "")
	}
	var t struct {
		Access  string  `json:"access_token"`
		Refresh string  `json:"refresh_token"`
		Type    string  `json:"token_type"`
		Expires int64   `json:"expires_in"`
		Scope   *string `json:"scope"`
	}
	if json.Unmarshal(b, &t) != nil || t.Access == "" || len(t.Access) > 8192 || len(t.Refresh) > 8192 || !strings.EqualFold(t.Type, "Bearer") || t.Expires < 1 || t.Expires > 86400 {
		return p.backoff("upstream_error", "")
	}
	if refresh && t.Refresh == "" {
		t.Refresh = p.tokens.refresh
	}
	read, control := false, false
	if t.Scope != nil {
		for _, s := range strings.Fields(*t.Scope) {
			if s == "user-read-playback-state" {
				read = true
			}
			if s == "user-modify-playback-state" {
				control = true
			}
		}
	} else if refresh {
		read, control = p.tokens.canRead, p.tokens.canControl
	}
	p.tokens = token{access: t.Access, refresh: t.Refresh, expires: p.now().Add(time.Duration(t.Expires) * time.Second), canRead: read, canControl: control}
	p.failures = 0
	p.nextAttempt = time.Time{}
	return nil
}

func (p *Player) ensureToken(ctx context.Context) *models.ServiceError {
	if e := p.deferred(); e != nil {
		return e
	}
	if p.tokens.access != "" && p.now().Before(p.tokens.expires.Add(-30*time.Second)) {
		return nil
	}
	if p.tokens.refresh == "" {
		p.tokens = token{}
		return loginRequired()
	}
	return p.exchange(ctx, url.Values{"grant_type": {"refresh_token"}, "refresh_token": {p.tokens.refresh}}, true)
}

func (p *Player) request(ctx context.Context, method, path string) ([]byte, int, *models.ServiceError) {
	if e := p.ensureToken(ctx); e != nil {
		return nil, 0, e
	}
	b, status, e := p.send(ctx, method, p.apiURL+path, nil, p.tokens.access)
	if e == nil && status == 401 {
		p.tokens.expires = time.Time{}
		if e = p.ensureToken(ctx); e != nil {
			return nil, 0, e
		}
		b, status, e = p.send(ctx, method, p.apiURL+path, nil, p.tokens.access)
	}
	if e != nil {
		return nil, status, e
	}
	switch status {
	case 200, 204:
		return b, status, nil
	case 401:
		p.tokens = token{}
		return nil, status, loginRequired()
	case 403:
		return nil, status, models.Problem("restricted", 403, "playback is restricted; check Premium, app access and requested permissions")
	case 404:
		return nil, status, models.Problem("no_device", 503, "start Spotify on an available playback device")
	default:
		return nil, status, p.backoff("upstream_error", "")
	}
}

type upstream struct {
	Device *struct {
		Active     bool `json:"is_active"`
		Restricted bool `json:"is_restricted"`
	} `json:"device"`
	Playing  bool   `json:"is_playing"`
	Progress int64  `json:"progress_ms"`
	Shuffle  bool   `json:"shuffle_state"`
	Repeat   string `json:"repeat_state"`
	Type     string `json:"currently_playing_type"`
	Context  *struct {
		Type string `json:"type"`
		URI  string `json:"uri"`
	} `json:"context"`
	Item *struct {
		ID       string `json:"id"`
		Name     string `json:"name"`
		Type     string `json:"type"`
		Duration int64  `json:"duration_ms"`
		Artists  []struct {
			Name string `json:"name"`
		} `json:"artists"`
		Album struct {
			Name   string `json:"name"`
			Images []struct {
				URL string `json:"url"`
			} `json:"images"`
		} `json:"album"`
	} `json:"item"`
	Actions map[string]json.RawMessage `json:"actions"`
}

func (u upstream) capabilities(control bool) models.Capabilities {
	if !control || u.Device == nil || !u.Device.Active || u.Device.Restricted || u.Item == nil || u.Item.Type != "track" {
		return models.Capabilities{}
	}
	disallowed := map[string]bool{}
	// Both deployed disallows wrappers and the current reference's flat shape occur.
	for k, v := range u.Actions {
		if k == "disallows" {
			var m map[string]bool
			if json.Unmarshal(v, &m) == nil {
				for a, b := range m {
					disallowed[a] = disallowed[a] || b
				}
			}
		} else {
			var b bool
			if json.Unmarshal(v, &b) == nil {
				disallowed[k] = disallowed[k] || b
			}
		}
	}
	return models.Capabilities{Play: !u.Playing && !disallowed["resuming"], Pause: u.Playing && !disallowed["pausing"], Previous: !disallowed["skipping_prev"], Next: !disallowed["skipping_next"], Shuffle: !disallowed["toggling_shuffle"], Repeat: !disallowed["toggling_repeat_context"] && !disallowed["toggling_repeat_track"]}
}

func playlistID(uri string) string {
	id := strings.TrimPrefix(uri, "spotify:playlist:")
	if id == uri || len(id) != 22 {
		return ""
	}
	for _, r := range id {
		if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9') {
			return ""
		}
	}
	return id
}
func (p *Player) playlistName(ctx context.Context, uri string) string {
	id := playlistID(uri)
	if id == "" {
		return ""
	}
	if e, ok := p.names[id]; ok && p.now().Before(e.expires) {
		return e.name
	}
	if len(p.names) >= 32 {
		p.names = make(map[string]nameEntry)
	}
	e := nameEntry{expires: p.now().Add(5 * time.Minute)}
	b, status, err := p.request(ctx, http.MethodGet, "/playlists/"+id+"?fields=name")
	if err == nil && status == 200 {
		var v struct {
			Name string `json:"name"`
		}
		if json.Unmarshal(b, &v) == nil {
			e.name = models.Text(v.Name, 256)
		}
	}
	p.names[id] = e
	return e.name
}

func (p *Player) Poll(ctx context.Context) {
	if !p.acquire(ctx) {
		return
	}
	defer p.release()
	if e := p.ensureToken(ctx); e != nil {
		p.fail(e)
		return
	}
	if !p.tokens.canRead {
		p.fail(models.Problem("restricted", 403, "reconnect with playback read permission"))
		return
	}
	b, status, e := p.request(ctx, http.MethodGet, "/me/player")
	if e != nil {
		p.fail(e)
		return
	}
	s := models.Empty("idle")
	s.UpdatedAtMS = p.now().UnixMilli()
	if status == 204 {
		p.failures = 0
		p.save(s, nil)
		return
	}
	var u upstream
	if json.Unmarshal(b, &u) != nil {
		p.fail(p.backoff("upstream_error", ""))
		return
	}
	if u.Item == nil {
		p.save(s, nil)
		return
	}
	if u.Item.Type != "track" || (u.Type != "" && u.Type != "track") {
		p.fail(models.Problem("restricted", 403, "this playback type is not supported"))
		return
	}
	s.State = "paused"
	if u.Playing {
		s.State = "playing"
	}
	s.TrackID = u.Item.ID
	s.Title = u.Item.Name
	s.Album = u.Item.Album.Name
	artists := make([]string, 0, len(u.Item.Artists))
	for _, a := range u.Item.Artists {
		artists = append(artists, a.Name)
	}
	s.Artist = strings.Join(artists, ", ")
	s.ProgressMS = u.Progress
	s.DurationMS = u.Item.Duration
	s.Shuffle = u.Shuffle
	s.Repeat = u.Repeat
	s.Capabilities = u.capabilities(p.tokens.canControl)
	if u.Context != nil && u.Context.Type == "playlist" {
		s.Context = models.Context{Type: "playlist", Name: p.playlistName(ctx, u.Context.URI)}
	}
	if p.tokens.access == "" {
		p.fail(loginRequired())
		return
	}
	for _, im := range u.Item.Album.Images {
		if artwork.Trusted(im.URL) {
			if a, err := p.art.FromSpotify(ctx, im.URL); err == nil {
				s.Artwork = a
			}
			break
		}
	}
	if e := p.deferred(); e != nil {
		s.State = "unavailable"
		s.Capabilities = models.Capabilities{}
		p.save(s, e)
		return
	}
	p.failures = 0
	p.save(s, nil)
}

func (p *Player) Run(ctx context.Context, interval time.Duration) {
	p.Poll(ctx)
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			p.Poll(ctx)
		case <-p.nudge:
			p.Poll(ctx)
		}
	}
}

func (p *Player) Control(ctx context.Context, c models.Control) *models.ServiceError {
	if c.Validate() != nil {
		return models.Problem("invalid_control", 400, "check action and value")
	}
	if !p.acquire(ctx) {
		return unavailable()
	}
	defer p.release()
	if e := p.deferred(); e != nil {
		return e
	}
	s, e := p.Snapshot()
	if e != nil {
		return e
	}
	if !s.Capabilities.Allows(c.Action) {
		return models.Problem("unsupported", 409, "this control is unavailable for the active playback device")
	}
	method, path := http.MethodPut, "/me/player/"+c.Action
	if c.Action == "next" || c.Action == "previous" {
		method = http.MethodPost
	}
	if c.Action == "shuffle" {
		path += "?state=" + strconv.FormatBool(c.Bool())
	}
	if c.Action == "repeat" {
		path += "?state=" + c.String()
	}
	_, _, e = p.request(ctx, method, path)
	if e != nil {
		p.fail(e)
		return e
	}
	select {
	case p.nudge <- struct{}{}:
	default:
	}
	return nil
}

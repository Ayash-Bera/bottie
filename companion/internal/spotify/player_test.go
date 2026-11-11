package spotify

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/models"
)

const playbackJSON = `{"device":{"is_active":true,"is_restricted":false},"is_playing":true,"progress_ms":1000,"shuffle_state":false,"repeat_state":"off","currently_playing_type":"track","item":{"type":"track","id":"track1","name":"Example","duration_ms":60000,"artists":[{"name":"Artist"}],"album":{"name":"Album","images":[]}},"context":{"type":"playlist","uri":"spotify:playlist:1234567890123456789012"},"actions":{"disallows":{"skipping_next":true}}}`
const tokenJSON = `{"access_token":"access-secret","refresh_token":"refresh-secret","token_type":"Bearer","expires_in":3600,"scope":"user-read-playback-state user-modify-playback-state"}`

func fakePlayer(t *testing.T, h http.HandlerFunc) (*Player, *time.Time) {
	t.Helper()
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	p := New(strings.Repeat("a", 32), "http://127.0.0.1:8787/auth/callback", artwork.New(96, nil), srv.Client())
	p.apiURL = srv.URL
	p.tokenURL = srv.URL + "/token"
	p.authorizeURL = srv.URL + "/authorize"
	now := time.Unix(1700000000, 0)
	p.now = func() time.Time { return now }
	return p, &now
}
func authorize(p *Player) {
	p.tokens = token{access: "access-secret", refresh: "refresh-secret", expires: p.now().Add(time.Hour), canRead: true, canControl: true}
}

func TestPKCEStateLifecycle(t *testing.T) {
	for _, scenario := range []string{"success", "wrong_state", "wrong_cookie", "expired", "denied", "missing_code", "exchange_failure"} {
		t.Run(scenario, func(t *testing.T) {
			var calls atomic.Int32
			var challenge string
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				if err := r.ParseForm(); err != nil {
					t.Error(err)
				}
				if r.Form.Get("grant_type") != "authorization_code" || r.Form.Get("code") != "one-use-code" || r.Form.Get("redirect_uri") != "http://127.0.0.1:8787/auth/callback" {
					t.Error("exchange form")
				}
				verifier := r.Form.Get("code_verifier")
				sum := sha256.Sum256([]byte(verifier))
				if len(verifier) < 43 || base64.RawURLEncoding.EncodeToString(sum[:]) != challenge {
					t.Error("PKCE mismatch")
				}
				if r.Header.Get("Authorization") != "" {
					t.Error("unexpected client secret")
				}
				if scenario == "exchange_failure" {
					w.WriteHeader(400)
					return
				}
				_, _ = io.WriteString(w, tokenJSON)
			})
			target, state, e := p.Begin(context.Background())
			if e != nil {
				t.Fatal(e)
			}
			u, _ := url.Parse(target)
			q := u.Query()
			challenge = q.Get("code_challenge")
			if q.Get("code_challenge_method") != "S256" || q.Get("state") != state || q.Get("scope") != scopes {
				t.Fatal("authorize parameters")
			}
			given, cookie, code, denial := state, state, "one-use-code", ""
			switch scenario {
			case "wrong_state":
				given = "wrong"
			case "wrong_cookie":
				cookie = "wrong"
			case "expired":
				*now = now.Add(11 * time.Minute)
			case "denied":
				denial = "access_denied"
			case "missing_code":
				code = ""
			}
			e = p.Complete(context.Background(), given, cookie, code, denial)
			if scenario == "success" {
				if e != nil {
					t.Fatal(e)
				}
				if p.tokens.access != "access-secret" {
					t.Fatal("token not retained")
				}
			} else if e == nil {
				t.Fatal("invalid callback accepted")
			}
			want := int32(0)
			if scenario == "success" || scenario == "exchange_failure" {
				want = 1
			}
			if calls.Load() != want {
				t.Fatal("unexpected token exchange")
			}
			if scenario != "wrong_state" && scenario != "wrong_cookie" {
				if p.Complete(context.Background(), state, state, "one-use-code", "") == nil {
					t.Fatal("replay accepted")
				}
				if calls.Load() != want {
					t.Fatal("replay called upstream")
				}
			}
		})
	}
}

func TestRefreshAndUnauthorized(t *testing.T) {
	for _, scenario := range []string{"retain", "rotate", "invalid_grant", "unauthorized_retry"} {
		t.Run(scenario, func(t *testing.T) {
			var refreshes, reads atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/token" {
					refreshes.Add(1)
					_ = r.ParseForm()
					if r.Form.Get("grant_type") != "refresh_token" || r.Form.Get("refresh_token") != "refresh-secret" || r.Form.Get("client_id") == "" {
						t.Error("refresh form")
					}
					if scenario == "invalid_grant" {
						w.WriteHeader(400)
						_, _ = io.WriteString(w, `{"error":"invalid_grant"}`)
						return
					}
					if scenario == "rotate" {
						_, _ = io.WriteString(w, `{"access_token":"renewed","refresh_token":"rotated","expires_in":3600,"token_type":"Bearer"}`)
					} else {
						_, _ = io.WriteString(w, `{"access_token":"renewed","expires_in":3600,"token_type":"Bearer"}`)
					}
					return
				}
				if r.URL.Path == "/me/player" {
					n := reads.Add(1)
					if scenario == "unauthorized_retry" && n == 1 {
						w.WriteHeader(401)
						return
					}
					if r.Header.Get("Authorization") != "Bearer renewed" {
						t.Error("old access token")
					}
					w.WriteHeader(204)
					return
				}
				t.Error("unexpected request")
			})
			authorize(p)
			if scenario != "unauthorized_retry" {
				p.tokens.expires = p.now()
			}
			p.Poll(context.Background())
			s, e := p.Snapshot()
			if refreshes.Load() != 1 {
				t.Fatal("refresh count")
			}
			if scenario == "invalid_grant" {
				if e == nil || e.Code != "not_authorized" || p.tokens.refresh != "" {
					t.Fatal("invalid grant not cleared")
				}
				return
			}
			if e != nil || s.State != "idle" {
				t.Fatalf("snapshot %+v %v", s, e)
			}
			want := "refresh-secret"
			if scenario == "rotate" {
				want = "rotated"
			}
			if p.tokens.refresh != want {
				t.Fatal("refresh retention")
			}
			if !p.tokens.canControl {
				t.Fatal("scope not retained")
			}
		})
	}
}

func TestPollCacheRestrictionsAndFallback(t *testing.T) {
	for _, scenario := range []string{"playlist", "missing_playlist", "restricted_device", "flat_actions", "missing_item", "unsupported_type", "overlong_metadata"} {
		t.Run(scenario, func(t *testing.T) {
			var calls atomic.Int32
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				if r.URL.Path == "/playlists/1234567890123456789012" {
					if r.URL.RawQuery != "fields=name" {
						t.Error("unbounded playlist query")
					}
					if scenario == "missing_playlist" {
						w.WriteHeader(403)
					} else {
						_, _ = io.WriteString(w, `{"name":"Evening Orbits"}`)
					}
					return
				}
				body := playbackJSON
				switch scenario {
				case "restricted_device":
					body = strings.Replace(body, `"is_restricted":false`, `"is_restricted":true`, 1)
				case "flat_actions":
					body = strings.Replace(body, `"disallows":{"skipping_next":true}`, `"skipping_next":true,"toggling_repeat_track":true`, 1)
				case "missing_item":
					body = `{"item":null}`
				case "unsupported_type":
					body = strings.ReplaceAll(body, `"track"`, `"episode"`)
				case "overlong_metadata":
					body = strings.Replace(body, `"Example"`, `"`+strings.Repeat("星", 500)+`"`, 1)
				}
				_, _ = io.WriteString(w, body)
			})
			authorize(p)
			p.Poll(context.Background())
			s, e := p.Snapshot()
			if scenario == "unsupported_type" {
				if e == nil || s.State != "unavailable" {
					t.Fatal("unsupported type accepted")
				}
				return
			}
			if e != nil {
				t.Fatal(e)
			}
			if scenario == "missing_item" {
				if s.State != "idle" {
					t.Fatal("missing item")
				}
				return
			}
			if s.Context.Type == "playlist" && s.Context.Name != "Evening Orbits" {
				t.Fatal("playlist name")
			}
			if scenario == "missing_playlist" && s.Context.Type != "album" {
				t.Fatal("album fallback")
			}
			if s.Capabilities.Next {
				t.Fatal("next restriction ignored")
			}
			if scenario == "restricted_device" && s.Capabilities.Pause {
				t.Fatal("restricted device")
			}
			if scenario == "flat_actions" && s.Capabilities.Repeat {
				t.Fatal("repeat restriction")
			}
			if len(s.Title) > 256 {
				t.Fatal("unbounded title")
			}
			before := calls.Load()
			for i := 0; i < 100; i++ {
				_, _ = p.Snapshot()
			}
			if calls.Load() != before {
				t.Fatal("snapshot made upstream calls")
			}
			*now = now.Add(16 * time.Second)
			s, e = p.Snapshot()
			if s.State != "unavailable" || s.Capabilities.Pause || e == nil {
				t.Fatal("stale cache accepted")
			}
			p.Poll(context.Background())
			if calls.Load() != before+1 {
				t.Fatal("playlist cache ignored")
			}
		})
	}
}

func TestBackoffAndResponseBounds(t *testing.T) {
	for _, scenario := range []struct {
		name    string
		status  int
		header  string
		body    string
		seconds int
		code    string
	}{
		{"seconds", 429, "120", "", 120, "rate_limited"},
		{"date", 429, time.Unix(1700000090, 0).UTC().Format(http.TimeFormat), "", 91, "rate_limited"},
		{"missing header", 429, "", "", 2, "rate_limited"},
		{"server", 503, "", "", 2, "upstream_error"},
		{"malformed", 200, "", "broken", 2, "upstream_error"},
		{"oversize", 200, "", strings.Repeat("x", maxResponse+1), 2, "upstream_error"},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			var calls atomic.Int32
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				w.Header().Set("Retry-After", scenario.header)
				w.WriteHeader(scenario.status)
				_, _ = io.WriteString(w, scenario.body)
			})
			authorize(p)
			p.Poll(context.Background())
			s, e := p.Snapshot()
			if e == nil || e.Code != scenario.code || e.RetryAfter != scenario.seconds || s.State != "unavailable" {
				t.Fatalf("snapshot %+v, error %v", s, e)
			}
			for i := 0; i < 5; i++ {
				p.Poll(context.Background())
				_ = p.Control(context.Background(), models.Control{Action: "next"})
			}
			if calls.Load() != 1 {
				t.Fatal("backoff bypassed")
			}
			*now = now.Add(time.Duration(scenario.seconds) * time.Second)
			p.Poll(context.Background())
			if calls.Load() != 2 {
				t.Fatal("backoff never retried")
			}
		})
	}
}

func TestControlMethodsAndErrors(t *testing.T) {
	for _, tt := range []struct{ action, value, method, path string }{{"play", "", "PUT", "/me/player/play"}, {"pause", "", "PUT", "/me/player/pause"}, {"previous", "", "POST", "/me/player/previous"}, {"next", "", "POST", "/me/player/next"}, {"shuffle", "true", "PUT", "/me/player/shuffle?state=true"}, {"repeat", `"track"`, "PUT", "/me/player/repeat?state=track"}} {
		t.Run(tt.action, func(t *testing.T) {
			var calls atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				if r.Method != tt.method || r.URL.RequestURI() != tt.path {
					t.Errorf("got %s %s", r.Method, r.URL.RequestURI())
				}
				w.WriteHeader(204)
			})
			authorize(p)
			s := models.Empty("paused")
			s.UpdatedAtMS = p.now().UnixMilli()
			s.Capabilities = models.Capabilities{Play: true, Pause: true, Previous: true, Next: true, Shuffle: true, Repeat: true}
			p.save(s, nil)
			if e := p.Control(context.Background(), models.Control{Action: tt.action, Value: json.RawMessage(tt.value)}); e != nil {
				t.Fatal(e)
			}
			if calls.Load() != 1 {
				t.Fatal("control request count")
			}
		})
	}
	for _, status := range []int{403, 404, 503} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			var calls atomic.Int32
			p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				w.WriteHeader(status)
				_, _ = io.WriteString(w, "private-upstream-error-access-secret")
			})
			authorize(p)
			s := models.Empty("playing")
			s.UpdatedAtMS = p.now().UnixMilli()
			s.Capabilities.Next = true
			p.save(s, nil)
			e := p.Control(context.Background(), models.Control{Action: "next"})
			if e == nil || strings.Contains(e.Error(), "secret") || calls.Load() != 1 {
				t.Fatal("unsafe error or replay")
			}
			s, _ = p.Snapshot()
			if s.Capabilities.Next {
				t.Fatal("failed control did not disable capabilities")
			}
		})
	}
}

func TestRunCancellationAndNoCredentials(t *testing.T) {
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) { t.Error("request without credentials") })
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { p.Run(ctx, time.Hour); close(done) }()
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("poll loop did not stop")
	}
}

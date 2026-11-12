package api

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/config"
	"bottie/companion/internal/credentials"
	"bottie/companion/internal/fixture"
	"bottie/companion/internal/models"
	"bottie/companion/internal/spotify"
)

func fixtureServer(t *testing.T, token string) (http.Handler, *bytes.Buffer) {
	t.Helper()
	cfg, e := config.Parse(func(k string) string {
		if k == "BOTTIE_DEVICE_TOKEN" {
			return token
		}
		return ""
	})
	if e != nil {
		t.Fatal(e)
	}
	store := artwork.New(96, nil)
	p, err := fixture.New(store, func() time.Time { return time.Unix(1700000000, 0) })
	if err != nil {
		t.Fatal(err)
	}
	log := &bytes.Buffer{}
	return New(cfg, p, store, nil, slog.New(slog.NewTextHandler(log, nil))), log
}
func request(h http.Handler, method, path, body, token, origin string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, "http://127.0.0.1:8787"+path, strings.NewReader(body))
	r.RemoteAddr = "127.0.0.1:40000"
	r.Header.Set("Content-Type", "application/json")
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	if origin != "" {
		r.Header.Set("Origin", origin)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}

func TestHTTPControlsAndBounds(t *testing.T) {
	h, _ := fixtureServer(t, "")
	for _, tt := range []struct {
		body   string
		status int
	}{
		{`{"action":"play"}`, 200}, {`{"action":"pause"}`, 200}, {`{"action":"previous"}`, 200}, {`{"action":"next"}`, 200}, {`{"action":"shuffle","value":true}`, 200}, {`{"action":"repeat","value":"context"}`, 200},
		{`{"action":"play","value":null}`, 400}, {`{"action":"shuffle","value":null}`, 400}, {`{"action":"repeat","value":true}`, 400}, {`{"action":"seek"}`, 400}, {`{"action":"play","other":1}`, 400}, {`{"action":"play","action":"next"}`, 400}, {`{"action":"play"} {}`, 400}, {`[]`, 400}, {`{"action":`, 400}, {strings.Repeat("x", 1025), 413},
	} {
		t.Run(tt.body[:min(len(tt.body), 30)], func(t *testing.T) {
			w := request(h, "POST", "/v1/control", tt.body, "", "")
			if w.Code != tt.status {
				t.Fatalf("status %d, %s", w.Code, w.Body.String())
			}
			if w.Body.Len() > 1024 {
				t.Fatal("error too large")
			}
		})
	}
	w := request(h, "GET", "/v1/playback", "", "", "")
	var p models.Playback
	if e := json.Unmarshal(w.Body.Bytes(), &p); e != nil {
		t.Fatal(e)
	}
	if w.Code != 200 || w.Body.Len() > 8192 || p.SchemaVersion != 1 {
		t.Fatal("playback response")
	}
	w = request(h, "GET", p.Artwork.URL, "", "", "")
	if w.Code != 200 || w.Body.Len() != 18448 || string(w.Body.Bytes()[:4]) != "BTAR" {
		t.Fatal("artwork transport")
	}
	w = request(h, "GET", "/v1/artwork/missing", "", "", "")
	if w.Code != 404 {
		t.Fatal("missing art")
	}
	r := httptest.NewRequest("POST", "http://127.0.0.1:8787/v1/control", strings.NewReader(`{"action":"play"}`))
	w = httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 415 {
		t.Fatal("content type")
	}
}

func TestAuthenticationOriginAndLogs(t *testing.T) {
	secret := strings.Repeat("private-device-secret", 2)
	h, logs := fixtureServer(t, secret)
	for _, tt := range []struct {
		token, origin string
		status        int
	}{{"", "", 401}, {"wrong", "", 401}, {secret, "", 200}, {secret, "http://evil.invalid", 403}, {secret, "http://127.0.0.1:8787", 200}} {
		w := request(h, "GET", "/v1/playback?token=never-log-this", "", tt.token, tt.origin)
		if w.Code != tt.status {
			t.Fatalf("want %d got %d", tt.status, w.Code)
		}
	}
	if strings.Contains(logs.String(), secret) || strings.Contains(logs.String(), "never-log-this") {
		t.Fatal("credentials logged")
	}
	r := httptest.NewRequest("GET", "http://rebound.invalid:8787/v1/playback", nil)
	r.Header.Set("Authorization", "Bearer "+secret)
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 403 {
		t.Fatal("untrusted host accepted")
	}
	r = httptest.NewRequest("GET", "http://127.0.0.1:8787/status", nil)
	r.RemoteAddr = "192.168.1.5:40000"
	w = httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 403 {
		t.Fatal("remote admin accepted")
	}
	w = request(h, "GET", "/v1/artwork/fixture-orbit", "", "", "")
	if w.Code != 401 {
		t.Fatal("unauthenticated artwork")
	}
}

type fakeOAuth struct{ calls, begins int }

func (f *fakeOAuth) Begin(context.Context) (string, string, *models.ServiceError) {
	f.begins++
	return "https://accounts.spotify.com/authorize?state=test-state", "test-state", nil
}
func (f *fakeOAuth) Complete(ctx context.Context, state, cookie, code, providerError string) *models.ServiceError {
	f.calls++
	if state != cookie {
		return models.Problem("invalid_state", 400, "state mismatch")
	}
	return nil
}

func TestOAuthFormOrigins(t *testing.T) {
	cfg, _ := config.Parse(func(string) string { return "" })
	store := artwork.New(96, nil)
	p, _ := fixture.New(store, nil)
	oauth := &fakeOAuth{}
	h := New(cfg, p, store, oauth, slog.New(slog.NewTextHandler(io.Discard, nil)))
	home := request(h, "GET", "/", "", "", "")
	if home.Code != http.StatusOK || home.Header().Get("Referrer-Policy") != "same-origin" {
		t.Fatal("home policy must preserve the origin of its browser form POST")
	}
	if !strings.Contains(home.Body.String(), "<form method=post action=/auth/login>") {
		t.Fatal("local login form missing")
	}
	for _, tt := range []struct {
		name, origin, site string
		status             int
	}{
		{"local", "http://127.0.0.1:8787", "same-origin", http.StatusSeeOther},
		{"foreign", "https://foreign.invalid", "cross-site", http.StatusForbidden},
		{"foreign_spoofed_metadata", "https://foreign.invalid", "same-origin", http.StatusForbidden},
		{"null_spoofed_metadata", "null", "same-origin", http.StatusForbidden},
		{"local_cross_site_metadata", "http://127.0.0.1:8787", "cross-site", http.StatusForbidden},
	} {
		t.Run(tt.name, func(t *testing.T) {
			before := oauth.begins
			r := httptest.NewRequest("POST", "http://127.0.0.1:8787/auth/login", nil)
			r.RemoteAddr = "127.0.0.1:40000"
			r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			r.Header.Set("Origin", tt.origin)
			r.Header.Set("Sec-Fetch-Site", tt.site)
			r.Header.Set("Sec-Fetch-Mode", "navigate")
			r.Header.Set("Sec-Fetch-Dest", "document")
			r.Header.Set("Sec-Fetch-User", "?1")
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if w.Code != tt.status {
				t.Fatalf("status: got %d, want %d", w.Code, tt.status)
			}
			if w.Header().Get("Referrer-Policy") != "no-referrer" {
				t.Fatal("login response must retain no-referrer")
			}
			if tt.status == http.StatusSeeOther {
				if oauth.begins != before+1 || w.Header().Get("Location") == "" {
					t.Fatal("local form did not begin authorization")
				}
			} else if oauth.begins != before || w.Header().Get("Location") != "" || len(w.Result().Cookies()) != 0 {
				t.Fatal("rejected origin started authorization")
			}
		})
	}
}

func TestProtectedLoginStatus(t *testing.T) {
	cfg, _ := config.Parse(func(k string) string {
		if k == "BOTTIE_TOKEN_STORE" {
			return "keyring"
		}
		return ""
	})
	art := artwork.New(96, nil)
	p := spotify.NewWithStore("test-client", "http://127.0.0.1:8787/auth/callback", art, nil, credentials.NewKeyring("test-client", "http://127.0.0.1:8787/auth/callback"))
	h := New(cfg, p, art, p, slog.New(slog.NewTextHandler(io.Discard, nil)))
	home := request(h, "GET", "/", "", "", "")
	if !strings.Contains(home.Body.String(), "protected desktop wallet") || strings.Contains(home.Body.String(), "clears the connection") {
		t.Fatal("wrong persistence instructions")
	}
	w := request(h, "GET", "/status", "", "", "")
	var v struct {
		Authorization struct {
			Authorized bool   `json:"authorized"`
			Storage    string `json:"storage"`
		} `json:"authorization"`
	}
	if json.Unmarshal(w.Body.Bytes(), &v) != nil || v.Authorization.Authorized || v.Authorization.Storage != "pending" {
		t.Fatal("safe status")
	}
	if strings.Contains(w.Body.String(), "test-client") || strings.Contains(w.Body.String(), "refresh_token") || strings.Contains(w.Body.String(), "access_token") {
		t.Fatal("private identity in status")
	}
}

func TestOAuthRoutes(t *testing.T) {
	cfg, _ := config.Parse(func(string) string { return "" })
	store := artwork.New(96, nil)
	p, _ := fixture.New(store, nil)
	oauth := &fakeOAuth{}
	h := New(cfg, p, store, oauth, slog.New(slog.NewTextHandler(io.Discard, nil)))
	w := request(h, "POST", "/auth/login", "", "", "")
	if w.Code != 303 {
		t.Fatal(w.Code)
	}
	if !strings.Contains(w.Header().Get("Content-Security-Policy"), "form-action 'self' https://accounts.spotify.com;") {
		t.Fatal("authorization redirect is blocked by the form policy")
	}
	cookies := w.Result().Cookies()
	if len(cookies) != 1 || !cookies[0].HttpOnly || cookies[0].SameSite != http.SameSiteLaxMode || cookies[0].MaxAge != 600 {
		t.Fatal("unsafe OAuth cookie")
	}
	w = request(h, "GET", "/auth/callback?state=test-state&code=test-code", "", "", "")
	if w.Code != 400 || oauth.calls != 0 {
		t.Fatal("callback without cookie")
	}
	if w.Header().Get("Referrer-Policy") != "no-referrer" {
		t.Fatal("callback error must retain no-referrer")
	}
	r := httptest.NewRequest("GET", "http://127.0.0.1:8787/auth/callback?state=test-state&code=test-code", nil)
	r.RemoteAddr = "127.0.0.1:40000"
	r.Header.Set("Sec-Fetch-Site", "cross-site")
	r.AddCookie(cookies[0])
	w = httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 303 || oauth.calls != 1 || w.Header().Get("Location") != "/status" {
		t.Fatal("callback routing")
	}
	if w.Header().Get("Referrer-Policy") != "no-referrer" {
		t.Fatal("callback redirect must retain no-referrer")
	}
}

func TestMaximumMetadataResponse(t *testing.T) {
	p := models.Empty("playing")
	p.TrackID = strings.Repeat("<", 128)
	p.Title = strings.Repeat("&", 256)
	p.Artist = strings.Repeat(">", 256)
	p.Album = strings.Repeat("<", 256)
	p.Context = models.Context{Type: "playlist", Name: strings.Repeat("&", 256)}
	w := httptest.NewRecorder()
	jsonReply(w, 200, p.Bounded())
	if w.Code != 200 || w.Body.Len() > 8192 {
		t.Fatalf("max metadata: %d, %d bytes", w.Code, w.Body.Len())
	}
	var got models.Playback
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil || got.Title != p.Title {
		t.Fatal("metadata altered")
	}
}

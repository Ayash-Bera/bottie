package devicebridge

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"bottie/companion/internal/api"
	"bottie/companion/internal/artwork"
	"bottie/companion/internal/config"
	"bottie/companion/internal/fixture"
	"bottie/companion/internal/models"
)

var testConfig = config.Bridge{Bind: "127.0.0.1:8788", Token: strings.Repeat("private-device-marker", 2)}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func testRequest(method, path, body string) *http.Request {
	r := httptest.NewRequest(method, path, strings.NewReader(body))
	r.Host = testConfig.Bind
	r.Header.Set("Authorization", "Bearer "+testConfig.Token)
	if method == "POST" {
		r.Header.Set("Content-Type", "application/json")
	}
	return r
}

func quietLog() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

func withUpstream(t *testing.T, handler http.Handler) http.Handler {
	t.Helper()
	s := httptest.NewServer(handler)
	t.Cleanup(s.Close)
	transport := newTransport()
	dial := transport.DialContext
	transport.DialContext = func(ctx context.Context, network, address string) (conn net.Conn, err error) {
		if address != "127.0.0.1:8787" {
			t.Errorf("unexpected upstream address: %s", address)
		}
		return dial(ctx, network, s.Listener.Addr().String())
	}
	t.Cleanup(transport.CloseIdleConnections)
	return newBridge(testConfig, quietLog(), transport)
}

func TestRejectedRequestsNeverReachUpstream(t *testing.T) {
	for _, tt := range []struct {
		name, method, path, body string
		modify                   func(*http.Request)
		status                   int
	}{
		{"missing token", "GET", "/v1/playback", "", func(r *http.Request) { r.Header.Del("Authorization") }, 401},
		{"wrong token", "GET", "/v1/playback", "", func(r *http.Request) { r.Header.Set("Authorization", "Bearer wrong") }, 401},
		{"duplicate auth", "GET", "/v1/playback", "", func(r *http.Request) { r.Header.Add("Authorization", "Bearer other") }, 401},
		{"foreign origin", "POST", "/v1/control", `{ "action":"next" }`, func(r *http.Request) {
			r.Header.Set("Origin", "https://foreign.example")
			r.Header.Set("Sec-Fetch-Site", "same-origin")
		}, 403},
		{"null origin", "POST", "/v1/control", `{ "action":"next" }`, func(r *http.Request) { r.Header.Set("Origin", "null"); r.Header.Set("Sec-Fetch-Site", "same-origin") }, 403},
		{"same origin browser", "GET", "/v1/playback", "", func(r *http.Request) { r.Header.Set("Origin", "http://"+testConfig.Bind) }, 403},
		{"empty origin header", "GET", "/v1/playback", "", func(r *http.Request) { r.Header["Origin"] = []string{""} }, 403},
		{"cross site", "GET", "/v1/playback", "", func(r *http.Request) { r.Header.Set("Sec-Fetch-Site", "cross-site") }, 403},
		{"host rebinding", "GET", "/v1/playback", "", func(r *http.Request) { r.Host = "foreign.example:8788" }, 403},
		{"home", "GET", "/", "", nil, 404},
		{"status", "GET", "/status", "", nil, 404},
		{"oauth", "POST", "/auth/login", "", nil, 404},
		{"callback", "GET", "/auth/callback", "", nil, 404},
		{"query", "GET", "/v1/playback?url=https://foreign.example", "", nil, 400},
		{"empty query", "GET", "/v1/playback?", "", nil, 400},
		{"encoded path", "GET", "/v1/artwork/%61", "", nil, 400},
		{"absolute target", "GET", "http://127.0.0.1:8788/v1/playback", "", nil, 400},
		{"dot id", "GET", "/v1/artwork/..", "", nil, 404},
		{"long id", "GET", "/v1/artwork/" + strings.Repeat("a", 129), "", nil, 404},
		{"traversal", "GET", "/v1/artwork/../control", "", nil, 404},
		{"wrong method", "POST", "/v1/playback", `{}`, nil, 405},
		{"head", "HEAD", "/v1/playback", "", nil, 405},
		{"options", "OPTIONS", "/v1/control", "", nil, 405},
		{"get body", "GET", "/v1/playback", `{}`, nil, 400},
		{"oversize control", "POST", "/v1/control", strings.Repeat(" ", 1025), nil, 413},
		{"malformed control", "POST", "/v1/control", `{`, nil, 400},
		{"content type", "POST", "/v1/control", `{}`, func(r *http.Request) { r.Header.Set("Content-Type", "text/plain") }, 415},
	} {
		t.Run(tt.name, func(t *testing.T) {
			calls := 0
			var logs bytes.Buffer
			h := newBridge(testConfig, slog.New(slog.NewTextHandler(&logs, nil)), roundTripFunc(func(*http.Request) (*http.Response, error) { calls++; return nil, io.EOF }))
			r := testRequest(tt.method, tt.path, tt.body)
			if tt.modify != nil {
				tt.modify(r)
			}
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if w.Code != tt.status || calls != 0 {
				t.Fatalf("status=%d calls=%d", w.Code, calls)
			}
			if strings.Contains(logs.String()+w.Body.String(), testConfig.Token) || strings.Contains(logs.String(), tt.path) {
				t.Fatal("sensitive request content logged or returned")
			}
			if w.Header().Get("Access-Control-Allow-Origin") != "" {
				t.Fatal("CORS enabled")
			}
		})
	}
}

func TestFixturePlaybackArtworkAndAllControls(t *testing.T) {
	store := artwork.New(96, nil)
	player, err := fixture.New(store, nil)
	if err != nil {
		t.Fatal(err)
	}
	cfg, err := config.Parse(func(string) string { return "" })
	if err != nil {
		t.Fatal(err)
	}
	apiHandler := api.New(cfg, player, store, nil, quietLog())
	h := withUpstream(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		for _, name := range []string{"Authorization", "Cookie", "Origin", "Forwarded", "X-Forwarded-For", "Idempotency-Key"} {
			if r.Header.Get(name) != "" {
				t.Errorf("caller header forwarded: %s", name)
			}
		}
		w.Header().Set("Set-Cookie", "private-upstream-cookie")
		w.Header().Set("Location", "https://foreign.example/private-marker")
		apiHandler.ServeHTTP(w, r)
	}))
	get := func(path string) *httptest.ResponseRecorder {
		r := testRequest("GET", path, "")
		r.Header.Set("Cookie", "private-caller-cookie")
		r.Header.Set("Forwarded", "private-caller-forwarded")
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != 200 {
			t.Fatalf("GET %s status=%d: %s", path, w.Code, w.Body.String())
		}
		if w.Header().Get("Set-Cookie") != "" || w.Header().Get("Location") != "" {
			t.Fatal("upstream secret header escaped")
		}
		return w
	}
	var p models.Playback
	if err := json.Unmarshal(get("/v1/playback").Body.Bytes(), &p); err != nil {
		t.Fatal(err)
	}
	if p.State != "paused" || p.Title != "Orbital Sunrise" {
		t.Fatalf("unexpected fixture state: %s %s", p.State, p.Title)
	}
	if data := get(p.Artwork.URL).Body.Bytes(); len(data) != maxArtwork || !validArtwork(data) {
		t.Fatal("fixture artwork rejected")
	}
	for _, body := range []string{`{"action":"play"}`, `{"action":"pause"}`, `{"action":"next"}`, `{"action":"previous"}`, `{"action":"shuffle","value":true}`, `{"action":"repeat","value":"context"}`} {
		r := testRequest("POST", "/v1/control", body)
		r.Header.Set("Idempotency-Key", "private-replay-marker")
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != 200 || w.Body.String() != `{"ok":true}` {
			t.Fatalf("fixture control %s: %d %s", body, w.Code, w.Body.String())
		}
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("POST", "/v1/control", `{"action":"next","action":"play"}`))
	if w.Code != 400 {
		t.Fatal("upstream duplicate control validation lost")
	}
}

func TestUpstreamResponseBoundaries(t *testing.T) {
	validJSON, _ := json.Marshal(models.Empty("idle"))
	art := make([]byte, maxArtwork)
	copy(art, "BTAR\x01\x01")
	binary.LittleEndian.PutUint16(art[6:], 96)
	binary.LittleEndian.PutUint16(art[8:], 96)
	binary.LittleEndian.PutUint16(art[10:], 192)
	binary.LittleEndian.PutUint32(art[12:], 96*96*2)
	oversizeDimensions := bytes.Clone(art)
	binary.LittleEndian.PutUint16(oversizeDimensions[6:], 97)
	badStride := bytes.Clone(art)
	binary.LittleEndian.PutUint16(badStride[10:], 191)
	badFormat := bytes.Clone(art)
	badFormat[5] = 2
	for _, tt := range []struct {
		name, path, media string
		body              []byte
		status, want      int
		chunked           bool
	}{
		{"json", "/v1/playback", "application/json", validJSON, 200, 200, false},
		{"json chunked", "/v1/playback", "application/json", validJSON, 200, 200, true},
		{"json oversized", "/v1/playback", "application/json", bytes.Repeat([]byte(" "), maxJSON+1), 200, 502, false},
		{"chunked oversized", "/v1/playback", "application/json", bytes.Repeat([]byte(" "), maxJSON+1), 200, 502, true},
		{"json trailing", "/v1/playback", "application/json", append(bytes.Clone(validJSON), []byte(`{}`)...), 200, 502, false},
		{"unknown fields", "/v1/playback", "application/json", []byte(`{"schema_version":1,"access_token":"private-marker"}`), 200, 502, false},
		{"unknown schema", "/v1/playback", "application/json", []byte(`{"schema_version":2}`), 200, 502, false},
		{"foreign art", "/v1/playback", "application/json", []byte(`{"schema_version":1,"artwork":{"url":"https://foreign.example/cover","width":96,"height":96}}`), 200, 502, false},
		{"wrong media", "/v1/playback", "text/html", validJSON, 200, 502, false},
		{"art as json", "/v1/playback", "application/json", art, 200, 502, false},
		{"art", "/v1/artwork/test", "application/octet-stream", art, 200, 200, false},
		{"art oversized", "/v1/artwork/test", "application/octet-stream", append(bytes.Clone(art), 0), 200, 502, true},
		{"art truncated", "/v1/artwork/test", "application/octet-stream", art[:len(art)-1], 200, 502, false},
		{"art header", "/v1/artwork/test", "application/octet-stream", badFormat, 200, 502, false},
		{"art dimensions", "/v1/artwork/test", "application/octet-stream", oversizeDimensions, 200, 502, false},
		{"art stride", "/v1/artwork/test", "application/octet-stream", badStride, 200, 502, false},
		{"upstream secret", "/v1/playback", "application/json", []byte(`{"error":"private-marker"}`), 503, 503, false},
		{"rate limited", "/v1/playback", "application/json", []byte(`{"error":"private-marker"}`), 429, 429, false},
		{"redirect", "/v1/playback", "application/json", []byte(`{}`), 302, 502, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			var calls atomic.Int32
			h := withUpstream(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				w.Header().Set("Content-Type", tt.media)
				w.Header().Set("Location", upstream+"/v1/playback")
				w.Header().Set("Set-Cookie", "private-marker")
				w.Header().Set("Retry-After", "9999")
				w.Header().Set("X-Bottie-Status", "private-marker")
				w.WriteHeader(tt.status)
				if tt.chunked {
					w.(http.Flusher).Flush()
				}
				_, _ = w.Write(tt.body)
			}))
			w := httptest.NewRecorder()
			h.ServeHTTP(w, testRequest("GET", tt.path, ""))
			if w.Code != tt.want || calls.Load() != 1 {
				t.Fatalf("status=%d calls=%d", w.Code, calls.Load())
			}
			if len(w.Body.Bytes()) > maxArtwork || strings.Contains(w.Body.String(), "private-marker") || strings.Contains(w.Header().Get("X-Bottie-Status"), "private-marker") || w.Header().Get("Set-Cookie") != "" || w.Header().Get("Location") != "" {
				t.Fatal("upstream data escaped bounds")
			}
			if w.Header().Get("Retry-After") != "300" {
				t.Fatal("retry delay not bounded")
			}
		})
	}
}

func TestAmbiguousControlIsNeverReplayed(t *testing.T) {
	var calls atomic.Int32
	h := withUpstream(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.Copy(io.Discard, r.Body)
		calls.Add(1)
		conn, _, err := w.(http.Hijacker).Hijack()
		if err != nil {
			t.Error(err)
			return
		}
		_ = conn.Close()
	}))
	w := httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("POST", "/v1/control", `{"action":"next"}`))
	if w.Code != 502 || calls.Load() != 1 {
		t.Fatalf("status=%d calls=%d", w.Code, calls.Load())
	}
}

func TestConcurrencyAndCancellation(t *testing.T) {
	started := make(chan struct{}, 8)
	h := newBridge(testConfig, quietLog(), roundTripFunc(func(r *http.Request) (*http.Response, error) {
		started <- struct{}{}
		<-r.Context().Done()
		return nil, r.Context().Err()
	}))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var workers sync.WaitGroup
	for range 8 {
		workers.Go(func() { h.ServeHTTP(httptest.NewRecorder(), testRequest("GET", "/v1/playback", "").WithContext(ctx)) })
	}
	for range 8 {
		<-started
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
	if w.Code != 429 || w.Header().Get("Retry-After") != "1" {
		t.Fatal("concurrency bound not enforced")
	}
	cancel()
	workers.Wait()
}

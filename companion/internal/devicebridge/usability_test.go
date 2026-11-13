package devicebridge

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"bottie/companion/internal/models"
	"bottie/companion/internal/textmask"
)

func TestSnapshotBudgetMatchesDeviceTokens(t *testing.T) {
	for _, count := range []int{254, 255, 256, 257} {
		data := []byte("[" + strings.Repeat("0,", count-1) + "0]")
		if snapshotBudget(data) != (count+1 <= 256) {
			t.Fatalf("array tokens%d", count+1)
		}
	}
	if snapshotBudget([]byte(`{"x":"`+strings.Repeat("x", 8192)+`"}`)) || snapshotBudget([]byte("{")) {
		t.Fatal("byte/syntax limit")
	}
	p := models.Playback{State: "playing", TrackID: strings.Repeat("<", 128), Title: strings.Repeat("<", 256), Artist: strings.Repeat("<", 256), Album: strings.Repeat("<", 256), Context: models.Context{Type: "playlist", Name: strings.Repeat("<", 256)}, ControlTarget: strings.Repeat("a", 32), ControlTrack: strings.Repeat("b", 32), DurationMS: 60000, Details: &models.Details{Layout: 1, ID: strings.Repeat("c", 64)}}
	p.DurationMS, p.ProgressMS, p.UpdatedAtMS = models.MaxDuration, models.MaxDuration, models.MaxTimestamp
	p.Artwork = models.Artwork{URL: "/v1/artwork/" + strings.Repeat("a", 128), Width: 96, Height: 96}
	p.TextMask = &models.TextMask{URL: "/v1/text/" + strings.Repeat("f", 64), Layout: 1}
	p.Clock = &models.Clock{UnixMS: 4102444799999, UTCOffsetMin: -840}
	p.Capabilities = models.Capabilities{Play: true, Pause: true, Previous: true, Next: true, Shuffle: true, Repeat: true, Volume: true, Seek: true}
	volume := 100
	p.VolumePercent = &volume
	for field := range 4 {
		p.Details.SetField(field, &models.DetailField{Pages: []string{strings.Repeat("d", 64), strings.Repeat("e", 64), strings.Repeat("f", 64), strings.Repeat("0", 64)}, Flags: 3})
	}
	data, _ := json.Marshal(p)
	if snapshotBudget(data) {
		t.Fatal("oversize negative control failed")
	}
	data, err := encodeSnapshot(p.Bounded())
	var got models.Playback
	if err != nil || !snapshotBudget(data) || json.Unmarshal(data, &got) != nil || got.Details != nil || got.Title != p.Title || got.TrackID != p.TrackID {
		t.Fatal("descriptor omission changed base fields or violated budget")
	}
}

func TestBridgeFeedbackReasonsAndPrivacy(t *testing.T) {
	for _, code := range []string{"track_changed", "target_changed", "unsupported", "rate_limited", "keyring_locked", "private-secret-code"} {
		h := newBridge(testConfig, quietLog(), roundTripFunc(func(*http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: 409, Header: http.Header{"Content-Type": {"application/json"}}, Body: io.NopCloser(strings.NewReader(`{"error":{"code":"` + code + `","message":"private-secret-message"}}`))}, nil
		}))
		w := httptest.NewRecorder()
		h.ServeHTTP(w, testRequest("POST", "/v1/control", `{"action":"pause"}`))
		if w.Code != 409 || strings.Contains(w.Body.String(), "private-secret") {
			t.Fatal("upstream private error leaked")
		}
		want := code
		if code == "private-secret-code" {
			want = "upstream_error"
		}
		if !strings.Contains(w.Body.String(), `"code":"`+want+`"`) {
			t.Fatal("feedback code lost")
		}
	}
	for _, reason := range []string{`"wallet_locked"`, `"future_reason"`, `{}`, `null`} {
		h := newBridge(testConfig, quietLog(), roundTripFunc(func(*http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {"application/json"}}, Body: io.NopCloser(strings.NewReader(`{"schema_version":1,"state":"paused","title":"kept","offline_reason":` + reason + `,"details":{"layout":1,"id":"bad"}}`))}, nil
		}))
		w := httptest.NewRecorder()
		h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
		var p models.Playback
		if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &p) != nil || p.Title != "kept" || p.Details != nil {
			t.Fatal("optional failure dropped snapshot")
		}
	}
}

type heldWriter struct {
	http.ResponseWriter
	entered chan<- struct{}
	release <-chan struct{}
}

func (w heldWriter) Write(p []byte) (int, error) {
	w.entered <- struct{}{}
	<-w.release
	return len(p), nil
}

func TestDetailGatewayAndSlowReaderBound(t *testing.T) {
	store, err := textmask.NewDetails(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	p := models.Playback{SchemaVersion: 1, State: "playing", TrackID: "detail", Title: "ক্ষিতিজ ORBIT", Artist: "夜に駆ける", Album: "চাঁদের আলো", Context: models.Context{Type: "album", Name: "চাঁদের আলো"}}
	data, _ := json.Marshal(p)
	transport := roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method != "GET" || r.URL.Path != "/v1/playback" {
			t.Error("detail forwarded upstream")
		}
		return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {"application/json"}}, Body: io.NopCloser(bytes.NewReader(data))}, nil
	})
	h := bridgeWithResources(testConfig, quietLog(), transport, nil, store, time.Now)
	deadline := time.Now().Add(2 * time.Second)
	var got models.Playback
	for time.Now().Before(deadline) {
		w := httptest.NewRecorder()
		h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
		if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &got) != nil {
			t.Fatal("playback failed")
		}
		if got.Details != nil {
			break
		}
		time.Sleep(time.Millisecond)
	}
	if got.Details == nil || got.Details.Context != nil || got.Details.Album == nil || got.Title != p.Title {
		t.Fatal("detail roles/identity")
	}
	path := "/v1/detail/" + got.Details.Title.Pages[0]
	w := httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", path, ""))
	if w.Code != 200 || len(w.Body.Bytes()) != textmask.DetailMaxBytes || fmt.Sprintf("/v1/detail/%x", sha256.Sum256(w.Body.Bytes())) != path {
		t.Fatal("detail body/digest")
	}
	for _, tc := range []struct {
		method, path, origin string
		auth                 bool
		code                 int
	}{{"GET", path, "", false, 401}, {"GET", path, "null", true, 403}, {"GET", path, "https://foreign.invalid", true, 403}, {"GET", path + "?text=x", "", true, 400}, {"POST", path, "", true, 405}, {"GET", "/v1/detail/" + strings.Repeat("A", 64), "", true, 404}} {
		r := testRequest(tc.method, tc.path, "")
		if !tc.auth {
			r.Header.Del("Authorization")
		}
		if tc.origin != "" {
			r.Header.Set("Origin", tc.origin)
		}
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != tc.code {
			t.Fatalf("detail route status%d want%d", w.Code, tc.code)
		}
	}
	entered := make(chan struct{}, 8)
	release := make(chan struct{})
	var workers sync.WaitGroup
	for range 8 {
		workers.Go(func() {
			h.ServeHTTP(heldWriter{httptest.NewRecorder(), entered, release}, testRequest("GET", path, ""))
		})
	}
	defer func() { close(release); workers.Wait() }()
	for range 8 {
		select {
		case <-entered:
		case <-time.After(time.Second):
			t.Fatal("slow reader did not reach body")
		}
	}
	w = httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", path, ""))
	if w.Code != 429 {
		t.Fatalf("ninth response allocated: %d", w.Code)
	}
	for i := range 5 {
		p.Title = fmt.Sprintf("replacement %d", i)
		deadline := time.Now().Add(time.Second)
		for store.Observe(p) == nil {
			if time.Now().After(deadline) {
				t.Fatal("eviction stalled behind readers")
			}
			time.Sleep(time.Millisecond)
		}
	}
	if _, ok := store.Get(path); ok {
		t.Fatal("old cache entry survived bounded eviction")
	}
}

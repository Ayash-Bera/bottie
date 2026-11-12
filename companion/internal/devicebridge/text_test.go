//go:build linux && cgo

package devicebridge

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"bottie/companion/internal/models"
	"bottie/companion/internal/textmask"
)

func TestTextGatewayPreservesMetadataAndRestrictsAssetAccess(t *testing.T) {
	masks, err := textmask.New()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(masks.Close)
	p := models.Empty("playing")
	p.TrackID, p.Title, p.Artist, p.Album = "test", "ক্ষিতিজ ORBIT", "夜に駆ける", "চাঁদের আলো"
	p.Context = models.Context{Type: "playlist", Name: "カタカナ・ひらがな"}
	p.TextMask = &models.TextMask{URL: "https://foreign.invalid/text", Layout: 99}
	transport := roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path != "/v1/playback" {
			t.Error("text request forwarded upstream")
		}
		data, err := json.Marshal(p)
		return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": []string{"application/json"}}, Body: io.NopCloser(bytes.NewReader(data))}, err
	})
	h := bridgeWithMasks(testConfig, quietLog(), transport, masks)
	w := httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
	var got models.Playback
	if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &got) != nil || got.TextMask == nil {
		t.Fatalf("playback %d", w.Code)
	}
	if got.Title != p.Title || got.Artist != p.Artist || got.Album != p.Album || got.Context != p.Context {
		t.Fatal("metadata changed")
	}
	path := got.TextMask.URL
	w = httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", path, ""))
	if w.Code != 200 || w.Header().Get("Content-Type") != "application/octet-stream" || len(w.Body.Bytes()) > textmask.MaxBytes || path != fmt.Sprintf("/v1/text/%x", sha256.Sum256(w.Body.Bytes())) {
		t.Fatal("bundle digest/limit")
	}
	before := append([]byte(nil), w.Body.Bytes()...)
	for _, tt := range []struct {
		path, method, body, origin string
		token                      bool
		want                       int
	}{
		{path, "GET", "", "", false, 401}, {path, "GET", "", "null", true, 403}, {path, "GET", "", "https://foreign.invalid", true, 403},
		{path, "POST", "{}", "", true, 405}, {path, "HEAD", "", "", true, 405}, {path, "GET", "{}", "", true, 400},
		{path + "?text=test", "GET", "", "", true, 400}, {"/v1/text/" + strings.Repeat("A", 64), "GET", "", "", true, 404},
		{"/v1/text/" + strings.Repeat("0", 64), "GET", "", "", true, 404}, {"/v1/text/../control", "GET", "", "", true, 404},
	} {
		r := testRequest(tt.method, tt.path, tt.body)
		if !tt.token {
			r.Header.Del("Authorization")
		}
		if tt.origin != "" {
			r.Header.Set("Origin", tt.origin)
			r.Header.Set("Sec-Fetch-Site", "same-origin")
		}
		w = httptest.NewRecorder()
		h.ServeHTTP(w, r)
		if w.Code != tt.want {
			t.Fatalf("text request %s %s: %d", tt.method, tt.path, w.Code)
		}
	}
	p.Title = "শ্রদ্ধা"
	w = httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
	if json.Unmarshal(w.Body.Bytes(), &got) != nil || got.TextMask == nil || got.TextMask.URL == path {
		t.Fatal("changed text reused previous descriptor")
	}
	w = httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", got.TextMask.URL, ""))
	if bytes.Equal(w.Body.Bytes(), before) {
		t.Fatal("changed text reused previous mask")
	}
	p.Title, p.Artist, p.Album, p.Context.Name = "TITLE", "ARTIST", "ALBUM", "PLAYLIST"
	w = httptest.NewRecorder()
	h.ServeHTTP(w, testRequest("GET", "/v1/playback", ""))
	if json.Unmarshal(w.Body.Bytes(), &got) != nil || bytes.Contains(w.Body.Bytes(), []byte(`"text_mask"`)) {
		t.Fatal("ASCII snapshot advertised text")
	}
}

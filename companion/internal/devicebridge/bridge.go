package devicebridge

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
	"encoding/json"
	"io"
	"log/slog"
	"mime"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"bottie/companion/internal/config"
	"bottie/companion/internal/models"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
)

const upstream = "http://127.0.0.1:8787"
const maxJSON = 8192
const maxArtwork = 16 + 96*96*2

type bridge struct {
	cfg    config.Bridge
	client *http.Client
	log    *slog.Logger
	slots  chan struct{}
}

func New(cfg config.Bridge, log *slog.Logger) http.Handler {
	return newBridge(cfg, log, newTransport())
}

func newTransport() *http.Transport {
	return &http.Transport{
		DialContext:            (&net.Dialer{Timeout: 2 * time.Second}).DialContext,
		DisableKeepAlives:      true, // Fresh connections prevent transport replay after an ambiguous control result.
		DisableCompression:     true,
		MaxResponseHeaderBytes: 16 << 10,
		ResponseHeaderTimeout:  5 * time.Second,
	}
}

func newBridge(cfg config.Bridge, log *slog.Logger, transport http.RoundTripper) http.Handler {
	b := &bridge{cfg: cfg, log: log, slots: make(chan struct{}, 8), client: &http.Client{
		Transport: transport,
		Timeout:   8 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}}
	r := chi.NewRouter()
	r.Use(b.recover, b.logger, b.origin, b.auth, b.limit)
	r.Get("/v1/playback", b.forward)
	r.Post("/v1/control", b.forward)
	r.Get("/v1/artwork/{id}", b.forward)
	r.NotFound(func(w http.ResponseWriter, r *http.Request) {
		problem(w, 404, "not_found", "device endpoint not found")
	})
	r.MethodNotAllowed(func(w http.ResponseWriter, r *http.Request) {
		problem(w, 405, "method_not_allowed", "method is not supported")
	})
	return r
}

func problem(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{"code": code, "message": message}})
}

func (b *bridge) recover(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if recover() != nil {
				b.log.Error("device request panic")
				problem(w, 500, "internal_error", "internal device request error")
			}
		}()
		next.ServeHTTP(w, r)
	})
}

func (b *bridge) logger(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		ww := middleware.NewWrapResponseWriter(w, r.ProtoMajor)
		defer func() {
			b.log.Info("device request", "status", ww.Status(), "duration_ms", time.Since(start).Milliseconds())
		}()
		next.ServeHTTP(ww, r)
	})
}

func (b *bridge) origin(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'; base-uri 'none'")
		if r.Host != b.cfg.Bind || len(r.Header.Values("Origin")) != 0 || len(r.Header.Values("Sec-Fetch-Site")) != 0 {
			problem(w, 403, "forbidden", "use the configured device endpoint without browser origin headers")
			return
		}
		if len(r.RequestURI) > 256 || r.URL.RawQuery != "" || r.URL.ForceQuery || r.URL.RawPath != "" || r.URL.IsAbs() || r.RequestURI != r.URL.Path {
			problem(w, 400, "invalid_path", "use a canonical device path without query parameters")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (b *bridge) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		expected := sha256.Sum256([]byte("Bearer " + b.cfg.Token))
		got := sha256.Sum256([]byte(r.Header.Get("Authorization")))
		if b.cfg.Token == "" || len(r.Header.Values("Authorization")) != 1 || subtle.ConstantTimeCompare(expected[:], got[:]) != 1 {
			w.Header().Set("WWW-Authenticate", "Bearer")
			problem(w, 401, "unauthorized", "a valid device bearer token is required")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (b *bridge) limit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case b.slots <- struct{}{}:
			defer func() { <-b.slots }()
		default:
			w.Header().Set("Retry-After", "1")
			problem(w, 429, "rate_limited", "device gateway is busy")
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 8*time.Second)
		defer cancel()
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func artworkPath(path string) bool {
	id, ok := strings.CutPrefix(path, "/v1/artwork/")
	return ok && len(id) > 0 && len(id) <= 128 && strings.IndexFunc(id, func(r rune) bool {
		return !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '_' || r == '-')
	}) < 0
}

func validArtwork(data []byte) bool {
	if len(data) < 16 || string(data[:6]) != "BTAR\x01\x01" {
		return false
	}
	w, h := int(binary.LittleEndian.Uint16(data[6:8])), int(binary.LittleEndian.Uint16(data[8:10]))
	stride, size := int(binary.LittleEndian.Uint16(data[10:12])), int(binary.LittleEndian.Uint32(data[12:16]))
	return w >= 16 && w <= 96 && h >= 16 && h <= 96 && stride == w*2 && size == w*h*2 && len(data) == 16+size
}

func decode(data []byte, v any) error {
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	return d.Decode(v)
}

func (b *bridge) forward(w http.ResponseWriter, r *http.Request) {
	art := strings.HasPrefix(r.URL.Path, "/v1/artwork/")
	if art && !artworkPath(r.URL.Path) {
		problem(w, 404, "not_found", "artwork path is invalid")
		return
	}
	var body []byte
	if r.Method == http.MethodPost {
		media, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
		if err != nil || media != "application/json" {
			problem(w, 415, "content_type", "send application/json")
			return
		}
		body, err = io.ReadAll(http.MaxBytesReader(w, r.Body, 1024))
		if err != nil {
			problem(w, 413, "too_large", "control body must not exceed 1024 bytes")
			return
		}
		if !json.Valid(body) {
			problem(w, 400, "invalid_json", "send one JSON control object")
			return
		}
	} else if r.ContentLength != 0 || len(r.TransferEncoding) != 0 {
		problem(w, 400, "invalid_body", "GET requests must have no body")
		return
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, upstream+r.URL.Path, bytes.NewReader(body))
	if err != nil {
		problem(w, 502, "upstream_error", "cannot prepare companion request")
		return
	}
	req.GetBody = nil
	req.Header.Set("Accept", "application/json")
	if art {
		req.Header.Set("Accept", "application/octet-stream")
	}
	if r.Method == http.MethodPost {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := b.client.Do(req)
	if err != nil {
		problem(w, 502, "upstream_error", "loopback companion request failed; a control may already have been accepted")
		return
	}
	defer resp.Body.Close()
	if seconds, err := strconv.Atoi(resp.Header.Get("Retry-After")); err == nil && seconds > 0 {
		w.Header().Set("Retry-After", strconv.Itoa(min(seconds, 300)))
	}
	if resp.StatusCode != 200 {
		status := resp.StatusCode
		if status < 400 || status > 599 {
			status = 502
		}
		problem(w, status, "upstream_error", "companion declined the device request; check its local status page")
		return
	}
	limit, media := maxJSON, "application/json"
	if art {
		limit, media = maxArtwork, "application/octet-stream"
	}
	actual, _, err := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if err != nil || actual != media || resp.ContentLength > int64(limit) || resp.Header.Get("Content-Encoding") != "" {
		problem(w, 502, "invalid_response", "companion response has invalid type or size")
		return
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, int64(limit+1)))
	if err != nil || len(data) > limit {
		problem(w, 502, "invalid_response", "companion response is incomplete or exceeds the size limit")
		return
	}
	valid := false
	if art {
		valid = validArtwork(data)
	} else if json.Valid(data) {
		if r.Method == http.MethodGet {
			var p models.Playback
			valid = decode(data, &p) == nil && p.SchemaVersion == 1
			if p.VolumePercent != nil && (*p.VolumePercent < 0 || *p.VolumePercent > 100) || p.ControlTarget != "" && !models.ValidControlTarget(p.ControlTarget) || p.Capabilities.Volume && p.ControlTarget == "" {
				valid = false
			}
			if p.Artwork.URL != "" && (!artworkPath(p.Artwork.URL) || p.Artwork.Width > 96 || p.Artwork.Height > 96) {
				valid = false
			}
			data, err = json.Marshal(p.Bounded())
		} else {
			var result struct {
				OK bool `json:"ok"`
			}
			valid = decode(data, &result) == nil && result.OK
			data, err = json.Marshal(result)
		}
	}
	if !valid || err != nil || len(data) > limit {
		problem(w, 502, "invalid_response", "companion response does not match protocol v1 or BTAR up to 96 pixels")
		return
	}
	switch status := resp.Header.Get("X-Bottie-Status"); status {
	case "ok", "not_authorized", "restricted", "no_device", "rate_limited", "upstream_error":
		w.Header().Set("X-Bottie-Status", status)
	}
	w.Header().Set("Content-Type", media)
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	w.WriteHeader(200)
	_, _ = w.Write(data)
}

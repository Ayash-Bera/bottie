package api

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"mime"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"

	"bottie/companion/internal/artwork"
	"bottie/companion/internal/config"
	"bottie/companion/internal/models"
	"bottie/companion/internal/spotify"
)

type OAuth interface {
	Begin(context.Context) (string, string, *models.ServiceError)
	Complete(context.Context, string, string, string, string) *models.ServiceError
}

type server struct {
	cfg    config.Config
	player models.Player
	art    *artwork.Store
	oauth  OAuth
	log    *slog.Logger
	slots  chan struct{}
}

func New(cfg config.Config, player models.Player, art *artwork.Store, oauth OAuth, log *slog.Logger) http.Handler {
	s := &server{cfg: cfg, player: player, art: art, oauth: oauth, log: log, slots: make(chan struct{}, 32)}
	r := chi.NewRouter()
	r.Use(s.recover, s.logger, s.cors)
	r.Group(func(r chi.Router) {
		r.Use(s.localOnly, s.limit)
		r.Get("/", s.home)
		r.Get("/status", s.status)
		r.Post("/auth/login", s.login)
		r.Get("/auth/callback", s.callback)
	})
	r.Route("/v1", func(r chi.Router) {
		r.Use(s.auth, s.limit)
		r.Get("/playback", s.playback)
		r.Post("/control", s.control)
		r.Get("/artwork/{id}", s.artwork)
	})
	r.NotFound(func(w http.ResponseWriter, r *http.Request) {
		problem(w, models.Problem("not_found", 404, "endpoint not found"))
	})
	return r
}

func jsonReply(w http.ResponseWriter, status int, v any) {
	var body bytes.Buffer
	enc := json.NewEncoder(&body)
	enc.SetEscapeHTML(false)
	err := enc.Encode(v)
	if err != nil || body.Len() > 8192 {
		http.Error(w, "response exceeds limit", 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(body.Bytes())
}
func problem(w http.ResponseWriter, e *models.ServiceError) {
	if e.RetryAfter > 0 {
		w.Header().Set("Retry-After", strconv.Itoa(e.RetryAfter))
	}
	jsonReply(w, e.Status, map[string]any{"error": map[string]string{"code": e.Code, "message": e.Message}})
}
func (s *server) recover(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if recover() != nil {
				s.log.Error("request panic")
				problem(w, models.Problem("internal_error", 500, "internal request error"))
			}
		}()
		next.ServeHTTP(w, r)
	})
}
func (s *server) logger(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		ww := middleware.NewWrapResponseWriter(w, r.ProtoMajor)
		defer func() { s.log.Info("request", "status", ww.Status(), "duration_ms", time.Since(start).Milliseconds()) }()
		next.ServeHTTP(ww, r)
	})
}
func (s *server) cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; form-action 'self' https://accounts.spotify.com; frame-ancestors 'none'; base-uri 'none'")
		if len(r.RequestURI) > 8192 {
			problem(w, models.Problem("too_large", 414, "request URL exceeds limit"))
			return
		}
		hostOK := s.cfg.AllowedHosts[r.Host]
		if !hostOK {
			bindHost, bindPort, _ := net.SplitHostPort(s.cfg.Bind)
			h, p, err := net.SplitHostPort(r.Host)
			hostOK = err == nil && net.ParseIP(bindHost).IsUnspecified() && net.ParseIP(h) != nil && p == bindPort
		}
		if !hostOK {
			problem(w, models.Problem("forbidden", 403, "use the configured companion IP address"))
			return
		}
		if origin := r.Header.Get("Origin"); origin != "" && origin != "http://"+r.Host {
			problem(w, models.Problem("forbidden", 403, "cross-origin requests are disabled"))
			return
		}
		if r.Header.Get("Sec-Fetch-Site") == "cross-site" && r.URL.Path != "/auth/callback" {
			problem(w, models.Problem("forbidden", 403, "cross-site requests are disabled"))
			return
		}
		next.ServeHTTP(w, r)
	})
}
func (s *server) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.cfg.DeviceToken != "" {
			expected := sha256.Sum256([]byte("Bearer " + s.cfg.DeviceToken))
			got := sha256.Sum256([]byte(r.Header.Get("Authorization")))
			if subtle.ConstantTimeCompare(expected[:], got[:]) != 1 {
				w.Header().Set("WWW-Authenticate", "Bearer")
				problem(w, models.Problem("unauthorized", 401, "a valid device bearer token is required"))
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}
func (s *server) localOnly(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h, _, err := net.SplitHostPort(r.RemoteAddr)
		host, _, hostErr := net.SplitHostPort(r.Host)
		if err != nil || hostErr != nil || !net.ParseIP(h).IsLoopback() || !net.ParseIP(host).IsLoopback() {
			problem(w, models.Problem("forbidden", 403, "open this page on the companion computer using its loopback address"))
			return
		}
		next.ServeHTTP(w, r)
	})
}
func (s *server) limit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case s.slots <- struct{}{}:
			defer func() { <-s.slots }()
		default:
			problem(w, &models.ServiceError{Code: "rate_limited", Status: 429, Message: "companion is busy", RetryAfter: 1})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
		defer cancel()
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func (s *server) playback(w http.ResponseWriter, r *http.Request) {
	p, e := s.player.Snapshot()
	w.Header().Set("X-Bottie-Status", "ok")
	if e != nil {
		w.Header().Set("X-Bottie-Status", e.Code)
		if e.RetryAfter > 0 {
			w.Header().Set("Retry-After", strconv.Itoa(e.RetryAfter))
		}
	}
	jsonReply(w, 200, p.Bounded())
}

func decodeControl(body io.Reader) (models.Control, error) {
	d := json.NewDecoder(body)
	t, err := d.Token()
	if err != nil || t != json.Delim('{') {
		return models.Control{}, errors.New("expected object")
	}
	seen := map[string]bool{}
	var c models.Control
	for d.More() {
		key, err := d.Token()
		if err != nil {
			return c, err
		}
		k, ok := key.(string)
		if !ok || seen[k] {
			return c, errors.New("duplicate key")
		}
		seen[k] = true
		switch k {
		case "action":
			err = d.Decode(&c.Action)
		case "value":
			err = d.Decode(&c.Value)
		default:
			return c, errors.New("unknown key")
		}
		if err != nil {
			return c, err
		}
	}
	if _, err = d.Token(); err != nil {
		return c, err
	}
	if _, err = d.Token(); err != io.EOF {
		return c, errors.New("trailing data")
	}
	return c, nil
}
func (s *server) control(w http.ResponseWriter, r *http.Request) {
	t, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || t != "application/json" {
		problem(w, models.Problem("content_type", 415, "send application/json"))
		return
	}
	b, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1024))
	if err != nil {
		var large *http.MaxBytesError
		if errors.As(err, &large) {
			problem(w, models.Problem("too_large", 413, "control body exceeds 1024 bytes"))
		} else {
			problem(w, models.Problem("invalid_json", 400, "cannot read control body"))
		}
		return
	}
	c, err := decodeControl(strings.NewReader(string(b)))
	if err != nil {
		problem(w, models.Problem("invalid_json", 400, "send one control object with no duplicate or unknown fields"))
		return
	}
	if err = c.Validate(); err != nil {
		problem(w, models.Problem("invalid_control", 400, "use play, pause, previous, next, shuffle with boolean value, or repeat with off/context/track"))
		return
	}
	if e := s.player.Control(r.Context(), c); e != nil {
		problem(w, e)
		return
	}
	jsonReply(w, 200, map[string]bool{"ok": true})
}
func (s *server) artwork(w http.ResponseWriter, r *http.Request) {
	id := chi.URLParam(r, "id")
	if len(id) > 128 {
		problem(w, models.Problem("not_found", 404, "artwork is unavailable"))
		return
	}
	b, ok := s.art.Get(id)
	if !ok {
		problem(w, models.Problem("not_found", 404, "artwork expired; fetch playback again"))
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(len(b)))
	_, _ = w.Write(b)
}
func (s *server) home(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Referrer-Policy", "same-origin")
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = io.WriteString(w, "<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width'><title>Bottie companion</title><h1>Bottie companion</h1><p><a href=/status>Connection status</a></p>")
	if s.oauth == nil {
		_, _ = io.WriteString(w, "<p>Fixture mode is ready. No account is needed. Device endpoint: /v1/playback.</p>")
	} else {
		_, _ = io.WriteString(w, "<p>Start playback on your Spotify device, then connect here.</p>")
		if s.cfg.TokenStore == "keyring" {
			_, _ = io.WriteString(w, "<p>Spotify login is saved in your protected desktop wallet. Unlock that wallet after desktop login if requested. Check connection status before reconnecting.</p>")
		} else {
			_, _ = io.WriteString(w, "<p>Restarting this service clears the connection.</p>")
		}
		_, _ = io.WriteString(w, "<form method=post action=/auth/login><button>Connect Spotify</button></form>")
	}
	_, _ = io.WriteString(w, "</html>")
}
func (s *server) status(w http.ResponseWriter, r *http.Request) {
	p, e := s.player.Snapshot()
	code, message := "ok", "playback service is ready"
	if e != nil {
		code, message = e.Code, e.Message
	}
	body := map[string]any{"mode": s.cfg.Mode, "state": p.State, "status": code, "message": message}
	if source, ok := s.oauth.(interface {
		AuthorizationStatus() spotify.AuthorizationStatus
	}); ok {
		body["authorization"] = source.AuthorizationStatus()
	}
	jsonReply(w, 200, body)
}
func (s *server) login(w http.ResponseWriter, r *http.Request) {
	if s.oauth == nil {
		problem(w, models.Problem("unsupported", 409, "fixture mode does not need an account"))
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	target, state, e := s.oauth.Begin(ctx)
	if e != nil {
		problem(w, e)
		return
	}
	http.SetCookie(w, &http.Cookie{Name: "bottie_oauth", Value: state, Path: "/auth/callback", HttpOnly: true, SameSite: http.SameSiteLaxMode, MaxAge: 600})
	http.Redirect(w, r, target, http.StatusSeeOther)
}
func (s *server) callback(w http.ResponseWriter, r *http.Request) {
	if s.oauth == nil {
		problem(w, models.Problem("unsupported", 409, "fixture mode does not need an account"))
		return
	}
	q := r.URL.Query()
	for _, k := range []string{"state", "code", "error"} {
		if len(q[k]) > 1 {
			problem(w, models.Problem("invalid_state", 400, "duplicate callback parameters"))
			return
		}
	}
	c, err := r.Cookie("bottie_oauth")
	if err != nil {
		problem(w, models.Problem("invalid_state", 400, "authorization cookie is missing; start connecting again"))
		return
	}
	http.SetCookie(w, &http.Cookie{Name: "bottie_oauth", Value: "", Path: "/auth/callback", HttpOnly: true, SameSite: http.SameSiteLaxMode, MaxAge: -1})
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	if e := s.oauth.Complete(ctx, q.Get("state"), c.Value, q.Get("code"), q.Get("error")); e != nil {
		problem(w, e)
		return
	}
	http.Redirect(w, r, "/status", http.StatusSeeOther)
}

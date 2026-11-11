package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"bottie/companion/internal/models"
)

type harness struct {
	ctx                                    context.Context
	client                                 *http.Client
	base, token, native, fixtures, scratch string
}

func (h harness) request(method, path string, body []byte) ([]byte, int, error) {
	r, err := http.NewRequestWithContext(h.ctx, method, h.base+path, bytes.NewReader(body))
	if err != nil {
		return nil, 0, err
	}
	r.Header.Set("Authorization", "Bearer "+h.token)
	r.Header.Set("Content-Type", "application/json")
	res, err := h.client.Do(r)
	if err != nil {
		return nil, 0, err
	}
	defer res.Body.Close()
	b, err := io.ReadAll(io.LimitReader(res.Body, 115217))
	if len(b) > 115216 {
		return nil, res.StatusCode, errors.New("response too large")
	}
	return b, res.StatusCode, err
}
func (h harness) snapshot() (models.Playback, []byte, error) {
	b, status, err := h.request("GET", "/v1/playback", nil)
	var p models.Playback
	if err != nil {
		return p, nil, err
	}
	if status != 200 || len(b) > 8192 {
		return p, nil, fmt.Errorf("playback status %d or size limit", status)
	}
	if err = json.Unmarshal(b, &p); err != nil {
		return p, nil, err
	}
	if p.SchemaVersion != 1 || p.ProgressMS < 0 || p.ProgressMS > p.DurationMS {
		return p, nil, errors.New("invalid playback bounds")
	}
	return p, b, nil
}
func (h harness) nativeRun(b []byte, args ...string) ([]byte, error) {
	cmd := exec.CommandContext(h.ctx, h.native, args...)
	cmd.Stdin = bytes.NewReader(b)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return out, fmt.Errorf("native %v: %w: %s", args, err, out)
	}
	return out, nil
}
func (h harness) check() error {
	p, b, err := h.snapshot()
	if err != nil {
		return err
	}
	if p.State != "paused" || p.TrackID != "fixture-orbit" {
		return errors.New("fixture initial state differs")
	}
	if _, err = h.nativeRun(b, "--wall-ms", strconv.FormatInt(p.UpdatedAtMS, 10)); err != nil {
		return err
	}
	art, status, err := h.request("GET", p.Artwork.URL, nil)
	if err != nil || status != 200 {
		return fmt.Errorf("artwork status %d: %v", status, err)
	}
	want, err := os.ReadFile(filepath.Join(h.fixtures, "orbit-96.btar"))
	if err != nil {
		return err
	}
	if !bytes.Equal(art, want) {
		return errors.New("live artwork differs from deterministic fixture")
	}
	artPath := filepath.Join(h.scratch, "live.btar")
	if err = os.WriteFile(artPath, art, 0600); err != nil {
		return err
	}
	if _, err = h.nativeRun(b, "--art", artPath, "--wall-ms", strconv.FormatInt(p.UpdatedAtMS, 10)); err != nil {
		return err
	}
	for _, step := range []struct{ button, action, state, id string }{
		{"play_pause", "play", "playing", "fixture-orbit"},
		{"play_pause", "pause", "paused", "fixture-orbit"},
		{"next", "next", "paused", "fixture-tide"},
		{"previous", "previous", "paused", "fixture-orbit"},
		{"shuffle", "shuffle", "paused", "fixture-orbit"},
		{"repeat", "repeat", "paused", "fixture-orbit"},
		{"repeat", "repeat", "paused", "fixture-orbit"},
		{"repeat", "repeat", "paused", "fixture-orbit"},
		{"next", "next", "paused", "fixture-signal"},
	} {
		command, err := h.nativeRun(b, "--control", step.button, "--wall-ms", strconv.FormatInt(p.UpdatedAtMS, 10))
		if err != nil {
			return err
		}
		var c models.Control
		if json.Unmarshal(command, &c) != nil || c.Validate() != nil || c.Action != step.action {
			return fmt.Errorf("native control mismatch: %s", command)
		}
		if _, status, err = h.request("POST", "/v1/control", command); err != nil || status != 200 {
			return fmt.Errorf("control %s status %d: %v", step.action, status, err)
		}
		p, b, err = h.snapshot()
		if err != nil {
			return err
		}
		if p.State != step.state || p.TrackID != step.id {
			return fmt.Errorf("control %s result %+v", step.action, p)
		}
		if c.Action == "shuffle" && p.Shuffle != c.Bool() {
			return errors.New("shuffle mismatch")
		}
		if c.Action == "repeat" && p.Repeat != c.String() {
			return errors.New("repeat mismatch")
		}
		if p.TrackID == "fixture-tide" && p.Context.Type != "album" {
			return errors.New("album fallback missing")
		}
		if p.TrackID == "fixture-signal" && (p.Artwork.URL != "" || p.Context.Type != "unknown") {
			return errors.New("missing metadata fallback")
		}
		if _, err = h.nativeRun(b, "--wall-ms", strconv.FormatInt(p.UpdatedAtMS, 10)); err != nil {
			return err
		}
	}
	for _, name := range []string{"playing", "paused", "idle", "unavailable", "missing", "stale"} {
		data, err := os.ReadFile(filepath.Join(h.fixtures, name+".json"))
		if err != nil {
			return err
		}
		if _, err = h.nativeRun(data, "--wall-ms", "1700000000000"); err != nil {
			return err
		}
	}
	for _, args := range [][]string{{"--offline"}, {"--elapsed-ms", "16000"}, {"--wall-ms", strconv.FormatInt(p.UpdatedAtMS+16000, 10)}} {
		if _, err = h.nativeRun(b, args...); err != nil {
			return err
		}
		if _, err = h.nativeRun(b, append(args, "--control", "next")...); err == nil {
			return errors.New("offline or stale control accepted")
		}
	}
	invalid, err := os.ReadFile(filepath.Join(h.fixtures, "invalid.json"))
	if err != nil {
		return err
	}
	if _, err = h.nativeRun(invalid); err == nil {
		return errors.New("invalid snapshot accepted")
	}
	if _, err = h.nativeRun(bytes.Repeat([]byte("x"), 8193)); err == nil {
		return errors.New("oversized snapshot accepted")
	}
	badArt := filepath.Join(h.scratch, "truncated.btar")
	if err = os.WriteFile(badArt, art[:len(art)-1], 0600); err != nil {
		return err
	}
	if _, err = h.nativeRun(b, "--art", badArt); err == nil {
		return errors.New("truncated artwork accepted")
	}
	_, status, err = h.request("POST", "/v1/control", []byte(`{"action":"shuffle","value":"true"}`))
	if err != nil || status != 400 {
		return errors.New("invalid control accepted")
	}
	return nil
}

func run() error {
	server := flag.String("server", "", "server binary path")
	native := flag.String("native", "", "native firmware CLI path")
	fixtures := flag.String("fixtures", "", "protocol fixture directory")
	flag.Parse()
	if *server == "" || *native == "" || *fixtures == "" {
		return errors.New("set -server, -native and -fixtures")
	}
	scratch, err := os.MkdirTemp("", "bottie-integration-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(scratch)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	addr := ln.Addr().String()
	if err = ln.Close(); err != nil {
		return err
	}
	secret := make([]byte, 32)
	if _, err = rand.Read(secret); err != nil {
		return err
	}
	token := hex.EncodeToString(secret)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, *server)
	cmd.Dir = scratch
	for _, v := range os.Environ() {
		if !strings.HasPrefix(v, "BOTTIE_") && !strings.HasPrefix(v, "SPOTIFY_") {
			cmd.Env = append(cmd.Env, v)
		}
	}
	cmd.Env = append(cmd.Env, "BOTTIE_MODE=fixture", "BOTTIE_BIND="+addr, "BOTTIE_DEVICE_TOKEN="+token)
	if err = cmd.Start(); err != nil {
		return err
	}
	defer func() { _ = cmd.Process.Signal(syscall.SIGTERM); _ = cmd.Wait() }()
	h := harness{ctx: ctx, client: &http.Client{Timeout: 2 * time.Second}, base: "http://" + addr, token: token, native: *native, fixtures: *fixtures, scratch: scratch}
	ready := false
	for i := 0; i < 60; i++ {
		if _, _, err = h.snapshot(); err == nil {
			ready = true
			break
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(50 * time.Millisecond):
		}
	}
	if !ready {
		return errors.New("fixture server did not become ready")
	}
	if err = h.check(); err != nil {
		return err
	}
	fmt.Println("INTEGRATION_OK")
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

package config

import (
	"fmt"
	"net"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/joho/godotenv"
)

type Config struct {
	Mode         string
	Bind         string
	DeviceToken  string
	ClientID     string
	RedirectURI  string
	ArtworkSize  int
	PollInterval time.Duration
	AllowedHosts map[string]bool
}

func Load() (Config, error) {
	if err := godotenv.Load(); err != nil && !os.IsNotExist(err) {
		return Config{}, fmt.Errorf("cannot load .env; check file permissions, variable names and quoting")
	}
	return Parse(os.Getenv)
}

func Parse(get func(string) string) (Config, error) {
	c := Config{Mode: get("BOTTIE_MODE"), Bind: get("BOTTIE_BIND"), DeviceToken: get("BOTTIE_DEVICE_TOKEN"), ClientID: get("SPOTIFY_CLIENT_ID"), RedirectURI: get("SPOTIFY_REDIRECT_URI"), ArtworkSize: 96, PollInterval: 3 * time.Second}
	if c.Mode == "" {
		c.Mode = "fixture"
	}
	if c.Bind == "" {
		c.Bind = "127.0.0.1:8787"
	}
	if c.Mode != "fixture" && c.Mode != "spotify" {
		return c, fmt.Errorf("BOTTIE_MODE must be fixture or spotify")
	}
	host, port, err := net.SplitHostPort(c.Bind)
	if err != nil {
		return c, fmt.Errorf("BOTTIE_BIND must be an IP literal and port")
	}
	ip := net.ParseIP(host)
	n, err := strconv.Atoi(port)
	if ip == nil || err != nil || n < 1 || n > 65535 {
		return c, fmt.Errorf("BOTTIE_BIND must be an IP literal and port 1..65535")
	}
	if c.DeviceToken != "" && (len(c.DeviceToken) < 32 || len(c.DeviceToken) > 256 || strings.IndexFunc(c.DeviceToken, func(r rune) bool { return r < 33 || r > 126 }) >= 0) {
		return c, fmt.Errorf("BOTTIE_DEVICE_TOKEN must be 32..256 printable ASCII characters without spaces")
	}
	if !ip.IsLoopback() && c.DeviceToken == "" {
		return c, fmt.Errorf("set BOTTIE_DEVICE_TOKEN before binding to a LAN or wildcard address")
	}
	if v := get("BOTTIE_ARTWORK_SIZE"); v != "" {
		c.ArtworkSize, err = strconv.Atoi(v)
		if err != nil || c.ArtworkSize < 16 || c.ArtworkSize > 240 {
			return c, fmt.Errorf("BOTTIE_ARTWORK_SIZE must be 16..240")
		}
	}
	if v := get("BOTTIE_POLL_SECONDS"); v != "" {
		n, err = strconv.Atoi(v)
		if err != nil || n < 2 || n > 10 {
			return c, fmt.Errorf("BOTTIE_POLL_SECONDS must be 2..10")
		}
		c.PollInterval = time.Duration(n) * time.Second
	}
	c.AllowedHosts = map[string]bool{net.JoinHostPort("127.0.0.1", port): true, net.JoinHostPort("::1", port): true, c.Bind: true}
	if c.Mode == "spotify" {
		if len(c.ClientID) != 32 || strings.IndexFunc(c.ClientID, func(r rune) bool { return !strings.ContainsRune("0123456789abcdefABCDEF", r) }) >= 0 {
			return c, fmt.Errorf("set SPOTIFY_CLIENT_ID to the 32-character application ID")
		}
		if c.RedirectURI == "" {
			c.RedirectURI = "http://127.0.0.1:" + port + "/auth/callback"
		}
		u, e := url.Parse(c.RedirectURI)
		if e != nil || u.Scheme != "http" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || u.Path != "/auth/callback" || u.Port() != port || net.ParseIP(u.Hostname()) == nil || !net.ParseIP(u.Hostname()).IsLoopback() {
			return c, fmt.Errorf("SPOTIFY_REDIRECT_URI must be http://127.0.0.1:%s/auth/callback (or [::1] on the same port)", port)
		}
		if !ip.IsUnspecified() && !ip.Equal(net.ParseIP(u.Hostname())) {
			return c, fmt.Errorf("BOTTIE_BIND must accept the configured loopback callback; use a wildcard bind with device authentication for LAN access")
		}
	}
	return c, nil
}

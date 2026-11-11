package config

import (
	"strings"
	"testing"
)

func TestConfiguration(t *testing.T) {
	for _, tt := range []struct {
		name string
		env  map[string]string
		want string
	}{
		{"defaults", nil, ""},
		{"lan needs secret", map[string]string{"BOTTIE_BIND": "0.0.0.0:8787"}, "BOTTIE_DEVICE_TOKEN"},
		{"lan authenticated", map[string]string{"BOTTIE_BIND": "0.0.0.0:8787", "BOTTIE_DEVICE_TOKEN": strings.Repeat("s", 32)}, ""},
		{"short secret", map[string]string{"BOTTIE_DEVICE_TOKEN": "sensitive"}, "BOTTIE_DEVICE_TOKEN"},
		{"host", map[string]string{"BOTTIE_BIND": "localhost:8787"}, "IP literal"},
		{"size", map[string]string{"BOTTIE_ARTWORK_SIZE": "241"}, "16..240"},
		{"poll", map[string]string{"BOTTIE_POLL_SECONDS": "0"}, "2..10"},
		{"mode", map[string]string{"BOTTIE_MODE": "live"}, "fixture or spotify"},
		{"missing client", map[string]string{"BOTTIE_MODE": "spotify"}, "SPOTIFY_CLIENT_ID"},
		{"spotify", map[string]string{"BOTTIE_MODE": "spotify", "SPOTIFY_CLIENT_ID": strings.Repeat("a", 32)}, ""},
		{"localhost callback", map[string]string{"BOTTIE_MODE": "spotify", "SPOTIFY_CLIENT_ID": strings.Repeat("a", 32), "SPOTIFY_REDIRECT_URI": "http://localhost:8787/auth/callback"}, "SPOTIFY_REDIRECT_URI"},
		{"foreign callback", map[string]string{"BOTTIE_MODE": "spotify", "SPOTIFY_CLIENT_ID": strings.Repeat("a", 32), "SPOTIFY_REDIRECT_URI": "https://example.com/auth/callback"}, "SPOTIFY_REDIRECT_URI"},
		{"unreachable callback", map[string]string{"BOTTIE_MODE": "spotify", "SPOTIFY_CLIENT_ID": strings.Repeat("a", 32), "BOTTIE_BIND": "192.168.0.3:8787", "BOTTIE_DEVICE_TOKEN": strings.Repeat("s", 32)}, "accept the configured loopback"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			c, err := Parse(func(k string) string { return tt.env[k] })
			if tt.want == "" {
				if err != nil {
					t.Fatal(err)
				}
				if c.ArtworkSize != 96 {
					t.Fatal(c.ArtworkSize)
				}
			} else if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("want %s, got %v", tt.want, err)
			}
			if err != nil && strings.Contains(err.Error(), "sensitive") {
				t.Fatal("secret in error")
			}
		})
	}
}

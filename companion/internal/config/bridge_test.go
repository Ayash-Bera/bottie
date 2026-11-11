package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBridgeConfiguration(t *testing.T) {
	for _, tt := range []struct {
		name, bind, token string
		mode              os.FileMode
		want              string
	}{
		{"default loopback", "", strings.Repeat("s", 32), 0600, ""},
		{"private interface", "192.168.1.148:8788", strings.Repeat("s", 32), 0600, ""},
		{"token newline", "", strings.Repeat("s", 256) + "\n", 0600, ""},
		{"firmware alphabet", "", strings.Repeat("-._~+/=", 5), 0600, ""},
		{"wildcard", "0.0.0.0:8788", strings.Repeat("s", 32), 0600, "specific private"},
		{"public", "8.8.8.8:8788", strings.Repeat("s", 32), 0600, "specific private"},
		{"hostname", "localhost:8788", strings.Repeat("s", 32), 0600, "specific private"},
		{"ipv6", "[::1]:8788", strings.Repeat("s", 32), 0600, "specific private"},
		{"upstream port", "127.0.0.1:8787", strings.Repeat("s", 32), 0600, "excluding companion"},
		{"zero port", "127.0.0.1:0", strings.Repeat("s", 32), 0600, "specific private"},
		{"public mode", "", strings.Repeat("s", 32), 0644, "0600"},
		{"short token", "", "secret-marker", 0600, "32..256"},
		{"long token", "", strings.Repeat("s", 258), 0600, "32..256"},
		{"space", "", strings.Repeat("s", 32) + " ", 0600, "32..256"},
		{"multiple lines", "", strings.Repeat("s", 32) + "\n\n", 0600, "32..256"},
		{"invalid alphabet", "", strings.Repeat("s", 32) + "'", 0600, "32..256"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "device-token")
			if err := os.WriteFile(path, []byte(tt.token), tt.mode); err != nil {
				t.Fatal(err)
			}
			calls := map[string]int{}
			cfg, err := ParseBridge(func(k string) string {
				calls[k]++
				return map[string]string{"BOTTIE_BRIDGE_BIND": tt.bind, "BOTTIE_BRIDGE_TOKEN_FILE": path}[k]
			})
			if tt.want == "" {
				if err != nil {
					t.Fatal(err)
				}
				if cfg.Token != strings.TrimSuffix(tt.token, "\n") {
					t.Fatal("token changed")
				}
				if tt.bind == "" && cfg.Bind != "127.0.0.1:8788" {
					t.Fatal(cfg.Bind)
				}
			} else if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("want %s, got %v", tt.want, err)
			}
			if err != nil && strings.Contains(err.Error(), tt.token) {
				t.Fatal("token leaked in error")
			}
			if len(calls) != 2 || calls["BOTTIE_BRIDGE_BIND"] != 1 || calls["BOTTIE_BRIDGE_TOKEN_FILE"] != 1 {
				t.Fatal(calls)
			}
		})
	}
}

func TestBridgeTokenFileBoundaries(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "token")
	if err := os.WriteFile(file, []byte(strings.Repeat("s", 32)), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "link")
	if err := os.Symlink(file, link); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{"", "relative", filepath.Join(dir, "missing"), dir, link} {
		t.Run(path, func(t *testing.T) {
			_, err := ParseBridge(func(k string) string {
				if k == "BOTTIE_BRIDGE_TOKEN_FILE" {
					return path
				}
				return ""
			})
			if err == nil {
				t.Fatal("accepted invalid token file")
			}
		})
	}
}

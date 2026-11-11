package config

import (
	"fmt"
	"io"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
)

type Bridge struct {
	Bind  string
	Token string
}

func LoadBridge() (Bridge, error) { return ParseBridge(os.Getenv) }

func ParseBridge(get func(string) string) (Bridge, error) {
	c := Bridge{Bind: get("BOTTIE_BRIDGE_BIND")}
	path := get("BOTTIE_BRIDGE_TOKEN_FILE")
	if c.Bind == "" {
		c.Bind = "127.0.0.1:8788"
	}
	addr, err := netip.ParseAddrPort(c.Bind)
	if err != nil || !addr.Addr().Is4() || (!addr.Addr().IsLoopback() && !addr.Addr().IsPrivate()) || addr.Port() == 0 || addr.Port() == 8787 || addr.String() != c.Bind {
		return Bridge{}, fmt.Errorf("BOTTIE_BRIDGE_BIND must be a specific private or loopback IPv4 address and port, excluding companion port 8787")
	}
	if !filepath.IsAbs(path) {
		return Bridge{}, fmt.Errorf("set BOTTIE_BRIDGE_TOKEN_FILE to an absolute path to a private 0600 device token file")
	}
	before, err := os.Lstat(path)
	if err != nil {
		return Bridge{}, fmt.Errorf("inspect BOTTIE_BRIDGE_TOKEN_FILE: %w", err)
	}
	if !before.Mode().IsRegular() || before.Mode().Perm() != 0600 {
		return Bridge{}, fmt.Errorf("BOTTIE_BRIDGE_TOKEN_FILE must be a regular file with mode 0600, not a symbolic link")
	}
	f, err := os.Open(path)
	if err != nil {
		return Bridge{}, fmt.Errorf("open BOTTIE_BRIDGE_TOKEN_FILE: %w", err)
	}
	defer f.Close()
	after, err := f.Stat()
	if err != nil || !os.SameFile(before, after) || !after.Mode().IsRegular() || after.Mode().Perm() != 0600 {
		return Bridge{}, fmt.Errorf("BOTTIE_BRIDGE_TOKEN_FILE changed while opening; check its permissions and retry")
	}
	b, err := io.ReadAll(io.LimitReader(f, 258))
	if err != nil {
		return Bridge{}, fmt.Errorf("read BOTTIE_BRIDGE_TOKEN_FILE: %w", err)
	}
	c.Token = strings.TrimSuffix(string(b), "\n")
	if len(c.Token) < 32 || len(c.Token) > 256 || strings.IndexFunc(c.Token, func(r rune) bool {
		return !(r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || strings.ContainsRune("-._~+/=", r))
	}) >= 0 {
		return Bridge{}, fmt.Errorf("device token must contain 32..256 letters, digits or -._~+/=, with at most one final newline")
	}
	return c, nil
}

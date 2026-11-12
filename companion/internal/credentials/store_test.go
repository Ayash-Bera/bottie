package credentials

import (
	"context"
	"errors"
	"strings"
	"testing"
)

func TestRecordIdentityAndBounds(t *testing.T) {
	k := NewKeyring("client", "redirect")
	for _, tt := range []struct {
		name   string
		record Record
		valid  bool
	}{
		{"valid", Record{Version: 1, ClientID: "client", RedirectURI: "redirect", RefreshToken: "token"}, true},
		{"version", Record{Version: 2, ClientID: "client", RedirectURI: "redirect", RefreshToken: "token"}, false},
		{"other app", Record{Version: 1, ClientID: "other", RedirectURI: "redirect", RefreshToken: "token"}, false},
		{"other callback", Record{Version: 1, ClientID: "client", RedirectURI: "other", RefreshToken: "token"}, false},
		{"missing", Record{Version: 1, ClientID: "client", RedirectURI: "redirect"}, false},
		{"oversize", Record{Version: 1, ClientID: "client", RedirectURI: "redirect", RefreshToken: strings.Repeat("x", 8193)}, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			if k.valid(tt.record) != tt.valid {
				t.Fatal("identity/bounds")
			}
			if !tt.valid && !errors.Is(k.Save(context.Background(), tt.record), ErrInvalid) {
				t.Fatal("invalid record reached backend")
			}
		})
	}
}

func TestCancelledStoreDoesNotConnect(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := NewKeyring("test", "test").Check(ctx); !errors.Is(err, ErrUnavailable) {
		t.Fatal("cancelled store")
	}
}

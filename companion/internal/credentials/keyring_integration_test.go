//go:build linux && cgo

package credentials

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"os"
	"testing"
	"time"
)

func TestDesktopKeyring(t *testing.T) {
	if os.Getenv("BOTTIE_TEST_KEYRING") != "1" {
		t.Skip("explicit app-scoped desktop probe only")
	}
	id := make([]byte, 16)
	if _, err := rand.Read(id); err != nil {
		t.Fatal("test identity")
	}
	identity := "bottie-probe-" + hex.EncodeToString(id)
	k := NewKeyring(identity, "http://127.0.0.1:8787/test-only")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := k.Check(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := k.Load(ctx); !errors.Is(err, ErrNotFound) {
		t.Fatal("probe identity already exists or lookup failed")
	}
	t.Cleanup(func() {
		c, done := context.WithTimeout(context.Background(), 5*time.Second)
		defer done()
		if err := k.Delete(c); err != nil {
			t.Error("probe cleanup failed")
		}
	})
	r := Record{Version: 1, ClientID: identity, RedirectURI: k.redirectURI, RefreshToken: "synthetic-probe-only", CanRead: true}
	if err := k.Save(ctx, r); err != nil {
		t.Fatal(err)
	}
	// Reopen through a new service/session to check the stored representation.
	other := NewKeyring(identity, k.redirectURI)
	if got, err := other.Load(ctx); err != nil || got != r {
		t.Fatal("probe roundtrip failed")
	}
	r.RefreshToken = "synthetic-rotation-only"
	if err := other.Save(ctx, r); err != nil {
		t.Fatal(err)
	}
	if got, err := k.Load(ctx); err != nil || got != r {
		t.Fatal("probe rotation failed")
	}
	if err := k.Delete(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := other.Load(ctx); !errors.Is(err, ErrNotFound) {
		t.Fatal("probe deletion failed")
	}
}

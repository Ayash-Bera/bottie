package httpserver

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"syscall"
	"testing"
	"time"
)

func TestAddressReadiness(t *testing.T) {
	for _, scenario := range []string{"late address", "occupied port", "cancelled"} {
		t.Run(scenario, func(t *testing.T) {
			ctx, stop := context.WithCancel(context.Background())
			defer stop()
			calls := 0
			fn := func(ctx context.Context, network, address string) (net.Listener, error) {
				calls++
				if scenario == "occupied port" {
					return nil, &net.OpError{Op: "listen", Err: syscall.EADDRINUSE}
				}
				if scenario == "cancelled" {
					stop()
				}
				if calls <= 2 {
					return nil, &net.OpError{Op: "listen", Err: syscall.EADDRNOTAVAIL}
				}
				var lc net.ListenConfig
				return lc.Listen(ctx, network, address)
			}
			ln, err := listen(ctx, "127.0.0.1:0", time.Millisecond, fn, slog.New(slog.NewTextHandler(io.Discard, nil)))
			switch scenario {
			case "late address":
				if err != nil || calls != 3 {
					t.Fatal("address did not recover")
				}
				_ = ln.Close()
			case "occupied port":
				if !errors.Is(err, syscall.EADDRINUSE) || calls != 1 {
					t.Fatal("occupied port should fail, never change bind")
				}
			case "cancelled":
				if !errors.Is(err, http.ErrServerClosed) || calls != 1 {
					t.Fatal("cancel did not stop retry")
				}
			}
		})
	}
}

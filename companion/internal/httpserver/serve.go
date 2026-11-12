package httpserver

import (
	"context"
	"errors"
	"log/slog"
	"net"
	"net/http"
	"syscall"
	"time"
)

func listen(ctx context.Context, address string, delay time.Duration, dial func(context.Context, string, string) (net.Listener, error), log *slog.Logger) (net.Listener, error) {
	waiting := false
	for {
		if ctx.Err() != nil {
			return nil, http.ErrServerClosed
		}
		ln, err := dial(ctx, "tcp", address)
		if err == nil {
			return ln, nil
		}
		if !errors.Is(err, syscall.EADDRNOTAVAIL) {
			return nil, err
		}
		if !waiting {
			log.Info("waiting for configured network address", "address", address)
			waiting = true
		}
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil, http.ErrServerClosed
		case <-timer.C:
		}
	}
}

func ListenAndServe(ctx context.Context, srv *http.Server, log *slog.Logger) error {
	var lc net.ListenConfig
	ln, err := listen(ctx, srv.Addr, 5*time.Second, lc.Listen, log)
	if err != nil {
		return err
	}
	return srv.Serve(ln)
}

package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"bottie/companion/internal/config"
	"bottie/companion/internal/devicebridge"
	"bottie/companion/internal/httpserver"
)

func run() error {
	cfg, err := config.LoadBridge()
	if err != nil {
		return err
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, nil))
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	srv := &http.Server{Addr: cfg.Bind, Handler: devicebridge.New(cfg, log), ReadHeaderTimeout: 3 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 << 10}
	var workers sync.WaitGroup
	workers.Go(func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdown); err != nil {
			log.Error("device gateway shutdown timed out")
			_ = srv.Close()
		}
	})
	log.Info("device gateway listening", "address", cfg.Bind)
	err = httpserver.ListenAndServe(ctx, srv, log)
	stop()
	workers.Wait()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func main() {
	if err := run(); err != nil {
		slog.Error("device gateway stopped", "reason", err.Error())
		os.Exit(1)
	}
}

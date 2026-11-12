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

	"bottie/companion/internal/api"
	"bottie/companion/internal/artwork"
	"bottie/companion/internal/config"
	"bottie/companion/internal/credentials"
	"bottie/companion/internal/fixture"
	"bottie/companion/internal/httpserver"
	"bottie/companion/internal/models"
	"bottie/companion/internal/spotify"
)

func run() error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, nil))
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	store := artwork.New(cfg.ArtworkSize, nil)
	var player models.Player
	var oauth api.OAuth
	var workers sync.WaitGroup
	if cfg.Mode == "fixture" {
		player, err = fixture.New(store, nil)
		if err != nil {
			return err
		}
	} else {
		var tokens credentials.Store
		if cfg.TokenStore == "keyring" {
			tokens = credentials.NewKeyring(cfg.ClientID, cfg.RedirectURI)
		}
		p := spotify.NewWithStore(cfg.ClientID, cfg.RedirectURI, store, nil, tokens)
		player = p
		oauth = p
		workers.Go(func() { p.Run(ctx, cfg.PollInterval) })
	}
	srv := &http.Server{Addr: cfg.Bind, Handler: api.New(cfg, player, store, oauth, log), ReadHeaderTimeout: 3 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 << 10}
	workers.Go(func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdown); err != nil {
			log.Error("server shutdown timed out")
			_ = srv.Close()
		}
	})
	log.Info("companion listening", "address", cfg.Bind, "mode", cfg.Mode)
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
		slog.Error("companion stopped", "reason", err.Error())
		os.Exit(1)
	}
}

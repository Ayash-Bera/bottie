package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"bottie/companion/internal/models"
	"bottie/companion/internal/textmask"
)

func run() error {
	if len(os.Args) != 2 {
		return fmt.Errorf("usage: detail-fixtures OUTPUT_DIRECTORY")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	store, err := textmask.NewDetails(ctx)
	if err != nil {
		return err
	}
	defer store.Close()
	masks, err := textmask.New()
	if err != nil {
		return err
	}
	defer masks.Close()
	if err := os.MkdirAll(os.Args[1], 0700); err != nil {
		return err
	}
	for _, c := range []struct{ name, title, artist, album, playlist string }{
		{"bengali", "ক্ষিতিজ শ্রদ্ধা কৃষ্ণ চাঁদের আলো কৌতূহল", "শ্রদ্ধা ও কৃষ্ণ", "চাঁদের আলো", "কৌতূহল"},
		{"japanese", "夜に駆ける カタカナ・ひらがな 東京 / ORBIT 2026", "カタカナ・ひらがな", "音楽（ライブ）", "パーティー"},
		{"mixed", "東京 / ORBIT 2026 ক্ষিতিজ", "বাংলা ARTIST", "日本語 ALBUM", "বাংলা PLAYLIST"},
		{"album-only", "কৌতূহল", "চাঁদের আলো", "夜に駆ける", ""},
		{"long", strings.Repeat("M", 252), strings.Repeat("音楽（ライブ）", 6), strings.Repeat("শ্রদ্ধা ", 10), "fictional long names"},
		{"max-pages", strings.Repeat("M", 252), strings.Repeat("M", 252), strings.Repeat("M", 252), strings.Repeat("M", 252)},
		{"uncertain", strings.Repeat("m", 250) + "ক্", "canonical が が", "conjunct ক্ষ ক্‌ষ", "near producer boundary"},
	} {
		p := models.Empty("playing")
		p.TrackID, p.Title, p.Artist, p.Album = "fixture-detail-"+c.name, c.title, c.artist, c.album
		p.Context = models.Context{Type: "playlist", Name: c.playlist}
		if c.playlist == "" {
			p.Context = models.Context{Type: "album", Name: c.album}
		}
		p.DurationMS, p.ProgressMS, p.UpdatedAtMS = 240000, 60000, 1700000000000
		p.Artwork = models.Artwork{URL: "/v1/artwork/orbit-96", Width: 96, Height: 96}
		p.ControlTarget, p.ControlTrack = strings.Repeat("a", 32), strings.Repeat("b", 32)
		p.Capabilities = models.Capabilities{Play: true, Pause: true, Seek: true}
		p = p.Bounded()
		p.TextMask, err = masks.Prepare(ctx, p)
		if err != nil {
			return fmt.Errorf("fixture %s line mask: %w", c.name, err)
		}
		for p.Details == nil {
			p.Details = store.Observe(p)
			if p.Details != nil {
				break
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(time.Millisecond):
			}
		}
		if p.TextMask != nil {
			body, ok := masks.Get(p.TextMask.URL)
			if !ok {
				return fmt.Errorf("missing line mask")
			}
			if err := os.WriteFile(filepath.Join(os.Args[1], c.name+".btxt"), body, 0600); err != nil {
				return err
			}
		}
		for field, pages := range p.Details.Fields() {
			if pages == nil {
				continue
			}
			for index, digest := range pages.Pages {
				body, ok := store.Get("/v1/detail/" + digest)
				if !ok {
					return fmt.Errorf("missing detail page")
				}
				name := fmt.Sprintf("%s-%d-%d.btdt", c.name, field, index)
				if err := os.WriteFile(filepath.Join(os.Args[1], name), body, 0600); err != nil {
					return err
				}
				fmt.Printf("%s %s %d\n", name, digest, len(body))
			}
		}
		data, err := json.MarshalIndent(p, "", "  ")
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(os.Args[1], c.name+".json"), append(data, '\n'), 0600); err != nil {
			return err
		}
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

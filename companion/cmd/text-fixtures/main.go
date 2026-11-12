package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"bottie/companion/internal/models"
	"bottie/companion/internal/textmask"
)

func run() error {
	if len(os.Args) != 2 {
		return fmt.Errorf("usage: text-fixtures OUTPUT_DIRECTORY")
	}
	store, err := textmask.New()
	if err != nil {
		return err
	}
	defer store.Close()
	if err := os.MkdirAll(os.Args[1], 0700); err != nil {
		return err
	}
	for _, c := range []struct{ name, title, artist, album, playlist string }{
		{"bengali", "ক্ষিতিজ", "শ্রদ্ধা ও কৃষ্ণ", "চাঁদের আলো", "কৌতূহল"},
		{"japanese", "夜に駆ける", "カタカナ・ひらがな", "音楽（ライブ）", "パーティー"},
		{"mixed", "東京 / ORBIT 2026", "বাংলা ARTIST", "日本語 ALBUM", "বাংলা PLAYLIST"},
		{"album-only", "কৌতূহল", "চাঁদের আলো", "夜に駆ける", ""},
		{"long", "ক্ষিতিজ শ্রদ্ধা কৃষ্ণ চাঁদের আলো কৌতূহল ক্ষিতিজ শ্রদ্ধা", "東京交響楽団 カタカナ・ひらがな パーティー 音楽", "চাঁদের আলো কৌতূহল শ্রদ্ধা কৃষ্ণ ক্ষিতিজ চাঁদের আলো", "夜に駆ける 東京交響楽団 カタカナ・ひらがな パーティー"},
	} {
		p := models.Empty("playing")
		p.TrackID, p.Title, p.Artist, p.Album = "fixture-text-"+c.name, c.title, c.artist, c.album
		p.Context = models.Context{Type: "playlist", Name: c.playlist}
		p.DurationMS, p.ProgressMS, p.UpdatedAtMS = 240000, 60000, 1700000000000
		p.Artwork = models.Artwork{URL: "/v1/artwork/orbit-96", Width: 96, Height: 96}
		p = p.Bounded()
		p.TextMask, err = store.Prepare(context.Background(), p)
		if err != nil || p.TextMask == nil {
			return fmt.Errorf("fixture %s text: %w", c.name, err)
		}
		body, ok := store.Get(p.TextMask.URL)
		if !ok {
			return fmt.Errorf("fixture %s lost bundle", c.name)
		}
		data, err := json.MarshalIndent(p, "", "  ")
		if err != nil {
			return err
		}
		for ext, value := range map[string][]byte{".json": append(data, '\n'), ".btxt": body} {
			if err := os.WriteFile(filepath.Join(os.Args[1], c.name+ext), value, 0600); err != nil {
				return err
			}
		}
		fmt.Printf("%s: %d bytes %s\n", c.name, len(body), p.TextMask.URL)
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

package artwork

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"
)

func TestOriginalFixture(t *testing.T) {
	want, err := os.ReadFile("../../../protocol/fixtures/orbit-96.btar")
	if err != nil {
		t.Fatal(err)
	}
	got, err := Encode(Original(0), 96)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatal("binary fixture differs from original cover")
	}
	if string(got[:4]) != "BTAR" || got[4] != 1 || got[5] != 1 || binary.LittleEndian.Uint16(got[6:]) != 96 || binary.LittleEndian.Uint16(got[8:]) != 96 || binary.LittleEndian.Uint16(got[10:]) != 192 || int(binary.LittleEndian.Uint32(got[12:])) != len(got)-16 {
		t.Fatal("header mismatch")
	}
}

func TestCachedReadsDuringDownload(t *testing.T) {
	started, finish := make(chan struct{}), make(chan struct{})
	client := &http.Client{Transport: roundTrip(func(r *http.Request) (*http.Response, error) {
		close(started)
		<-finish
		return nil, fmt.Errorf("offline")
	})}
	s := New(96, client)
	if _, err := s.AddOriginal("fixture", 0); err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { _, _ = s.FromSpotify(context.Background(), "https://i.scdn.co/image/slow"); close(done) }()
	<-started
	read := make(chan bool)
	go func() { _, ok := s.Get("fixture"); read <- ok }()
	select {
	case ok := <-read:
		if !ok {
			t.Error("cached image missing")
		}
	case <-time.After(time.Second):
		t.Error("cached read blocked on network")
	}
	close(finish)
	<-done
}

func TestCanceledDownloadDoesNotPoisonRetry(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	started := make(chan struct{})
	done := make(chan struct{})
	var image bytes.Buffer
	if err := png.Encode(&image, Original(0)); err != nil {
		t.Fatal(err)
	}
	calls := 0
	s := New(96, &http.Client{Transport: roundTrip(func(r *http.Request) (*http.Response, error) {
		calls++
		if calls == 1 {
			close(started)
			<-r.Context().Done()
			return nil, r.Context().Err()
		}
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(image.Bytes()))}, nil
	})})
	go func() { _, _ = s.FromSpotify(ctx, "https://i.scdn.co/image/cancel"); close(done) }()
	<-started
	cancel()
	<-done
	if _, err := s.FromSpotify(context.Background(), "https://i.scdn.co/image/cancel"); err != nil || calls != 2 {
		t.Fatal("canceled identity blocked next legitimate fetch", err)
	}
}

func TestAspectFitAndBounds(t *testing.T) {
	for _, dims := range [][2]int{{32, 16}, {16, 32}, {16, 16}} {
		t.Run(fmt.Sprint(dims), func(t *testing.T) {
			im := image.NewRGBA(image.Rect(0, 0, dims[0], dims[1]))
			for y := 0; y < dims[1]; y++ {
				for x := 0; x < dims[0]; x++ {
					im.Set(x, y, color.RGBA{255, 0, 0, 255})
				}
			}
			im.Set(0, 0, color.RGBA{0, 255, 0, 255})
			im.Set(dims[0]-1, dims[1]-1, color.RGBA{0, 0, 255, 255})
			b, e := Encode(im, 32)
			if e != nil {
				t.Fatal(e)
			}
			ox, oy := 0, 0
			if dims[0] > dims[1] {
				oy = 8
			}
			if dims[1] > dims[0] {
				ox = 8
			}
			pixel := func(x, y int) uint16 { return binary.LittleEndian.Uint16(b[16+2*(y*32+x):]) }
			if pixel(ox, oy) != 0x07e0 || pixel(31-ox, 31-oy) != 0x001f {
				t.Fatal("cover corners cropped or wrong byte order")
			}
			if (ox > 0 || oy > 0) && pixel(0, 0) != 0 {
				t.Fatal("padding not black")
			}
		})
	}
	for _, size := range []int{-1, 0, 15, 241, 65535} {
		if _, e := Encode(Original(0), size); e == nil {
			t.Fatalf("accepted size %d", size)
		}
	}
	if _, e := Decode(make([]byte, MaxDownload+1), 96); e == nil {
		t.Fatal("oversize accepted")
	}
	if _, e := Decode([]byte("not an image"), 96); e == nil {
		t.Fatal("invalid image accepted")
	}
	im := image.NewRGBA(image.Rect(0, 0, 4097, 1))
	var b bytes.Buffer
	if e := png.Encode(&b, im); e != nil {
		t.Fatal(e)
	}
	if _, e := Decode(b.Bytes(), 96); e == nil {
		t.Fatal("large dimensions accepted")
	}
}

type roundTrip func(*http.Request) (*http.Response, error)

func (f roundTrip) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func TestTrustedDownloadsAndCache(t *testing.T) {
	for _, raw := range []string{"http://i.scdn.co/image/x", "https://i.scdn.co.evil/image/x", "https://127.0.0.1/image/x", "https://user@i.scdn.co/image/x", "https://i.scdn.co:443/image/x", "https://i.scdn.co/image/x?q=y"} {
		if Trusted(raw) {
			t.Fatal(raw)
		}
	}
	var encoded bytes.Buffer
	_ = png.Encode(&encoded, Original(0))
	calls := 0
	client := &http.Client{Transport: roundTrip(func(r *http.Request) (*http.Response, error) {
		calls++
		if r.Header.Get("Authorization") != "" {
			t.Fatal("token on artwork request")
		}
		return &http.Response{StatusCode: 200, Body: io.NopCloser(bytes.NewReader(encoded.Bytes())), Header: make(http.Header)}, nil
	})}
	s := New(96, client)
	for i := 0; i < 2; i++ {
		if _, e := s.FromSpotify(context.Background(), "https://i.scdn.co/image/a"); e != nil {
			t.Fatal(e)
		}
	}
	if calls != 1 {
		t.Fatalf("cache calls=%d", calls)
	}
	if _, e := s.FromSpotify(context.Background(), "http://127.0.0.1/private"); e == nil || calls != 1 {
		t.Fatal("untrusted download")
	}
	for i := 0; i < 12; i++ {
		_, e := s.FromSpotify(context.Background(), fmt.Sprintf("https://i.scdn.co/image/%d", i))
		if e != nil {
			t.Fatal(e)
		}
	}
	if len(s.entries) > 8 || s.bytes > MaxCacheBytes {
		t.Fatal("unbounded cache")
	}
	client.Transport = roundTrip(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: 302, Header: http.Header{"Location": []string{"http://127.0.0.1/private"}}, Body: io.NopCloser(strings.NewReader(""))}, nil
	})
	if _, e := New(96, client).FromSpotify(context.Background(), "https://i.scdn.co/image/redir"); e == nil {
		t.Fatal("redirect accepted")
	}
}

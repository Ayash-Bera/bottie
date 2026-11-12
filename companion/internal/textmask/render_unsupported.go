//go:build !linux || !cgo

package textmask

import "fmt"

func newRenderer() (renderer, error) {
	return nil, fmt.Errorf("metadata masks require Linux with cgo, PangoCairo, PangoFc and the documented pinned Noto fonts")
}

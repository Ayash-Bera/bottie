//go:build !linux || !cgo

package credentials

import "context"

func (k *Keyring) call(context.Context, int, []byte) ([]byte, error) {
	return nil, ErrUnavailable
}

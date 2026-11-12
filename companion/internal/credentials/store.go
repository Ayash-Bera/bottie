package credentials

import (
	"context"
	"encoding/json"
	"errors"
)

var (
	ErrNotFound    = errors.New("no saved Spotify authorization")
	ErrLocked      = errors.New("desktop keyring is locked")
	ErrUnavailable = errors.New("protected desktop keyring is unavailable")
	ErrInvalid     = errors.New("saved Spotify authorization is invalid")
)

const maxRecord = 12 << 10

type Record struct {
	Version      int    `json:"version"`
	ClientID     string `json:"client_id"`
	RedirectURI  string `json:"redirect_uri"`
	RefreshToken string `json:"refresh_token"`
	CanRead      bool   `json:"can_read"`
	CanControl   bool   `json:"can_control"`
}

type Store interface {
	Check(context.Context) error
	Load(context.Context) (Record, error)
	Save(context.Context, Record) error
	Delete(context.Context) error
}

type Keyring struct{ clientID, redirectURI string }

func NewKeyring(clientID, redirectURI string) *Keyring {
	return &Keyring{clientID: clientID, redirectURI: redirectURI}
}

func (k *Keyring) valid(r Record) bool {
	return r.Version == 1 && r.ClientID == k.clientID && r.RedirectURI == k.redirectURI && len(r.RefreshToken) > 0 && len(r.RefreshToken) <= 8192
}

func (k *Keyring) Check(ctx context.Context) error {
	_, err := k.call(ctx, 0, nil)
	return err
}

func (k *Keyring) Load(ctx context.Context) (Record, error) {
	b, err := k.call(ctx, 1, nil)
	if err != nil {
		return Record{}, err
	}
	var r Record
	if len(b) > maxRecord || json.Unmarshal(b, &r) != nil || !k.valid(r) {
		return Record{}, ErrInvalid
	}
	return r, nil
}

func (k *Keyring) Save(ctx context.Context, r Record) error {
	if !k.valid(r) {
		return ErrInvalid
	}
	b, err := json.Marshal(r)
	if err != nil || len(b) > maxRecord {
		return ErrInvalid
	}
	_, err = k.call(ctx, 2, b)
	return err
}

func (k *Keyring) Delete(ctx context.Context) error {
	_, err := k.call(ctx, 3, nil)
	return err
}

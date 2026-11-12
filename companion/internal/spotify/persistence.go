package spotify

import (
	"context"
	"errors"
	"time"

	"bottie/companion/internal/credentials"
	"bottie/companion/internal/models"
)

type AuthorizationStatus struct {
	Authorized bool   `json:"authorized"`
	Storage    string `json:"storage"`
}

func (p *Player) AuthorizationStatus() AuthorizationStatus {
	p.mu.RLock()
	defer p.mu.RUnlock()
	return p.authorization
}

func (p *Player) storageStatus(status string) {
	p.mu.Lock()
	p.authorization = AuthorizationStatus{Authorized: p.tokens.access != "", Storage: status}
	p.mu.Unlock()
}

func storageProblem(err error) *models.ServiceError {
	if errors.Is(err, credentials.ErrLocked) {
		return models.Problem("keyring_locked", 503, "unlock the desktop wallet; saved Spotify authorization will retry automatically")
	}
	if errors.Is(err, credentials.ErrInvalid) {
		return models.Problem("keyring_invalid", 503, "the Bottie authorization entry is invalid or ambiguous; remove only that entry in the desktop wallet and reconnect")
	}
	return models.Problem("keyring_unavailable", 503, "start and unlock the protected desktop wallet with a persistent default collection; authorization will retry automatically")
}

func (p *Player) storageFailure(err error) *models.ServiceError {
	e := storageProblem(err)
	e.RetryAfter = 30
	p.storeError = e
	p.storeRetry = p.now().Add(30 * time.Second)
	p.storageStatus(e.Code)
	return e
}

func (p *Player) record() credentials.Record {
	return credentials.Record{Version: 1, ClientID: p.clientID, RedirectURI: p.redirectURI, RefreshToken: p.tokens.refresh, CanRead: p.tokens.canRead, CanControl: p.tokens.canControl}
}

func (p *Player) syncCredentials(ctx context.Context) *models.ServiceError {
	if p.store == nil {
		p.storageStatus("memory")
		return nil
	}
	if p.now().Before(p.storeRetry) {
		return p.storeError
	}
	if !p.storeLoaded {
		r, err := p.store.Load(ctx)
		if errors.Is(err, credentials.ErrNotFound) {
			p.storeLoaded = true
			p.storageStatus("ready")
		} else if err != nil {
			return p.storageFailure(err)
		} else {
			if r.Version != 1 || r.ClientID != p.clientID || r.RedirectURI != p.redirectURI || r.RefreshToken == "" || len(r.RefreshToken) > 8192 {
				return p.storageFailure(credentials.ErrInvalid)
			}
			p.tokens = token{refresh: r.RefreshToken, canRead: r.CanRead, canControl: r.CanControl}
			p.storeLoaded = true
			p.storageStatus("saved")
		}
	}
	if p.deletePending {
		if err := p.store.Delete(ctx); err != nil {
			return p.storageFailure(err)
		}
		p.deletePending = false
		p.storageStatus("ready")
	}
	if p.storeDirty {
		if err := p.store.Save(ctx, p.record()); err != nil {
			return p.storageFailure(err)
		}
		p.storeDirty = false
		p.storageStatus("saved")
	}
	p.storeError = nil
	p.storeRetry = time.Time{}
	return nil
}

func (p *Player) revoke(ctx context.Context) *models.ServiceError {
	p.tokens = token{}
	p.storeDirty = false
	p.storeLoaded = true
	p.deletePending = p.store != nil
	p.storeRetry = time.Time{}
	if e := p.syncCredentials(ctx); e != nil {
		return e
	}
	return loginRequired()
}

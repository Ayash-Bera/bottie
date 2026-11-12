package spotify

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"bottie/companion/internal/credentials"
)

type memoryStore struct {
	record                  credentials.Record
	err, saveErr, deleteErr error
	loads, saves, deletes   int
}

func (s *memoryStore) Check(context.Context) error { return s.err }
func (s *memoryStore) Load(context.Context) (credentials.Record, error) {
	s.loads++
	if s.err != nil {
		return credentials.Record{}, s.err
	}
	if s.record.RefreshToken == "" {
		return credentials.Record{}, credentials.ErrNotFound
	}
	return s.record, nil
}
func (s *memoryStore) Save(_ context.Context, r credentials.Record) error {
	s.saves++
	if s.saveErr != nil {
		return s.saveErr
	}
	s.record = r
	return nil
}
func (s *memoryStore) Delete(context.Context) error {
	s.deletes++
	if s.deleteErr != nil {
		return s.deleteErr
	}
	s.record = credentials.Record{}
	return nil
}

func saved() credentials.Record {
	return credentials.Record{Version: 1, ClientID: strings.Repeat("a", 32), RedirectURI: "http://127.0.0.1:8787/auth/callback", RefreshToken: "refresh-secret", CanRead: true, CanControl: true}
}

func TestProtectedRestartRefresh(t *testing.T) {
	for _, scenario := range []string{"retain", "rotate", "invalid_grant", "network_failure", "bad_client", "save_failure", "delete_failure"} {
		t.Run(scenario, func(t *testing.T) {
			store := &memoryStore{record: saved()}
			calls := 0
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/token" {
					calls++
					_ = r.ParseForm()
					if r.Form.Get("refresh_token") != "refresh-secret" {
						t.Error("wrong saved credential")
					}
					switch scenario {
					case "invalid_grant", "delete_failure":
						w.WriteHeader(400)
						_, _ = io.WriteString(w, `{"error":"invalid_grant"}`)
					case "bad_client":
						w.WriteHeader(401)
						_, _ = io.WriteString(w, `{"error":"invalid_client"}`)
					case "network_failure":
						w.WriteHeader(503)
					default:
						if scenario == "rotate" || scenario == "save_failure" {
							_, _ = io.WriteString(w, `{"access_token":"new-access","refresh_token":"rotated","expires_in":3600,"token_type":"Bearer"}`)
						} else {
							_, _ = io.WriteString(w, `{"access_token":"new-access","expires_in":3600,"token_type":"Bearer"}`)
						}
					}
					return
				}
				w.WriteHeader(204)
			})
			p.store = store
			if scenario == "save_failure" {
				store.saveErr = credentials.ErrLocked
			}
			if scenario == "delete_failure" {
				store.deleteErr = credentials.ErrUnavailable
			}
			p.Poll(context.Background())
			_, e := p.Snapshot()
			if calls != 1 || store.loads != 1 {
				t.Fatal("startup did not perform one saved refresh")
			}
			switch scenario {
			case "retain", "rotate":
				if e != nil || p.AuthorizationStatus().Storage != "saved" || !p.AuthorizationStatus().Authorized {
					t.Fatal("restoration failed")
				}
				want := "refresh-secret"
				if scenario == "rotate" {
					want = "rotated"
				}
				if store.record.RefreshToken != want || !store.record.CanControl {
					t.Fatal("rotation/scope persistence failed")
				}
				// A new instance must consume the newly saved credential, without an access token.
				next, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
					if r.URL.Path == "/token" {
						_ = r.ParseForm()
						if r.Form.Get("refresh_token") != want {
							t.Error("restart used old token")
						}
						_, _ = io.WriteString(w, tokenJSON)
						return
					}
					w.WriteHeader(204)
				})
				next.store = store
				next.Poll(context.Background())
				if _, e := next.Snapshot(); e != nil {
					t.Fatal(e)
				}
			case "invalid_grant":
				if e == nil || e.Code != "not_authorized" || store.record.RefreshToken != "" || store.deletes != 1 {
					t.Fatal("revoked credential retained")
				}
				p.Poll(context.Background())
				if calls != 1 {
					t.Fatal("replayed revoked token")
				}
			case "delete_failure":
				if e == nil || e.Code != "keyring_unavailable" || p.tokens.refresh != "" {
					t.Fatal("revocation cleanup hidden")
				}
				store.deleteErr = nil
				*now = now.Add(31 * time.Second)
				p.Poll(context.Background())
				if store.record.RefreshToken != "" || calls != 1 {
					t.Fatal("cleanup retry refreshed revoked token")
				}
			case "save_failure":
				if e == nil || e.Code != "keyring_locked" || !p.storeDirty || p.tokens.refresh != "rotated" {
					t.Fatal("unsaved rotation lost")
				}
				store.saveErr = nil
				*now = now.Add(31 * time.Second)
				p.Poll(context.Background())
				if store.record.RefreshToken != "rotated" || calls != 1 {
					t.Fatal("save retry repeated refresh or lost rotation")
				}
			default:
				if e == nil || store.deletes != 0 || store.record.RefreshToken != "refresh-secret" {
					t.Fatal("transient/config error destroyed saved login")
				}
			}
		})
	}
}

func TestProtectedStoreReadiness(t *testing.T) {
	for _, err := range []error{credentials.ErrLocked, credentials.ErrUnavailable, credentials.ErrInvalid} {
		t.Run(err.Error(), func(t *testing.T) {
			calls := 0
			p, now := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) {
				calls++
				if r.URL.Path == "/token" {
					_, _ = io.WriteString(w, tokenJSON)
				} else {
					w.WriteHeader(204)
				}
			})
			s := &memoryStore{record: saved(), err: err}
			p.store = s
			p.Poll(context.Background())
			p.Poll(context.Background())
			if calls != 0 || s.loads != 1 {
				t.Fatal("failed store retried without delay or contacted upstream")
			}
			if _, _, e := p.Begin(context.Background()); e == nil {
				t.Fatal("login starts with unusable store")
			}
			s.err = nil
			*now = now.Add(31 * time.Second)
			p.Poll(context.Background())
			if _, e := p.Snapshot(); e != nil || calls != 2 {
				t.Fatal("unlock did not restore automatically")
			}
		})
	}
}

func TestProtectedLoginSavesOnlyRefresh(t *testing.T) {
	p, _ := fakePlayer(t, func(w http.ResponseWriter, r *http.Request) { _, _ = io.WriteString(w, tokenJSON) })
	s := &memoryStore{}
	p.store = s
	_, state, e := p.Begin(context.Background())
	if e != nil {
		t.Fatal(e)
	}
	if e := p.Complete(context.Background(), state, state, "one-use-code", ""); e != nil {
		t.Fatal(e)
	}
	if s.saves != 1 || s.record.RefreshToken != "refresh-secret" || !s.record.CanRead || !s.record.CanControl {
		t.Fatal("login not persisted")
	}
	if p.Complete(context.Background(), state, state, "one-use-code", "") == nil || s.saves != 1 {
		t.Fatal("callback replay")
	}
}

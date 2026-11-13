// notion-auth — the OAuth broker for mdk Notes' "Connect Notion" button.
//
// Notion's OAuth has no PKCE: exchanging an authorization code needs the
// integration's client secret, which a desktop binary cannot keep. This
// sidecar keeps it, and nothing else. It holds no database: a pending login
// lives in memory for at most PendingTTL, and a token is handed over once.
//
//	app   GET  /notion/start?port=P&challenge=C   -> 302 to Notion's consent page
//	Notion GET /notion/cb?code=…&state=…          -> exchanges the code, 302 to
//	                                                 http://127.0.0.1:P/cb?nonce=N
//	app   POST /notion/claim {nonce, verifier}    -> the token JSON, once, if
//	                                                 base64url(sha256(verifier)) == C
//	app   POST /notion/refresh {refresh_token}    -> a refreshed token
//	      GET  /health
//
// The token never appears in a URL or the browser's history: the loopback
// redirect carries only a nonce, and claiming it needs the verifier that
// only the app that started the login knows (PKCE between app and broker).
//
// Env: NOTION_CLIENT_ID, NOTION_CLIENT_SECRET, NOTION_REDIRECT_URI, LISTEN
// (default 127.0.0.1:8840).
package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	PendingTTL   = 5 * time.Minute
	MaxPending   = 1000
	notionAuth   = "https://api.notion.com/v1/oauth/authorize"
	notionToken  = "https://api.notion.com/v1/oauth/token"
	notionVer    = "2022-06-28"
	maxBodyBytes = 16 << 10
)

var version = "dev"

type pending struct {
	port      int
	challenge string
	created   time.Time
	token     []byte // set once the code is exchanged
	errMsg    string
}

type broker struct {
	clientID, secret, redirect string
	http                       *http.Client

	mu      sync.Mutex
	pending map[string]*pending // by nonce (also the OAuth state)
	hits    map[string][]time.Time
}

func main() {
	b := &broker{
		clientID: os.Getenv("NOTION_CLIENT_ID"),
		secret:   os.Getenv("NOTION_CLIENT_SECRET"),
		redirect: os.Getenv("NOTION_REDIRECT_URI"),
		http:     &http.Client{Timeout: 20 * time.Second},
		pending:  map[string]*pending{},
		hits:     map[string][]time.Time{},
	}
	if b.clientID == "" || b.secret == "" || b.redirect == "" {
		log.Fatal("notion-auth: NOTION_CLIENT_ID, NOTION_CLIENT_SECRET and NOTION_REDIRECT_URI are required")
	}
	addr := os.Getenv("LISTEN")
	if addr == "" {
		addr = "127.0.0.1:8840"
	}
	go b.sweep()
	mux := http.NewServeMux()
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"ok": true, "service": "notion-auth", "version": version})
	})
	mux.HandleFunc("/notion/start", b.limit(b.start))
	mux.HandleFunc("/notion/cb", b.limit(b.callback))
	mux.HandleFunc("/notion/claim", b.limit(b.claim))
	mux.HandleFunc("/notion/refresh", b.limit(b.refresh))
	srv := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	log.Printf("notion-auth %s listening on %s (redirect %s)", version, addr, b.redirect)
	log.Fatal(srv.ListenAndServe())
}

// GET /notion/start?port=P&challenge=C
func (b *broker) start(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", 405)
		return
	}
	port, err := strconv.Atoi(r.URL.Query().Get("port"))
	if err != nil || port < 1024 || port > 65535 {
		http.Error(w, "port must be 1024-65535", 400)
		return
	}
	ch := r.URL.Query().Get("challenge")
	if l := len(ch); l < 43 || l > 128 || strings.Trim(ch, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != "" {
		http.Error(w, "challenge must be base64url(sha256(verifier))", 400)
		return
	}
	nonce := randHex(16)
	b.mu.Lock()
	if len(b.pending) >= MaxPending {
		b.mu.Unlock()
		http.Error(w, "too many pending logins, retry shortly", 503)
		return
	}
	b.pending[nonce] = &pending{port: port, challenge: ch, created: time.Now()}
	b.mu.Unlock()
	q := url.Values{
		"client_id":     {b.clientID},
		"response_type": {"code"},
		"owner":         {"user"},
		"redirect_uri":  {b.redirect},
		"state":         {nonce},
	}
	authURL := notionAuth + "?" + q.Encode()
	// Desktop apps can't read a 302's Location header through Machin's
	// http_get, so ?format=json hands them the URL to open directly.
	if r.URL.Query().Get("format") == "json" {
		writeJSON(w, 200, map[string]string{"url": authURL, "nonce": nonce})
		return
	}
	http.Redirect(w, r, authURL, http.StatusFound)
}

// GET /notion/cb?code=…&state=…  (or ?error=… when the user declined)
func (b *broker) callback(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	nonce := q.Get("state")
	b.mu.Lock()
	p := b.pending[nonce]
	b.mu.Unlock()
	if p == nil || time.Since(p.created) > PendingTTL {
		page(w, 400, "This login link expired", "Go back to Notes and click Connect Notion again.")
		return
	}
	if e := q.Get("error"); e != "" {
		b.finish(nonce, nil, e)
		http.Redirect(w, r, loopback(p.port, nonce, e), http.StatusFound)
		return
	}
	code := q.Get("code")
	if code == "" {
		page(w, 400, "Missing authorization code", "Go back to Notes and try again.")
		return
	}
	tok, err := b.exchange(map[string]string{"grant_type": "authorization_code", "code": code, "redirect_uri": b.redirect})
	if err != nil {
		log.Printf("exchange failed: %v", err)
		b.finish(nonce, nil, "exchange_failed")
		http.Redirect(w, r, loopback(p.port, nonce, "exchange_failed"), http.StatusFound)
		return
	}
	b.finish(nonce, tok, "")
	http.Redirect(w, r, loopback(p.port, nonce, ""), http.StatusFound)
}

// POST /notion/claim {"nonce": "...", "verifier": "..."}
func (b *broker) claim(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", 405)
		return
	}
	var in struct{ Nonce, Verifier string }
	if err := json.NewDecoder(io.LimitReader(r.Body, maxBodyBytes)).Decode(&in); err != nil {
		writeJSON(w, 400, map[string]string{"error": "bad json"})
		return
	}
	sum := sha256.Sum256([]byte(in.Verifier))
	want := base64.RawURLEncoding.EncodeToString(sum[:])
	b.mu.Lock()
	p := b.pending[in.Nonce]
	ok := p != nil && time.Since(p.created) <= PendingTTL &&
		subtle.ConstantTimeCompare([]byte(want), []byte(p.challenge)) == 1
	var tok []byte
	var errMsg string
	if ok {
		tok, errMsg = p.token, p.errMsg
		if tok != nil || errMsg != "" {
			delete(b.pending, in.Nonce) // single use
		}
	}
	b.mu.Unlock()
	switch {
	case !ok:
		writeJSON(w, 403, map[string]string{"error": "unknown nonce or wrong verifier"})
	case errMsg != "":
		writeJSON(w, 400, map[string]string{"error": errMsg})
	case tok == nil:
		writeJSON(w, 409, map[string]string{"error": "pending"}) // consent not finished yet
	default:
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(200)
		w.Write(tok)
	}
}

// POST /notion/refresh {"refresh_token": "..."}
func (b *broker) refresh(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", 405)
		return
	}
	var in struct {
		RefreshToken string `json:"refresh_token"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, maxBodyBytes)).Decode(&in); err != nil || in.RefreshToken == "" {
		writeJSON(w, 400, map[string]string{"error": "refresh_token required"})
		return
	}
	tok, err := b.exchange(map[string]string{"grant_type": "refresh_token", "refresh_token": in.RefreshToken})
	if err != nil {
		log.Printf("refresh failed: %v", err)
		writeJSON(w, 502, map[string]string{"error": "refresh_failed"})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.Write(tok)
}

// exchange POSTs to Notion's token endpoint with the client secret.
func (b *broker) exchange(body map[string]string) ([]byte, error) {
	raw, _ := json.Marshal(body)
	req, _ := http.NewRequest(http.MethodPost, notionToken, bytes.NewReader(raw))
	req.SetBasicAuth(b.clientID, b.secret)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Notion-Version", notionVer)
	resp, err := b.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	out, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("notion %d: %s", resp.StatusCode, truncate(out, 300))
	}
	return out, nil
}

func (b *broker) finish(nonce string, tok []byte, errMsg string) {
	b.mu.Lock()
	if p := b.pending[nonce]; p != nil {
		p.token, p.errMsg = tok, errMsg
	}
	b.mu.Unlock()
}

func (b *broker) sweep() {
	for range time.Tick(time.Minute) {
		b.mu.Lock()
		for k, p := range b.pending {
			if time.Since(p.created) > PendingTTL {
				delete(b.pending, k)
			}
		}
		b.hits = map[string][]time.Time{}
		b.mu.Unlock()
	}
}

// limit: 30 requests per minute per client IP (Traefik passes X-Forwarded-For).
func (b *broker) limit(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ip := r.Header.Get("X-Real-Ip")
		if ip == "" {
			ip = strings.TrimSpace(strings.Split(r.Header.Get("X-Forwarded-For"), ",")[0])
		}
		if ip == "" {
			ip, _, _ = net.SplitHostPort(r.RemoteAddr)
		}
		b.mu.Lock()
		now := time.Now()
		hs := b.hits[ip][:0]
		for _, t := range b.hits[ip] {
			if now.Sub(t) < time.Minute {
				hs = append(hs, t)
			}
		}
		hs = append(hs, now)
		b.hits[ip] = hs
		n := len(hs)
		b.mu.Unlock()
		if n > 30 {
			http.Error(w, "rate limited", 429)
			return
		}
		h(w, r)
	}
}

func loopback(port int, nonce, errMsg string) string {
	q := url.Values{"nonce": {nonce}}
	if errMsg != "" {
		q.Set("error", errMsg)
	}
	return fmt.Sprintf("http://127.0.0.1:%d/cb?%s", port, q.Encode())
}

func randHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func page(w http.ResponseWriter, code int, title, msg string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(code)
	fmt.Fprintf(w, `<!doctype html><meta charset="utf-8"><title>%s</title><body style="font:16px system-ui;background:#16161a;color:#ededf2;display:grid;place-items:center;height:100vh;margin:0"><div><h2>%s</h2><p style="color:#9a9aa8">%s</p></div>`, title, title, msg)
}

func truncate(b []byte, n int) string {
	if len(b) > n {
		return string(b[:n]) + "…"
	}
	return string(b)
}

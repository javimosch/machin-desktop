package main

import (
	"crypto/sha256"
	"encoding/base64"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func newTestBroker(notion *httptest.Server) *broker {
	b := &broker{
		clientID: "cid", secret: "sec", redirect: "https://notes-auth.example/notion/cb",
		http: notion.Client(), pending: map[string]*pending{}, hits: map[string][]time.Time{},
	}
	return b
}

func challengeOf(v string) string {
	s := sha256.Sum256([]byte(v))
	return base64.RawURLEncoding.EncodeToString(s[:])
}

// The whole flow against a fake Notion token endpoint.
func TestFlow(t *testing.T) {
	var gotAuth, gotBody string
	notion := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		u, p, _ := r.BasicAuth()
		gotAuth = u + ":" + p
		raw, _ := io.ReadAll(r.Body)
		gotBody = string(raw)
		w.Write([]byte(`{"access_token":"ntn_x","refresh_token":"r","workspace_name":"W"}`))
	}))
	defer notion.Close()
	b := newTestBroker(notion)
	// point exchange at the fake: swap the token URL via a transport rewrite
	b.http.Transport = rewrite{to: notion.URL, base: notion.Client().Transport}

	verifier := strings.Repeat("v", 50)
	rr := httptest.NewRecorder()
	b.start(rr, httptest.NewRequest("GET", "/notion/start?port=7997&challenge="+challengeOf(verifier), nil))
	if rr.Code != 302 {
		t.Fatalf("start: %d %s", rr.Code, rr.Body)
	}
	loc, _ := url.Parse(rr.Header().Get("Location"))
	if loc.Host != "api.notion.com" || loc.Query().Get("client_id") != "cid" || loc.Query().Get("redirect_uri") != b.redirect {
		t.Fatalf("authorize url: %s", loc)
	}
	nonce := loc.Query().Get("state")

	// claim before consent: pending
	rr = httptest.NewRecorder()
	b.claim(rr, httptest.NewRequest("POST", "/notion/claim", strings.NewReader(`{"nonce":"`+nonce+`","verifier":"`+verifier+`"}`)))
	if rr.Code != 409 {
		t.Fatalf("early claim: %d", rr.Code)
	}

	rr = httptest.NewRecorder()
	b.callback(rr, httptest.NewRequest("GET", "/notion/cb?code=abc&state="+nonce, nil))
	if rr.Code != 302 || !strings.HasPrefix(rr.Header().Get("Location"), "http://127.0.0.1:7997/cb?nonce="+nonce) {
		t.Fatalf("callback: %d %s", rr.Code, rr.Header().Get("Location"))
	}
	if strings.Contains(rr.Header().Get("Location"), "ntn_") {
		t.Fatal("token leaked into the loopback URL")
	}
	if gotAuth != "cid:sec" || !strings.Contains(gotBody, `"code":"abc"`) || !strings.Contains(gotBody, `"grant_type":"authorization_code"`) {
		t.Fatalf("exchange: auth=%q body=%q", gotAuth, gotBody)
	}

	// wrong verifier
	rr = httptest.NewRecorder()
	b.claim(rr, httptest.NewRequest("POST", "/notion/claim", strings.NewReader(`{"nonce":"`+nonce+`","verifier":"nope"}`)))
	if rr.Code != 403 {
		t.Fatalf("wrong verifier: %d", rr.Code)
	}
	// right verifier: token, once
	rr = httptest.NewRecorder()
	b.claim(rr, httptest.NewRequest("POST", "/notion/claim", strings.NewReader(`{"nonce":"`+nonce+`","verifier":"`+verifier+`"}`)))
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), "ntn_x") {
		t.Fatalf("claim: %d %s", rr.Code, rr.Body)
	}
	rr = httptest.NewRecorder()
	b.claim(rr, httptest.NewRequest("POST", "/notion/claim", strings.NewReader(`{"nonce":"`+nonce+`","verifier":"`+verifier+`"}`)))
	if rr.Code != 403 {
		t.Fatalf("second claim must fail: %d", rr.Code)
	}
}

func TestStartValidation(t *testing.T) {
	b := &broker{pending: map[string]*pending{}, hits: map[string][]time.Time{}}
	for _, q := range []string{"port=80&challenge=" + challengeOf("x"), "port=7997&challenge=short", "port=abc&challenge=" + challengeOf("x")} {
		rr := httptest.NewRecorder()
		b.start(rr, httptest.NewRequest("GET", "/notion/start?"+q, nil))
		if rr.Code != 400 {
			t.Errorf("%s: want 400, got %d", q, rr.Code)
		}
	}
}

func TestExpiredCallback(t *testing.T) {
	b := &broker{pending: map[string]*pending{"n": {port: 7997, created: time.Now().Add(-time.Hour)}}, hits: map[string][]time.Time{}}
	rr := httptest.NewRecorder()
	b.callback(rr, httptest.NewRequest("GET", "/notion/cb?code=c&state=n", nil))
	if rr.Code != 400 {
		t.Fatalf("expired: %d", rr.Code)
	}
}

type rewrite struct {
	to   string
	base http.RoundTripper
}

func (r rewrite) RoundTrip(req *http.Request) (*http.Response, error) {
	u, _ := url.Parse(r.to)
	req.URL.Scheme, req.URL.Host = u.Scheme, u.Host
	return r.base.RoundTrip(req)
}

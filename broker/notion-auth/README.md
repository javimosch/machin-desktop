# notion-auth

OAuth broker for mdk Notes' **Connect Notion** button. Notion's OAuth has no
PKCE — exchanging an authorization code requires the integration's client
secret, which a desktop binary can never keep (an AppImage is extractable). This
sidecar holds the secret and nothing else: pending logins live in memory for at
most 5 minutes, and a token is handed over exactly once.

Deployed on dk1 as `notes-auth.intrane.fr` → Traefik → `127.0.0.1:8840`.

## Flow

```
app    GET  /notion/start?port=P&challenge=C   → 302 Notion consent page
       (challenge = base64url(sha256(verifier)); verifier never leaves the app)
user   picks pages in Notion
Notion GET  /notion/cb?code=…&state=…          → exchanges code with client
                                                  secret, 302 to
                                                  http://127.0.0.1:P/cb?nonce=N
                                                  (token never in a URL)
app    POST /notion/claim {nonce, verifier}    → token JSON, once, only if
                                                  base64url(sha256(verifier))
                                                  matches the stored challenge
app    POST /notion/refresh {refresh_token}    → refreshed token
       GET  /health
```

`state` and `nonce` are the same value; the port is bound to the flow at
`/notion/start`, so a stolen nonce can't redirect a victim's browser anywhere
but the port the flow was started for. Pending entries expire after 5 minutes
and are swept every minute. Each endpoint is rate-limited to 30 req/min/IP.

## Deploy (dk1)

```bash
# build (pure Go, ~5 MB stripped)
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath \
  -ldflags "-s -w -X main.version=$(git rev-parse --short HEAD)" -o notion-auth .

# env — secret comes from the owner's backup, never the repo
#   NOTION_CLIENT_ID, NOTION_CLIENT_SECRET, NOTION_REDIRECT_URI
#   LISTEN=127.0.0.1:8840

scp notion-auth dk1:/tmp/ && ssh dk1 '
  sudo install -d -m 755 /opt/notion-auth
  sudo install -m 755 /tmp/notion-auth /opt/notion-auth/notion-auth
  sudo install -d -m 700 /etc/notion-auth
  # write NOTION_* lines (NOT AUTHORIZE_URL) to /etc/notion-auth/notion-auth.env, mode 600
  sudo install -m 644 deploy/notion-auth.service /etc/systemd/system/
  sudo systemctl daemon-reload && sudo systemctl enable --now notion-auth
  sudo install -m 644 deploy/36-notes-auth.yml /etc/traefik/dynamic.d/
'   # Traefik watches dynamic.d — no restart needed; LE cert issues in ~30 s

# DNS: A record notes-auth.intrane.fr → 92.113.145.178, proxied=false
```

Rollback: `sudo systemctl disable --now notion-auth` and remove
`36-notes-auth.yml` from `/etc/traefik/dynamic.d/`.

## Tests

`go test ./...` covers the full flow against a fake Notion token endpoint:
challenge binding, pending/403/409 claim states, single-use token, no token in
the loopback URL, and start/callback validation.

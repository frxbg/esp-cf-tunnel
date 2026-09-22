module esp-cf-tunnel/rpc-reference

go 1.26

require (
	github.com/cloudflare/cloudflared v0.0.0
	zombiezen.com/go/capnproto2 v2.18.0+incompatible
)

require (
	github.com/philhofer/fwd v1.2.0 // indirect
	golang.org/x/net v0.56.0 // indirect
)

replace github.com/cloudflare/cloudflared => ../../.cache/cloudflared

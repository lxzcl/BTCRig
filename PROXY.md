# BTCRig Proxy

[简体中文](PROXY.zh-CN.md)

`btc_proxy` is a transparent Stratum forwarding proxy. Each client connection gets its own upstream pool connection.

Run:

```bash
./build/btc_proxy
```

Default configuration:

```text
listen: 0.0.0.0:4333
mode: auto
upstream: stratum+tls://public-pool.io:14333
upstream verify: true
```

The same listener can auto-detect plain TCP and TLS clients:

```bash
./build/btc_stratum -o stratum+tcp://proxy-host:4333
./build/btc_stratum -o stratum+tls://proxy-host:4333
```

## Automatic Certificate

If no certificate is configured, the proxy generates a self-signed certificate in the current working directory:

```text
cert.pem
cert_key.pem
```

You can also set a persistent data directory:

```bash
./build/btc_proxy --data-dir /var/lib/btc-proxy
```

Or use an environment variable:

```bash
export BTC_PROXY_DATA_DIR=/var/lib/btc-proxy
./build/btc_proxy
```

## Trusted Certificate

For public deployments, use a certificate signed by a trusted CA:

```bash
./build/btc_proxy \
  --listen-auto 0.0.0.0:4333 \
  --cert /etc/letsencrypt/live/example.com/fullchain.pem \
  --key /etc/letsencrypt/live/example.com/privkey.pem \
  --upstream stratum+tls://public-pool.io:14333
```

## proxy.json Example

```json
{
  "listen": {
    "mode": "auto",
    "host": "0.0.0.0",
    "port": 4333
  },
  "upstream": {
    "url": "stratum+tls://public-pool.io:14333",
    "verify": true
  }
}
```

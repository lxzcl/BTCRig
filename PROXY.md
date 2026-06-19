# BTCRig Proxy

`btc_proxy` 是透明 Stratum 转发代理。每个客户端连接都会建立一个独立的上游矿池连接。

默认运行：

```bash
./build/btc_proxy
```

默认配置：

```text
listen: 0.0.0.0:4333
mode: auto
upstream: stratum+tls://public-pool.io:4333
upstream verify: true
```

同一个监听端口支持自动识别客户端 TCP 和 TLS：

```bash
./build/btc_stratum -o stratum+tcp://proxy-host:4333
./build/btc_stratum -o stratum+tls://proxy-host:4333
```

## 自动证书

代理没有配置证书时，会在当前工作目录自动生成：

```text
cert.pem
cert_key.pem
```

也可以指定持久目录：

```bash
./build/btc_proxy --data-dir /var/lib/btc-proxy
```

或使用环境变量：

```bash
export BTC_PROXY_DATA_DIR=/var/lib/btc-proxy
./build/btc_proxy
```

## 使用可信证书

公开部署时可以使用 CA 签发的证书：

```bash
./build/btc_proxy \
  --listen-auto 0.0.0.0:4333 \
  --cert /etc/letsencrypt/live/example.com/fullchain.pem \
  --key /etc/letsencrypt/live/example.com/privkey.pem \
  --upstream stratum+tls://public-pool.io:4333
```

## proxy.json 示例

```json
{
  "listen": {
    "mode": "auto",
    "host": "0.0.0.0",
    "port": 4333
  },
  "upstream": {
    "url": "stratum+tls://public-pool.io:4333",
    "verify": true
  }
}
```

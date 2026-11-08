<p align="right"><a href="README.zh_CN.md">简体中文</a></p>

# Scoped HTTP transmit-buffer lifetime

Vendored from ESP-IDF v5.5.3 under Apache-2.0; upstream source hashes are in UPSTREAM.json. The system SDK is unchanged.

Only fresh redirect-disabled GET clients explicitly calling esp_http_client_defer_get_tx_buffer opt in. Request scratch is acquired after TLS handshake and wiped/released after complete zero-body headers. Partial headers, request bodies and ordinary POST clients retain their buffers. Public configuration layout is unchanged.

See the paired Chinese README and the host lifetime test plus native read/cancel/pressure qualification.

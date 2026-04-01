# URL Shortener (C++17)

Dependency-free HTTP URL shortener using POSIX sockets. Linux/macOS.

## Build & run
    cmake -B build && cmake --build build
    ./build/shortener [port=8080] [dbfile=links.db]
    # optional: BASE_URL=https://sho.rt ./build/shortener

## API
    # raw URL as the body (easiest from curl; & and ? are safe)
    curl -d "https://example.com/long/path?a=1&b=2" localhost:8080/shorten
    curl -d "https://example.com" "localhost:8080/shorten?alias=my-link"
    # or form-encoded
    curl --data-urlencode "url=https://example.com/p?a=1&b=2" -d "alias=my-link" localhost:8080/shorten
    curl -i localhost:8080/<code>        # 302 redirect
    curl localhost:8080/stats/<code>     # {"url":...,"hits":N}
Open http://localhost:8080/ for a web form.

## Design
- `src/shortener.hpp`: thread-safe store, random 7-char base62 codes, same URL -> same code, custom aliases, atomic file persistence (tmp + rename).
- `src/main.cpp`: thread-per-connection HTTP/1.1 server and routing.
- 302 (not 301) so browsers don't cache redirects and every click is counted.

## Next steps
SQLite storage, rate limiting, thread pool / epoll, TTL expiry, HTTPS via a reverse proxy.

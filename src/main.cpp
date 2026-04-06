#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

#include "shortener.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

const char* kIndexHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>URL Shortener</title>
<style>
body{font-family:system-ui,sans-serif;max-width:520px;margin:10vh auto;padding:0 1rem}
input,button{font:inherit;padding:.6rem;margin:.25rem 0;width:100%;box-sizing:border-box}
button{cursor:pointer}#out{margin-top:1rem;word-break:break-all}
</style></head><body>
<h1>URL Shortener</h1>
<input id="url" placeholder="https://example.com/very/long/link">
<input id="alias" placeholder="custom alias (optional)">
<button onclick="go()">Shorten</button>
<div id="out"></div>
<script>
async function go(){
  const body = new URLSearchParams({url:url.value, alias:alias.value});
  const r = await fetch('/shorten',{method:'POST',body});
  const j = await r.json();
  out.innerHTML = r.ok ? '<a href="'+j.short+'">'+j.short+'</a>' : 'Error: '+j.error;
}
</script></body></html>)HTML";

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                   std::isxdigit((unsigned char)s[i + 2])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parseForm(const std::string& body) {
    std::map<std::string, std::string> kv;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        kv[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    return kv;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

void respond(int fd, int status, const char* reason, const char* ctype, const std::string& body,
             const std::string& extraHeaders = "") {
    std::ostringstream r;
    r << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << extraHeaders << "\r\n"
      << body;
    const std::string data = r.str();
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

void json(int fd, int status, const char* reason, const std::string& body) {
    respond(fd, status, reason, "application/json", body);
}

void handle(int fd, Shortener& store, const std::string& base) {
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    std::string req;
    char buf[2048];
    size_t headEnd;
    while ((headEnd = req.find("\r\n\r\n")) == std::string::npos) {
        if (req.size() > 16384) { json(fd, 431, "Request Header Fields Too Large", "{\"error\":\"headers too large\"}"); close(fd); return; }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { close(fd); return; }
        req.append(buf, static_cast<size_t>(n));
    }

    std::istringstream head(req.substr(0, headEnd));
    std::string method, target, version, line;
    head >> method >> target >> version;
    std::getline(head, line);  // rest of request line

    size_t contentLength = 0;
    while (std::getline(head, line)) {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.rfind("content-length:", 0) == 0)
            contentLength = std::strtoul(lower.c_str() + 15, nullptr, 10);
    }
    if (contentLength > 8192) { json(fd, 413, "Payload Too Large", "{\"error\":\"body too large\"}"); close(fd); return; }

    const size_t bodyStart = headEnd + 4;
    while (req.size() - bodyStart < contentLength) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { close(fd); return; }
        req.append(buf, static_cast<size_t>(n));
    }
    const std::string body = req.substr(bodyStart, contentLength);

    std::string query;
    if (size_t q = target.find('?'); q != std::string::npos) {
        query = target.substr(q + 1);
        target.resize(q);
    }

    if (method == "GET" && target == "/") {
        respond(fd, 200, "OK", "text/html; charset=utf-8", kIndexHtml);

    } else if (method == "POST" && target == "/shorten") {
        // Either a form body (url=...&alias=..., percent-encoded) or the raw URL as the body
        // with an optional ?alias=name query string.
        std::string url, alias;
        if (body.rfind("http://", 0) == 0 || body.rfind("https://", 0) == 0) {
            url = body;
            alias = parseForm(query)["alias"];
        } else {
            auto form = parseForm(body);
            url = form["url"];
            alias = form["alias"];
        }
        std::string err;
        std::string code = store.create(url, alias, err);
        if (code.empty())
            json(fd, 400, "Bad Request", "{\"error\":\"" + jsonEscape(err) + "\"}");
        else
            json(fd, 201, "Created", "{\"code\":\"" + code + "\",\"short\":\"" + base + "/" + code + "\"}");

    } else if (method == "GET" && target.rfind("/stats/", 0) == 0) {
        if (auto s = store.stats(target.substr(7)))
            json(fd, 200, "OK", "{\"url\":\"" + jsonEscape(s->url) + "\",\"hits\":" + std::to_string(s->hits) + "}");
        else
            json(fd, 404, "Not Found", "{\"error\":\"unknown code\"}");

    } else if (method == "GET" && target.size() > 1 && target.find('/', 1) == std::string::npos) {
        if (auto url = store.resolve(target.substr(1)))
            respond(fd, 302, "Found", "text/plain", "", "Location: " + *url + "\r\n");  // 302 so every click is counted
        else
            json(fd, 404, "Not Found", "{\"error\":\"unknown code\"}");

    } else {
        json(fd, 404, "Not Found", "{\"error\":\"not found\"}");
    }
    close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const std::string dbPath = argc > 2 ? argv[2] : "links.db";
    const char* envBase = std::getenv("BASE_URL");
    const std::string base = envBase ? envBase : "http://localhost:" + std::to_string(port);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = onSignal;  // no SA_RESTART: accept() returns EINTR so we can exit cleanly
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Shortener store(dbPath);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(server, 128) < 0) {
        perror("bind/listen");
        return 1;
    }
    std::cout << "Listening on " << base << " (db: " << dbPath << ")\n";

    while (!g_stop) {
        int fd = accept(server, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread(handle, fd, std::ref(store), std::cref(base)).detach();
    }

    std::cout << "\nShutting down, saving...\n";
    store.flush();
    close(server);
    return 0;
}

#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>

struct Link {
    std::string url;
    unsigned long hits = 0;
};

// Thread-safe in-memory link store, persisted to a tab-separated file.
class Shortener {
private:
    std::string path_;
    std::mutex mu_;
    std::mt19937_64 rng_;
    std::unordered_map<std::string, Link> links_;
    std::unordered_map<std::string, std::string> byUrl_;
    int unsaved_ = 0;

    static bool validUrl(const std::string& u) {
        if (u.size() > 2048) return false;
        if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return false;
        if (u.size() <= 8) return false;
        for (unsigned char c : u)
            if (c <= 0x20 || c == 0x7f) return false;
        return true;
    }

    static bool validAlias(const std::string& a) {
        if (a.size() < 3 || a.size() > 32) return false;
        if (a == "stats" || a == "shorten") return false;
        for (unsigned char c : a)
            if (!std::isalnum(c) && c != '-' && c != '_') return false;
        return true;
    }

    std::string randomCode(size_t len) {
        static const char* kDigits =
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::uniform_int_distribution<int> pick(0, 61);
        std::string s;
        for (size_t i = 0; i < len; ++i) s += kDigits[pick(rng_)];
        return s;
    }

    void add(const std::string& code, const std::string& url) {
        links_[code] = Link{url, 0};
        byUrl_.emplace(url, code);
        save();
    }

    void load() {
        std::ifstream in(path_);
        std::string line;
        while (std::getline(in, line)) {
            size_t t1 = line.find('\t');
            size_t t2 = (t1 == std::string::npos) ? std::string::npos : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::string code = line.substr(0, t1);
            unsigned long hits = std::strtoul(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            std::string url = line.substr(t2 + 1);
            links_[code] = Link{url, hits};
            byUrl_.emplace(url, code);
        }
    }

    // Caller must hold mu_. Writes to a temp file then renames, so a crash can't corrupt the db.
    void save() {
        const std::string tmp = path_ + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            for (const auto& [code, link] : links_)
                out << code << '\t' << link.hits << '\t' << link.url << '\n';
        }
        std::rename(tmp.c_str(), path_.c_str());
        unsaved_ = 0;
    }

public:
    explicit Shortener(std::string path) : path_(std::move(path)), rng_(std::random_device{}()) {
        load();
    }

    // Returns the short code, or "" and sets err.
    std::string create(const std::string& url, const std::string& alias, std::string& err) {
        if (!validUrl(url)) {
            err = "URL must start with http:// or https:// and contain no spaces or control characters (max 2048 chars)";
            return "";
        }
        std::lock_guard<std::mutex> lock(mu_);

        if (!alias.empty()) {
            if (!validAlias(alias)) {
                err = "alias must be 3-32 chars of letters, digits, '-' or '_' (and not a reserved word)";
                return "";
            }
            if (links_.count(alias)) {
                err = "alias already taken";
                return "";
            }
            add(alias, url);
            return alias;
        }

        auto existing = byUrl_.find(url);  // same URL -> same code
        if (existing != byUrl_.end()) return existing->second;

        std::string code;
        do {
            code = randomCode(7);
        } while (links_.count(code));
        add(code, url);
        return code;
    }

    // Looks up a code and counts the hit.
    std::optional<std::string> resolve(const std::string& code) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = links_.find(code);
        if (it == links_.end()) return std::nullopt;
        ++it->second.hits;
        if (++unsaved_ >= 10) save();  // batch hit-count writes
        return it->second.url;
    }

    std::optional<Link> stats(const std::string& code) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = links_.find(code);
        if (it == links_.end()) return std::nullopt;
        return it->second;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mu_);
        save();
    }
};

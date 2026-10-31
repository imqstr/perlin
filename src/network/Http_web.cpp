#include "network/Http.hpp"

#include <emscripten/fetch.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace {
    void IgnoreFetch(emscripten_fetch_t*) {}
}

namespace perlin::http {
    namespace {
    Response Fetch(const std::string& url, const std::string& method, const std::string& body,
                   const std::string& contentType, const std::map<std::string, std::string>& extra,
                   int timeoutSecs) {

        emscripten_fetch_attr_t attr;
        emscripten_fetch_attr_init(&attr);
        std::strncpy(attr.requestMethod, method.c_str(), sizeof(attr.requestMethod) - 1);
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;
        attr.onsuccess = IgnoreFetch;
        attr.onerror = IgnoreFetch;
        attr.onprogress = IgnoreFetch;
        attr.onreadystatechange = IgnoreFetch;
        attr.timeoutMSecs = static_cast<unsigned long>(timeoutSecs) * 1000UL;

        std::vector<std::string> headerStrings;
        if (!contentType.empty()) {
            headerStrings.push_back("Content-Type");
            headerStrings.push_back(contentType);
        }
        for (const auto& [k, v] : extra) {
            headerStrings.push_back(k);
            headerStrings.push_back(v);
        }
        std::vector<const char*> headerPointers;
        for (const std::string& h : headerStrings)
            headerPointers.push_back(h.c_str());
        headerPointers.push_back(nullptr);
        attr.requestHeaders = headerPointers.data();

        if (method != "GET" && method != "HEAD") {
            attr.requestData = body.data();
            attr.requestDataSize = body.size();
        }

        emscripten_fetch_t* fetch = emscripten_fetch(&attr, url.c_str());
        if (!fetch)
            return {};
        Response out;
        out.status = fetch->status;
        if (fetch->data && fetch->numBytes > 0)
            out.body.assign(fetch->data, static_cast<size_t>(fetch->numBytes));

        const size_t headerBytes = emscripten_fetch_get_response_headers_length(fetch);
        if (headerBytes > 0) {
            std::string raw(headerBytes + 1, '\0');
            emscripten_fetch_get_response_headers(fetch, raw.data(), raw.size());
            size_t at = 0;
            while (at < raw.size() && raw[at] != '\0') {
                size_t end = raw.find("\r\n", at);
                if (end == std::string::npos)
                    end = raw.find('\0', at);
                if (end == std::string::npos)
                    end = raw.size();
                const std::string line = raw.substr(at, end - at);
                const size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string key = line.substr(0, colon);
                    for (char& c : key)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    size_t v = colon + 1;
                    while (v < line.size() && line[v] == ' ')
                        v++;
                    out.headers.emplace(std::move(key), line.substr(v));
                }
                at = end + 2;
            }
        }
        emscripten_fetch_close(fetch);
        return out;
    }
    }

    Response Request(const std::string& host, uint16_t port, bool tls, const std::string& method,
                     const std::string& path, const std::string& body, const std::string& contentType,
                     const std::map<std::string, std::string>& extra, int timeoutSecs) {
        if (host.empty() && (path.starts_with("http://") || path.starts_with("https://")))
            return Fetch(path, method, body, contentType, extra, timeoutSecs);
        const std::string url = std::string(tls ? "https://" : "http://") + host + ":" + std::to_string(port) + path;
        Response direct = Fetch(url, method, body, contentType, extra, timeoutSecs);
        if (direct.status != 0 || tls)
            return direct;
        Response relayed = Fetch("/__relay/" + host + "/" + std::to_string(port) + path, method, body, contentType,
                                 extra, timeoutSecs);
        if (relayed.status != 0 && relayed.status != 404 && relayed.status != 501)
            return relayed;
        return direct;
    }

    Response GetUrl(const std::string& url, const std::map<std::string, std::string>& headers, int timeoutSecs) {
        return Fetch(url, "GET", {}, {}, headers, timeoutSecs);
    }
}

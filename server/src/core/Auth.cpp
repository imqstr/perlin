#include "core/Auth.hpp"

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <chrono>
#include <memory>
#include <sstream>

namespace perlin::server::auth {
    namespace {
        struct PkeyFree { void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); } };
        using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;

        Pkey LoadKey(const std::string& b64) {
            const std::string der = json::Base64UrlDecode(b64);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
            return Pkey(d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size())));
        }

        std::string B64UrlEncode(const std::string& s) {
            static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string out;
            for (size_t i = 0; i < s.size(); i += 3) {
                uint32_t v = static_cast<unsigned char>(s[i]) << 16;
                if (i + 1 < s.size()) v |= static_cast<unsigned char>(s[i + 1]) << 8;
                if (i + 2 < s.size()) v |= static_cast<unsigned char>(s[i + 2]);
                out += t[(v >> 18) & 63];
                out += t[(v >> 12) & 63];
                if (i + 1 < s.size()) out += t[(v >> 6) & 63];
                if (i + 2 < s.size()) out += t[v & 63];
            }
            return out;
        }

        int64_t Now() {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        struct Jwt {
            std::string signingInput, signature, x5u;
            json::Value payload;
        };

        bool Split(const std::string& token, Jwt& out) {
            const size_t a = token.find('.');
            const size_t b = a == std::string::npos ? a : token.find('.', a + 1);
            if (b == std::string::npos)
                return false;
            json::Value header;
            if (!json::Parse(json::Base64UrlDecode(token.substr(0, a)), header))
                return false;
            if (header["alg"].String() != "ES384")
                return false;
            out.x5u = header["x5u"].String();
            if (!json::Parse(json::Base64UrlDecode(token.substr(a + 1, b - a - 1)), out.payload))
                return false;
            out.signingInput = token.substr(0, b);
            out.signature = json::Base64UrlDecode(token.substr(b + 1));
            return !out.x5u.empty();
        }

        // exp and nbf with a few minutes of clock slack; absent means no limit
        bool InTime(const json::Value& payload) {
            const int64_t now = Now();
            constexpr int64_t kSlack = 300;
            if (payload["nbf"].kind == json::Value::Kind::Number && now + kSlack < payload["nbf"].Int())
                return false;
            if (payload["exp"].kind == json::Value::Kind::Number && now - kSlack > payload["exp"].Int())
                return false;
            return true;
        }
    }

    const std::vector<std::string>& DefaultRootKeys() {
        static const std::vector<std::string> keys = {
            // the current root, and the one used before 2020 (old clients and some relays still chain to it)
            "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAECRXueJeTDqNRRgJi/vlRufByu/2G0i2Ebt6YMar5QX/R0DIIyrJMcUpruK4QveTfJSTp3Shlq4Gk34cD/4GUWwkv0DVuzeuB+tXija7HBxii03NHDbPAD0AKnLr2wdAp",
            "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE8ELkixyLcwlZryUQcu1TvPOmI2B7vX83ndnWRUaXm74wFfa5f/lwQNTfrLVHa2PmenpGI6JhIMUJaWZrjmMj90NoKNFSNBuKdm8rYiXsfaz3K36x/1U26HpG0ZxK/V1V"};
        return keys;
    }

    bool VerifyEs384(const std::string& signingInput, const std::string& raw, const std::string& publicKeyB64) {
        if (raw.size() != 96)
            return false;
        const Pkey key = LoadKey(publicKeyB64);
        if (!key)
            return false;
        // jws carries r and s as two fixed width integers, openssl wants the der form
        ECDSA_SIG* sig = ECDSA_SIG_new();
        BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), 48, nullptr);
        BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()) + 48, 48, nullptr);
        if (!sig || !r || !s || ECDSA_SIG_set0(sig, r, s) != 1) {
            ECDSA_SIG_free(sig);
            BN_free(r);
            BN_free(s);
            return false;
        }
        unsigned char* der = nullptr;
        const int derLen = i2d_ECDSA_SIG(sig, &der);
        ECDSA_SIG_free(sig);
        if (derLen <= 0)
            return false;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha384(), nullptr, key.get()) == 1 &&
                  EVP_DigestVerifyUpdate(ctx, signingInput.data(), signingInput.size()) == 1 &&
                  EVP_DigestVerifyFinal(ctx, der, static_cast<size_t>(derLen)) == 1;
        EVP_MD_CTX_free(ctx);
        OPENSSL_free(der);
        return ok;
    }

    Login VerifyLogin(const std::string& identityJson, const std::string& clientToken, const std::vector<std::string>& rootKeys) {
        Login out;
        json::Value identity;
        if (!json::Parse(identityJson, identity)) {
            out.error = "the identity is not json";
            return out;
        }
        std::vector<std::string> chain;
        const json::Value* source = &identity;
        json::Value inner;
        if (identity["Certificate"].IsString()) {
            if (!json::Parse(identity["Certificate"].text, inner)) {
                out.error = "the certificate is not json";
                return out;
            }
            source = &inner;
        }
        for (const json::Value& t : (*source)["chain"].items)
            chain.push_back(t.String());
        if (chain.empty() || chain.size() > 8) {
            out.error = "no certificate chain";
            return out;
        }

        struct Identity {
            std::string name, xuid, uuid;
            bool found = false;
        };
        Identity claimed, verified;
        std::string expectedKey;
        bool rooted = false;
        for (size_t i = 0; i < chain.size(); i++) {
            Jwt jwt;
            if (!Split(chain[i], jwt)) {
                out.error = "malformed certificate link " + std::to_string(i);
                return out;
            }
            // the first link vouches for itself; every other link must be signed by the key the one before named
            if (i > 0 && jwt.x5u != expectedKey) {
                out.error = "certificate link " + std::to_string(i) + " is not signed by the key the previous link named";
                return out;
            }
            if (!VerifyEs384(jwt.signingInput, jwt.signature, jwt.x5u) || !InTime(jwt.payload)) {
                out.error = "certificate link " + std::to_string(i) + " has a bad signature or is out of date";
                return out;
            }
            for (const std::string& root : rootKeys)
                if (jwt.x5u == root)
                    rooted = true;
            // a link before the root's signature can say anything; only later ones are the service's word
            const json::Value& extra = jwt.payload["extraData"];
            if (extra.IsObject()) {
                Identity& dst = rooted ? verified : claimed;
                dst.name = extra["displayName"].String();
                dst.xuid = extra["XUID"].String();
                dst.uuid = extra["identity"].String();
                dst.found = true;
            }
            expectedKey = jwt.payload["identityPublicKey"].String();
            if (expectedKey.empty()) {
                out.error = "certificate link " + std::to_string(i) + " names no key";
                return out;
            }
        }
        out.chainValid = true;
        out.clientKey = expectedKey;
        if (rooted && verified.found && !verified.xuid.empty()) {
            out.xboxVerified = true;
            out.name = verified.name;
            out.xuid = verified.xuid;
            out.uuid = verified.uuid;
        } else {
            // an unrooted chain carries any name it likes: usable as a label, never as an identity, so no xuid
            out.name = claimed.name.empty() ? verified.name : claimed.name;
            out.uuid = claimed.uuid;
        }

        Jwt client;
        if (Split(clientToken, client) && client.x5u == out.clientKey && VerifyEs384(client.signingInput, client.signature, out.clientKey) &&
            InTime(client.payload)) {
            out.clientData = client.payload;
            out.clientDataValid = true;
        } else {
            out.chainValid = false;
            out.xboxVerified = false;
            out.xuid.clear();
            out.error = "the client data is not signed by the key in the certificate";
        }
        return out;
    }

    bool VerifyOfferBinding(const std::string& offerSdp, const std::string& clientKey, std::string& error) {
        // a=fingerprint:<algorithm> <digest>, the same text the client signed
        std::string payload = "{\"fingerprint\":[";
        bool first = true;
        std::istringstream in(offerSdp);
        std::string line;
        std::string assertionAttr;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("a=fingerprint:", 0) == 0) {
                const std::string rest = line.substr(14);
                const size_t sp = rest.find(' ');
                if (sp == std::string::npos)
                    continue;
                payload += std::string(first ? "" : ",") + "{\"algorithm\":\"" + json::Escape(rest.substr(0, sp)) + "\",\"digest\":\"" +
                           json::Escape(rest.substr(sp + 1)) + "\"}";
                first = false;
            } else if (line.rfind("a=identity:", 0) == 0 && assertionAttr.empty()) {
                assertionAttr = line.substr(11);
            }
        }
        payload += "]}";
        if (first) {
            error = "the offer has no dtls fingerprint";
            return false;
        }
        if (assertionAttr.empty()) {
            error = "the offer carries no identity assertion";
            return false;
        }
        json::Value envelope, assertion;
        if (!json::Parse(json::Base64UrlDecode(assertionAttr), envelope) || !json::Parse(envelope["assertion"].String(), assertion)) {
            error = "the identity assertion is malformed";
            return false;
        }
        // compact jws with the payload left out: header..signature
        const std::string fps = assertion["fingerprints"].String();
        const size_t gap = fps.find("..");
        if (gap == std::string::npos) {
            error = "the fingerprint signature is malformed";
            return false;
        }
        const std::string signingInput = fps.substr(0, gap) + "." + B64UrlEncode(payload);
        if (!VerifyEs384(signingInput, json::Base64UrlDecode(fps.substr(gap + 2)), clientKey)) {
            error = "the offer's fingerprints are not signed by the player's key";
            return false;
        }
        return true;
    }
}

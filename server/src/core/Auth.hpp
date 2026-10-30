#pragma once
#include "core/Json.hpp"

#include <string>
#include <vector>

namespace perlin::server::auth {

    // public keys (base64 der) whose signature makes a login chain an Xbox Live one
    const std::vector<std::string>& DefaultRootKeys();

    struct Login {
        // every signature in the chain checks out and each link names the next key
        bool chainValid = false;
        // some link in the chain was signed by a trusted root
        bool xboxVerified = false;
        // from the links after the root signature when verified, otherwise only claimed
        std::string name, xuid, uuid;
        // the key the player signs with (base64 der): their client data and, over nethernet, their offer
        std::string clientKey;
        json::Value clientData;
        bool clientDataValid = false;
        std::string error;
    };

    // `identity` is the login packet's identity json {Certificate, AuthenticationType, Token}, `clientToken` the client data jwt
    Login VerifyLogin(const std::string& identity, const std::string& clientToken, const std::vector<std::string>& rootKeys);

    // nethernet binds the account to the data channel: the offer carries an a=identity assertion whose signature covers the
    // dtls fingerprints. true when it is signed by `clientKey` and covers exactly the fingerprints in `offerSdp`
    bool VerifyOfferBinding(const std::string& offerSdp, const std::string& clientKey, std::string& error);

    // exposed for tests
    bool VerifyEs384(const std::string& signingInput, const std::string& rawSignature, const std::string& publicKeyB64);
}

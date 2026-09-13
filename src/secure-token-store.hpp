// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace multistream {

class SecureTokenStore final {
public:
    static bool store_facebook_token(const std::string &token, std::string &error);
    static bool load_facebook_token(std::string &token, std::string &error);
    static bool remove_facebook_token(std::string &error);
    static bool store_twitch_token(const std::string &token, std::string &error);
    static bool load_twitch_token(std::string &token, std::string &error);
    static bool remove_twitch_token(std::string &error);
    static bool store_youtube_token(const std::string &token, std::string &error);
    static bool load_youtube_token(std::string &token, std::string &error);
    static bool remove_youtube_token(std::string &error);
    // The Desktop OAuth client credential is optional and is only used for
    // local configurations whose Google token endpoint requires it.
    static bool store_youtube_client_secret(const std::string &secret, std::string &error);
    static bool load_youtube_client_secret(std::string &secret, std::string &error);
    static bool remove_youtube_client_secret(std::string &error);
};

} // namespace multistream

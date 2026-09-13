// SPDX-License-Identifier: GPL-2.0-or-later

#include "secure-token-store.hpp"

#if defined(__linux__)
#include <libsecret/secret.h>
#endif

namespace multistream {
#if defined(__linux__)
namespace {

constexpr const char *kAccountAttribute = "account";
constexpr const char *kAccountValue = "default";
constexpr const char *kFacebookSchemaName = "org.penta.multistream.facebook";
constexpr const char *kTwitchSchemaName = "org.penta.multistream.twitch";
constexpr const char *kYouTubeSchemaName = "org.penta.multistream.youtube";
constexpr const char *kYouTubeClientSecretSchemaName = "org.penta.multistream.youtube-client-secret";
constexpr const char *kFacebookLabel = "Penta Multistream Facebook session";
constexpr const char *kTwitchLabel = "Penta Multistream Twitch session";
constexpr const char *kYouTubeLabel = "Penta Multistream YouTube session";
constexpr const char *kYouTubeClientSecretLabel = "Penta Multistream YouTube OAuth client credential";

const SecretSchema make_schema(const char *name)
{
    SecretSchema schema{};
    schema.name = name;
    schema.flags = SECRET_SCHEMA_NONE;
    schema.attributes[0] = {kAccountAttribute, SECRET_SCHEMA_ATTRIBUTE_STRING};
    return schema;
}

const SecretSchema kFacebookSchema = make_schema(kFacebookSchemaName);
const SecretSchema kTwitchSchema = make_schema(kTwitchSchemaName);
const SecretSchema kYouTubeSchema = make_schema(kYouTubeSchemaName);
const SecretSchema kYouTubeClientSecretSchema = make_schema(kYouTubeClientSecretSchemaName);

std::string consume_error(GError *error, const char *fallback)
{
    if (!error)
        return fallback;
    const std::string message = error->message ? error->message : fallback;
    g_error_free(error);
    return message;
}

bool store_token(const SecretSchema *schema, const char *label, const char *provider,
                 const std::string &token, std::string &error)
{
    error.clear();
    if (token.empty()) {
        error = std::string(provider) + " token is empty";
        return false;
    }
    GError *storage_error = nullptr;
    const gboolean stored = secret_password_store_sync(
        schema, SECRET_COLLECTION_DEFAULT, label, token.c_str(), nullptr, &storage_error,
        kAccountAttribute, kAccountValue, nullptr);
    if (!stored) {
        error = consume_error(storage_error, "Could not store the token securely");
        return false;
    }
    return true;
}

bool load_token(const SecretSchema *schema, const char *provider, std::string &token, std::string &error)
{
    token.clear();
    error.clear();
    GError *storage_error = nullptr;
    gchar *stored = secret_password_lookup_sync(schema, nullptr, &storage_error,
                                                kAccountAttribute, kAccountValue, nullptr);
    if (storage_error) {
        error = consume_error(storage_error, "Could not read the token securely");
        return false;
    }
    if (stored) {
        token = stored;
        secret_password_free(stored);
    }
    (void)provider;
    return true;
}

bool remove_token(const SecretSchema *schema, std::string &error)
{
    error.clear();
    GError *storage_error = nullptr;
    const gboolean removed = secret_password_clear_sync(
        schema, nullptr, &storage_error, kAccountAttribute, kAccountValue, nullptr);
    if (!removed) {
        error = consume_error(storage_error, "Could not remove the token securely");
        return false;
    }
    return true;
}

} // namespace

bool SecureTokenStore::store_facebook_token(const std::string &token, std::string &error)
{
    return store_token(&kFacebookSchema, kFacebookLabel, "Facebook", token, error);
}

bool SecureTokenStore::load_facebook_token(std::string &token, std::string &error)
{
    return load_token(&kFacebookSchema, "Facebook", token, error);
}

bool SecureTokenStore::remove_facebook_token(std::string &error)
{
    return remove_token(&kFacebookSchema, error);
}

bool SecureTokenStore::store_twitch_token(const std::string &token, std::string &error)
{
    return store_token(&kTwitchSchema, kTwitchLabel, "Twitch", token, error);
}

bool SecureTokenStore::load_twitch_token(std::string &token, std::string &error)
{
    return load_token(&kTwitchSchema, "Twitch", token, error);
}

bool SecureTokenStore::remove_twitch_token(std::string &error)
{
    return remove_token(&kTwitchSchema, error);
}

bool SecureTokenStore::store_youtube_token(const std::string &token, std::string &error)
{
    return store_token(&kYouTubeSchema, kYouTubeLabel, "YouTube", token, error);
}

bool SecureTokenStore::load_youtube_token(std::string &token, std::string &error)
{
    return load_token(&kYouTubeSchema, "YouTube", token, error);
}

bool SecureTokenStore::remove_youtube_token(std::string &error)
{
    return remove_token(&kYouTubeSchema, error);
}

bool SecureTokenStore::store_youtube_client_secret(const std::string &secret, std::string &error)
{
    return store_token(&kYouTubeClientSecretSchema, kYouTubeClientSecretLabel,
                       "YouTube client credential", secret, error);
}

bool SecureTokenStore::load_youtube_client_secret(std::string &secret, std::string &error)
{
    return load_token(&kYouTubeClientSecretSchema, "YouTube client credential", secret, error);
}

bool SecureTokenStore::remove_youtube_client_secret(std::string &error)
{
    return remove_token(&kYouTubeClientSecretSchema, error);
}

#else

bool SecureTokenStore::store_facebook_token(const std::string &, std::string &error)
{
    error = "Secure Facebook token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::load_facebook_token(std::string &token, std::string &error)
{
    token.clear();
    error = "Secure Facebook token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::remove_facebook_token(std::string &error)
{
    error = "Secure Facebook token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::store_twitch_token(const std::string &, std::string &error)
{
    error = "Secure Twitch token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::load_twitch_token(std::string &token, std::string &error)
{
    token.clear();
    error = "Secure Twitch token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::remove_twitch_token(std::string &error)
{
    error = "Secure Twitch token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::store_youtube_token(const std::string &, std::string &error)
{
    error = "Secure YouTube token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::load_youtube_token(std::string &token, std::string &error)
{
    token.clear();
    error = "Secure YouTube token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::remove_youtube_token(std::string &error)
{
    error = "Secure YouTube token storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::store_youtube_client_secret(const std::string &, std::string &error)
{
    error = "Secure YouTube client credential storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::load_youtube_client_secret(std::string &secret, std::string &error)
{
    secret.clear();
    error = "Secure YouTube client credential storage is not implemented on this platform";
    return false;
}

bool SecureTokenStore::remove_youtube_client_secret(std::string &error)
{
    error = "Secure YouTube client credential storage is not implemented on this platform";
    return false;
}

#endif

} // namespace multistream

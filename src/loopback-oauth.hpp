// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class QTcpServer;
class QTcpSocket;

namespace multistream {

// PKCE material generated for an authorization-code flow.
struct PkcePair {
    std::string verifier;   // 43-128 char high-entropy random string.
    std::string challenge;  // BASE64URL(SHA256(verifier)), no padding.
};

// Generate a fresh PKCE verifier/challenge (S256) pair.
PkcePair generate_pkce_pair();

// Generate a random URL-safe state token used to mitigate CSRF.
std::string generate_oauth_state();

// A local HTTP server bound to the loopback interface (127.0.0.1) on an
// ephemeral (or fixed) port. It opens the system browser at the authorization
// URL and captures the OAuth redirect.
//
// Two capture modes are supported:
//  - Query mode (authorization code flow, e.g. Google): the provider returns
//    the code and state in the query string of the redirect. The server reads
//    them directly from the GET request line.
//  - Fragment mode (implicit flow, e.g. Twitch/Facebook): the token is returned
//    in the URL fragment, which browsers do not send to the server. The server
//    replies with a small HTML page whose JavaScript reposts location.hash to a
//    secondary path so the values can be captured.
class LoopbackOAuthServer final : public QObject {
public:
    enum class CaptureMode { Query, Fragment };

    struct Result {
        bool success = false;
        // Query mode: parsed query parameters (code, state, error...).
        // Fragment mode: parsed fragment parameters (access_token, state...).
        std::string code;
        std::string access_token;
        std::string state;
        std::string error;        // OAuth error code, if any.
        std::string error_detail; // Human readable detail, if any.
    };

    using ResultCallback = std::function<void(const Result &)>;

    explicit LoopbackOAuthServer(QObject *parent = nullptr);
    ~LoopbackOAuthServer() override;

    LoopbackOAuthServer(const LoopbackOAuthServer &) = delete;
    LoopbackOAuthServer &operator=(const LoopbackOAuthServer &) = delete;

    // Start listening. When port is 0 an ephemeral port is chosen. Returns false
    // if the server could not bind.
    bool start(CaptureMode mode, quint16 port = 0);

    // The loopback redirect URI the caller must send as redirect_uri, e.g.
    // "http://127.0.0.1:54321". The path component is fixed to "/".
    QString redirect_uri() const;

    // Selected port after start().
    quint16 port() const;

    // Register the callback fired once, when the redirect is captured or fails.
    void set_result_callback(ResultCallback callback);

    // Stop listening and drop pending sockets.
    void stop();

private:
    void handle_connection();
    void handle_request(QTcpSocket *socket, const QByteArray &request);
    void respond_html(QTcpSocket *socket, const QByteArray &body);
    void finish(const Result &result);

    std::unique_ptr<QTcpServer> server_;
    CaptureMode mode_ = CaptureMode::Query;
    quint16 port_ = 0;
    ResultCallback callback_;
    bool finished_ = false;
};

} // namespace multistream

// SPDX-License-Identifier: GPL-2.0-or-later

#include "loopback-oauth.hpp"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace multistream {
namespace {

// Unreserved characters allowed in a PKCE code_verifier per RFC 7636.
constexpr char kVerifierAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

std::string random_string(int length)
{
    std::string value;
    value.reserve(static_cast<std::size_t>(length));
    const int alphabet_size = static_cast<int>(sizeof(kVerifierAlphabet) - 1);
    for (int i = 0; i < length; ++i) {
        const quint32 index = QRandomGenerator::system()->bounded(alphabet_size);
        value.push_back(kVerifierAlphabet[index]);
    }
    return value;
}

QByteArray base64_url_no_padding(const QByteArray &data)
{
    return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

// Parse "key=value&key=value" pairs (percent-decoded) into the Result fields.
void fill_from_query(const QString &encoded, LoopbackOAuthServer::Result &result)
{
    const QUrlQuery query(encoded);
    result.code = query.queryItemValue("code", QUrl::FullyDecoded).toStdString();
    result.access_token = query.queryItemValue("access_token", QUrl::FullyDecoded).toStdString();
    result.state = query.queryItemValue("state", QUrl::FullyDecoded).toStdString();
    result.error = query.queryItemValue("error", QUrl::FullyDecoded).toStdString();
    const QString detail = query.queryItemValue("error_description", QUrl::FullyDecoded);
    result.error_detail = detail.toStdString();
}

// Minimal HTML shown after the user authorizes. Fragment mode uses JavaScript to
// repost the fragment to /fragment so the token can be captured server-side.
QByteArray success_page()
{
    return QByteArrayLiteral(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>OBS Multistream</title></head>"
        "<body style=\"font-family:sans-serif;background:#24212c;color:#d9ebe5;text-align:center;padding-top:60px\">"
        "<h2>Authorization complete</h2><p>You can close this tab and return to OBS.</p></body></html>");
}

QByteArray fragment_bridge_page()
{
    // On the initial GET the fragment is not sent to the server. This page reads
    // location.hash and reissues a request to /fragment?<hash> so the server can
    // read the token from the query string.
    return QByteArrayLiteral(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>OBS Multistream</title></head>"
        "<body style=\"font-family:sans-serif;background:#24212c;color:#d9ebe5;text-align:center;padding-top:60px\">"
        "<h2>Finishing sign-in…</h2>"
        "<script>"
        "var h=window.location.hash?window.location.hash.substring(1):'';"
        "fetch('/fragment?'+h).then(function(){document.body.innerHTML="
        "'<h2>Authorization complete</h2><p>You can close this tab and return to OBS.</p>';});"
        "</script></body></html>");
}

} // namespace

PkcePair generate_pkce_pair()
{
    PkcePair pair;
    pair.verifier = random_string(64);
    const QByteArray hash =
        QCryptographicHash::hash(QByteArray::fromStdString(pair.verifier), QCryptographicHash::Sha256);
    pair.challenge = base64_url_no_padding(hash).toStdString();
    return pair;
}

std::string generate_oauth_state()
{
    return random_string(32);
}

LoopbackOAuthServer::LoopbackOAuthServer(QObject *parent) : QObject(parent) {}

LoopbackOAuthServer::~LoopbackOAuthServer()
{
    stop();
}

bool LoopbackOAuthServer::start(CaptureMode mode, quint16 port)
{
    stop();
    mode_ = mode;
    finished_ = false;
    server_ = std::make_unique<QTcpServer>(this);
    if (!server_->listen(QHostAddress::LocalHost, port)) {
        server_.reset();
        return false;
    }
    port_ = server_->serverPort();
    connect(server_.get(), &QTcpServer::newConnection, this, &LoopbackOAuthServer::handle_connection);
    return true;
}

QString LoopbackOAuthServer::redirect_uri() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(port_);
}

quint16 LoopbackOAuthServer::port() const
{
    return port_;
}

void LoopbackOAuthServer::set_result_callback(ResultCallback callback)
{
    callback_ = std::move(callback);
}

void LoopbackOAuthServer::stop()
{
    // Invalidate the callback before closing sockets. A readyRead event may
    // already be queued; it must not deliver an OAuth result after the owner
    // has cancelled or destroyed the provider.
    finished_ = true;
    callback_ = {};
    if (server_) {
        server_->close();
        server_.reset();
    }
    port_ = 0;
}

void LoopbackOAuthServer::handle_connection()
{
    if (!server_)
        return;
    while (QTcpSocket *socket = server_->nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            const QByteArray request = socket->readAll();
            handle_request(socket, request);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void LoopbackOAuthServer::handle_request(QTcpSocket *socket, const QByteArray &request)
{
    // Parse the request line: METHOD SP path SP HTTP/x.y
    const int line_end = request.indexOf("\r\n");
    const QByteArray request_line = line_end >= 0 ? request.left(line_end) : request;
    const auto parts = request_line.split(' ');
    if (parts.size() < 2) {
        respond_html(socket, success_page());
        return;
    }

    const QByteArray target = parts.at(1);
    const int query_start = target.indexOf('?');
    const QString path = QString::fromUtf8(query_start >= 0 ? target.left(query_start) : target);
    const QString query = query_start >= 0 ? QString::fromUtf8(target.mid(query_start + 1)) : QString{};

    if (mode_ == CaptureMode::Fragment && path != QStringLiteral("/fragment")) {
        // First hit: hand the browser the JS bridge that reposts the fragment.
        respond_html(socket, fragment_bridge_page());
        return;
    }

    Result result;
    fill_from_query(query, result);
    result.success = result.error.empty() &&
                     (!result.code.empty() || !result.access_token.empty());
    respond_html(socket, success_page());
    finish(result);
}

void LoopbackOAuthServer::respond_html(QTcpSocket *socket, const QByteArray &body)
{
    if (!socket)
        return;
    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\nConnection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void LoopbackOAuthServer::finish(const Result &result)
{
    if (finished_)
        return;
    finished_ = true;
    // Move the callback out before invoking it. The callback may stop the server
    // synchronously; destroying callback_ while std::function is executing is
    // undefined behavior.
    ResultCallback callback = std::move(callback_);
    callback_ = {};
    if (callback) {
        const Result deferred_result = result;
        QTimer::singleShot(0, this, [callback = std::move(callback), deferred_result]() mutable {
            callback(deferred_result);
        });
    }
}

} // namespace multistream

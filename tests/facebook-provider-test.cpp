// SPDX-License-Identifier: GPL-2.0-or-later

#include "facebook-provider.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace multistream {

class FacebookProviderTestAccess {
public:
    static void set_user_token(FacebookProvider &provider, std::string token)
    {
        provider.user_token_ = std::move(token);
    }

    static void fetch_pages(FacebookProvider &provider, FacebookProvider::ResultCallback callback)
    {
        provider.fetch_pages(std::move(callback));
    }

    static void fetch_account(FacebookProvider &provider, FacebookProvider::ResultCallback callback)
    {
        provider.fetch_account(std::move(callback));
    }
};

} // namespace multistream

namespace {

class FakeGraphServer final : public QObject {
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QByteArray headers;
        QByteArray body;
    };

    struct Response {
        int status = 200;
        QByteArray body;
        int delay_ms = 0;
    };

    FakeGraphServer()
    {
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                buffers_.insert(socket, {});
                QObject::connect(socket, &QIODevice::readyRead, &server_, [this, socket]() {
                    read_request(socket);
                });
                QObject::connect(socket, &QAbstractSocket::disconnected, &server_, [this, socket]() {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    QString base_url() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort());
    }

    void enqueue(Response response)
    {
        responses_.push_back(std::move(response));
    }

    void enqueue_json(const QByteArray &body, int status = 200, int delay_ms = 0)
    {
        enqueue({status, body, delay_ms});
    }

    const std::vector<Request> &requests() const
    {
        return requests_;
    }

private:
    static int content_length(const QByteArray &headers)
    {
        const QList<QByteArray> lines = headers.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:"))
                return line.mid(sizeof("content-length:") - 1).trimmed().toInt();
        }
        return 0;
    }

    void read_request(QTcpSocket *socket)
    {
        buffers_[socket].append(socket->readAll());
        QByteArray &buffer = buffers_[socket];
        const int header_end = buffer.indexOf("\r\n\r\n");
        if (header_end < 0)
            return;

        const QByteArray headers = buffer.left(header_end);
        const int body_length = content_length(headers);
        const int request_length = header_end + 4 + body_length;
        if (buffer.size() < request_length)
            return;

        const QByteArray first_line = headers.left(headers.indexOf("\r\n"));
        const QList<QByteArray> request_parts = first_line.split(' ');
        if (request_parts.size() >= 2) {
            Request request;
            request.method = request_parts.at(0);
            request.path = request_parts.at(1);
            request.headers = headers;
            request.body = buffer.mid(header_end + 4, body_length);
            requests_.push_back(std::move(request));
        }
        buffer.remove(0, request_length);

        Response response;
        if (!responses_.empty()) {
            response = std::move(responses_.front());
            responses_.erase(responses_.begin());
        } else {
            response = {500, R"({"error":{"message":"No fake response queued"}})", 0};
        }
        send_response(socket, std::move(response));
    }

    void send_response(QTcpSocket *socket, Response response)
    {
        QPointer<QTcpSocket> guarded_socket(socket);
        auto send = [guarded_socket, response = std::move(response)]() {
            if (!guarded_socket)
                return;
            const QByteArray reason = response.status == 200 ? "OK" : "Error";
            const QByteArray header = "HTTP/1.1 " + QByteArray::number(response.status) + " " + reason +
                                      "\r\nContent-Type: application/json\r\nContent-Length: " +
                                      QByteArray::number(response.body.size()) +
                                      "\r\nConnection: close\r\n\r\n";
            guarded_socket->write(header);
            guarded_socket->write(response.body);
            guarded_socket->disconnectFromHost();
        };

        if (response.delay_ms > 0)
            QTimer::singleShot(response.delay_ms, socket, std::move(send));
        else
            send();
    }

    QTcpServer server_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    std::vector<Response> responses_;
    std::vector<Request> requests_;
};

bool wait_for(const std::function<void(const std::function<void()> &)> &start, int timeout_ms = 2000)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool called = false;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    start([&]() {
        called = true;
        loop.quit();
    });
    timeout.start(timeout_ms);
    loop.exec();
    return called;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    FakeGraphServer server;
    assert(server.listen());

    multistream::FacebookProvider::Options options;
    options.graph_base_url = server.base_url();
    options.transfer_timeout_ms = 50;
    options.max_retries = 1;
    options.retry_backoff_ms = 1;
    multistream::FacebookProvider provider(options);
    multistream::FacebookProviderTestAccess::set_user_token(provider, "user-token");

    server.enqueue_json(QByteArray(R"({"data":[{"id":"page-1","name":"Test Page","access_token":"page-token"},{"id":"ignored","name":"No token"}],"paging":{"next":")") +
                         server.base_url().toUtf8() + R"(/me/accounts?after=cursor"}})");
    server.enqueue_json(R"({"data":[{"id":"page-2","name":"Second Page","access_token":"page-token-2"}]})");
    bool fetch_success = false;
    std::string fetch_message;
    const bool fetch_called = wait_for([&](const auto &done) {
        multistream::FacebookProviderTestAccess::fetch_pages(
            provider, [&](bool success, const std::string &message) {
                fetch_success = success;
                fetch_message = message;
                done();
            });
    });
    assert(fetch_called && fetch_success);
    assert(fetch_message.empty());
    assert(provider.pages().size() == 2);
    assert(provider.pages().at(0).id == "page-1");
    assert(provider.pages().at(1).id == "page-2");
    assert(server.requests().size() == 2);
    assert(server.requests().at(0).method == "GET");
    assert(server.requests().at(0).path.startsWith("/me/accounts?fields="));
    assert(server.requests().at(1).path == "/me/accounts?after=cursor");
    assert(server.requests().at(0).headers.contains("Authorization: Bearer user-token"));
    assert(server.requests().at(1).headers.contains("Authorization: Bearer user-token"));

    multistream::DestinationConfig timeline;
    timeline.id = "timeline";
    timeline.name = "Timeline";
    timeline.provider_id = "facebook";
    timeline.facebook_target = "timeline";
    timeline.facebook_title = "Test live";
    timeline.facebook_privacy = "UNLISTED";

    server.enqueue_json(R"({"id":"live-1","secure_stream_url":"rtmps://live.example/app/key-1"})");
    bool create_success = false;
    multistream::FacebookLive created_live;
    const bool create_called = wait_for([&](const auto &done) {
        provider.create_live(timeline, [&](bool success, const multistream::FacebookLive &live,
                                          const std::string &message) {
            create_success = success;
            created_live = live;
            assert(message.empty() == success);
            done();
        });
    });
    assert(create_called && create_success);
    assert(created_live.id == "live-1");
    assert(created_live.server == "rtmps://live.example/app/");
    assert(created_live.stream_key == "key-1");
    assert(server.requests().size() == 3);
    assert(server.requests().at(2).method == "POST");
    assert(server.requests().at(2).path == "/me/live_videos");
    assert(server.requests().at(2).headers.contains("Authorization: Bearer user-token"));
    assert(server.requests().at(2).body.contains("status=LIVE_NOW"));
    assert(server.requests().at(2).body.contains("privacy="));
    assert(server.requests().at(2).body.contains("ALL_FRIENDS"));

    server.enqueue_json(R"({"success":true})");
    bool stop_success = false;
    const bool stop_called = wait_for([&](const auto &done) {
        provider.stop_live("live-1", [&](bool success, const std::string &message) {
            stop_success = success;
            assert(message.empty() == success);
            done();
        });
    });
    assert(stop_called && stop_success);
    assert(server.requests().size() == 4);
    assert(server.requests().at(3).method == "POST");
    assert(server.requests().at(3).path == "/live-1");
    assert(server.requests().at(3).body == "end_live_video=true");

    server.enqueue_json(R"({"id":"live-rtmp","stream_url":"rtmp://live.example/app/insecure"})");
    bool insecure_success = true;
    std::string insecure_message;
    const bool insecure_called = wait_for([&](const auto &done) {
        provider.create_live(timeline, [&](bool success, const multistream::FacebookLive &,
                                          const std::string &message) {
            insecure_success = success;
            insecure_message = message;
            done();
        });
    });
    assert(insecure_called && !insecure_success);
    assert(insecure_message == "Facebook did not return a usable secure RTMPS stream URL.");

    const auto assert_create_error = [&](const QByteArray &body, int status, const std::string &expected) {
        const std::size_t requests_before = server.requests().size();
        server.enqueue_json(body, status);
        bool success = true;
        std::string message;
        const bool called = wait_for([&](const auto &done) {
            provider.create_live(timeline, [&](bool request_success, const multistream::FacebookLive &,
                                               const std::string &request_message) {
                success = request_success;
                message = request_message;
                done();
            });
        });
        assert(called && !success);
        assert(message.find(expected) != std::string::npos);
        assert(server.requests().size() == requests_before + 1);
    };

    assert_create_error(R"({"error":{"message":"Invalid token","code":190}})", 401,
                        "session expired");
    assert_create_error(R"({"error":{"message":"Invalid token","code":190}})", 401, "code 190");
    assert_create_error(R"({"error":{"message":"Permission denied","code":10}})", 403,
                        "denied permission");
    assert_create_error(R"({"error":{"message":"Rate limited","code":4,"error_subcode":17}})", 429,
                        "rate-limited");
    assert_create_error(R"({"error":{"message":"Service unavailable","code":2}})", 500,
                        "temporarily unavailable");
    assert_create_error("{invalid json", 200, "invalid response");

    server.enqueue_json(R"({"id":"live-timeout","secure_stream_url":"rtmps://live.example/app/timeout"})", 200, 250);
    bool timeout_success = true;
    std::string timeout_message;
    const bool timeout_called = wait_for([&](const auto &done) {
        provider.create_live(timeline, [&](bool success, const multistream::FacebookLive &,
                                          const std::string &message) {
            timeout_success = success;
            timeout_message = message;
            done();
        });
    }, 1000);
    assert(timeout_called && !timeout_success);
    assert(timeout_message.find("network error") != std::string::npos);

    server.enqueue_json(R"({"id":"live-cancelled","secure_stream_url":"rtmps://live.example/app/cancel"})", 200, 250);
    bool cancel_success = true;
    int cancel_callbacks = 0;
    const bool cancel_called = wait_for([&](const auto &done) {
        const auto request_id = provider.create_live(
            timeline, [&](bool success, const multistream::FacebookLive &, const std::string &) {
                cancel_success = success;
                ++cancel_callbacks;
                done();
            });
        assert(request_id != 0);
        QTimer::singleShot(10, &application, [&provider, request_id]() {
            provider.cancel_request(request_id);
        });
    }, 1000);
    assert(cancel_called && !cancel_success);
    assert(cancel_callbacks == 1);

    server.enqueue_json(R"({"error":{"message":"Rate limited","code":4}})", 429);
    server.enqueue_json(R"({"data":[{"id":"page-retried","name":"Retried Page","access_token":"retried-token"}]})");
    bool retry_success = false;
    const bool retry_called = wait_for([&](const auto &done) {
        multistream::FacebookProviderTestAccess::fetch_pages(
            provider, [&](bool success, const std::string &) {
                retry_success = success;
                done();
            });
    });
    assert(retry_called && retry_success);
    assert(provider.pages().size() == 1);
    assert(provider.pages().front().id == "page-retried");

    server.enqueue_json(R"({"error":{"message":"Service unavailable","code":2}})", 500);
    server.enqueue_json(R"({"error":{"message":"Service unavailable","code":2}})", 500);
    bool exhausted_success = true;
    std::string exhausted_message;
    const bool exhausted_called = wait_for([&](const auto &done) {
        multistream::FacebookProviderTestAccess::fetch_pages(
            provider, [&](bool success, const std::string &message) {
                exhausted_success = success;
                exhausted_message = message;
                done();
            });
    });
    assert(exhausted_called && !exhausted_success);
    assert(exhausted_message.find("temporarily unavailable") != std::string::npos);

    server.enqueue_json(R"({"id":"user-1","name":"Test User"})");
    bool account_success = false;
    std::string account_message;
    const bool account_called = wait_for([&](const auto &done) {
        multistream::FacebookProviderTestAccess::fetch_account(
            provider, [&](bool success, const std::string &message) {
                account_success = success;
                account_message = message;
                done();
            });
    });
    assert(account_called && account_success);
    assert(account_message.empty());
    assert(provider.account_id() == "user-1");
    assert(provider.account_name() == "Test User");
    assert(server.requests().back().method == "GET");
    assert(server.requests().back().path.startsWith("/me?fields="));

    server.enqueue_json(R"({"id":"user-2","first_name":"Ada","last_name":"Lovelace"})");
    account_success = false;
    account_message.clear();
    const bool fallback_account_called = wait_for([&](const auto &done) {
        multistream::FacebookProviderTestAccess::fetch_account(
            provider, [&](bool success, const std::string &message) {
                account_success = success;
                account_message = message;
                done();
            });
    });
    assert(fallback_account_called && account_success);
    assert(account_message.empty());
    assert(provider.account_id() == "user-2");
    assert(provider.account_name() == "Ada Lovelace");
    assert(server.requests().back().path.startsWith("/me?fields="));

    std::cout << "facebook provider tests passed\n";
    return 0;
}

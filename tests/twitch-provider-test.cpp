// SPDX-License-Identifier: GPL-2.0-or-later

#include "twitch-provider.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include <cassert>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>

extern "C" const char *obs_module_text(const char *key)
{
    return key;
}

namespace multistream {
class TwitchProviderTestAccess {
public:
    static void set_session(TwitchProvider &provider, std::string token, std::string account_id)
    {
        provider.user_token_ = std::move(token);
        provider.account_id_ = std::move(account_id);
        provider.account_name_ = "test-user";
    }
};
} // namespace multistream

namespace {
class FakeServer final : public QObject {
public:
    explicit FakeServer(QObject *parent = nullptr) : QObject(parent)
    {
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this]() {
            auto *socket = server_.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                const QByteArray request = socket->readAll();
                requests_.push_back(request);
                const QByteArray response = QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ") +
                                             QByteArray::number(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    QString base_url() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }
    const std::vector<QByteArray> &requests() const { return requests_; }
    void set_body(QByteArray body) { body_ = std::move(body); }

private:
    QTcpServer server_;
    std::vector<QByteArray> requests_;
    QByteArray body_ = R"({"data":[{"stream_key":"secret-key"}]})";
};

bool wait_for(const std::function<void(const std::function<void()> &)> &start)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool called = false;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    start([&]() { called = true; loop.quit(); });
    timeout.start(2000);
    loop.exec();
    return called;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    FakeServer server;
    assert(server.listen());

    multistream::TwitchProvider::Options options;
    options.client_id = "test-client";
    options.api_base_url = server.base_url();
    options.ingest_url = "rtmps://live.twitch.tv/app/";
    options.transfer_timeout_ms = 500;
    multistream::TwitchProvider provider(options);
    multistream::TwitchProviderTestAccess::set_session(provider, "access-token", "user-1");

    multistream::DestinationConfig config;
    config.id = "twitch";
    config.name = "Twitch";
    config.provider_id = "twitch";
    config.twitch_title = "Test stream";

    bool success = false;
    std::string message;
    const bool called = wait_for([&](const auto &done) {
        provider.create_live(config, [&](bool ok, const multistream::TwitchLive &live, const std::string &error) {
            success = ok;
            message = error;
            assert(live.server == "rtmps://live.twitch.tv/app/");
            assert(live.stream_key == "secret-key");
            done();
        });
    });
    assert(called && success && message.empty());
    assert(server.requests().size() == 2);
    assert(server.requests().at(0).startsWith("PATCH /channels?broadcaster_id=user-1"));
    assert(server.requests().at(0).contains("Client-Id: test-client"));
    assert(server.requests().at(0).contains("Authorization: Bearer access-token"));
    assert(server.requests().at(0).contains("\"title\":\"Test stream\""));
    assert(server.requests().at(1).startsWith("GET /streams/key?broadcaster_id=user-1"));

    std::cout << "twitch provider tests passed\n";
    return 0;
}

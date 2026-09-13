// SPDX-License-Identifier: GPL-2.0-or-later

#include "youtube-provider.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

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
class YouTubeProviderTestAccess {
public:
    static void set_session(YouTubeProvider &provider, std::string access, std::string refresh,
                            std::string channel_id)
    {
        provider.access_token_ = std::move(access);
        provider.refresh_token_ = std::move(refresh);
        provider.channel_id_ = std::move(channel_id);
        provider.channel_title_ = "Test Channel";
    }
};
} // namespace multistream

namespace {

// Serves scripted JSON responses in order, one per HTTP request.
class ScriptedServer final : public QObject {
public:
    explicit ScriptedServer(std::vector<QByteArray> bodies) : bodies_(std::move(bodies))
    {
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this]() {
            auto *socket = server_.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                const QByteArray request = socket->readAll();
                requests_.push_back(request);
                const QByteArray body = index_ < bodies_.size() ? bodies_.at(index_) : QByteArray("{}");
                ++index_;
                QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
                response += QByteArray::number(body.size());
                response += "\r\nConnection: close\r\n\r\n";
                response += body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    QString base_url() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }
    const std::vector<QByteArray> &requests() const { return requests_; }

private:
    QTcpServer server_;
    std::vector<QByteArray> bodies_;
    std::vector<QByteArray> requests_;
    std::size_t index_ = 0;
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

    // Scripted responses: broadcast insert, stream insert, bind.
    std::vector<QByteArray> bodies = {
        QByteArray(R"({"id":"broadcast-1"})"),
        QByteArray(R"({"id":"stream-1","cdn":{"ingestionInfo":{"ingestionAddress":"rtmp://a.rtmp.youtube.com/live2","streamName":"key-123"}}})"),
        QByteArray(R"({"id":"broadcast-1"})"),
    };
    ScriptedServer server(std::move(bodies));
    assert(server.listen());

    multistream::YouTubeProvider::Options options;
    options.client_id = "test-client";
    options.api_base_url = server.base_url();
    options.transfer_timeout_ms = 500;
    multistream::YouTubeProvider provider(options);
    multistream::YouTubeProviderTestAccess::set_session(provider, "access-token", "refresh-token", "chan-1");

    multistream::DestinationConfig config;
    config.id = "youtube";
    config.name = "YouTube";
    config.provider_id = "youtube";
    config.youtube_title = "Test stream";
    config.youtube_privacy = "unlisted";

    bool success = false;
    std::string message;
    multistream::YouTubeLive captured;
    const bool called = wait_for([&](const auto &done) {
        provider.create_live(config, [&](bool ok, const multistream::YouTubeLive &live, const std::string &error) {
            success = ok;
            message = error;
            captured = live;
            done();
        });
    });

    assert(called);
    assert(success && message.empty());
    assert(captured.broadcast_id == "broadcast-1");
    assert(captured.stream_id == "stream-1");
    assert(captured.server == "rtmp://a.rtmp.youtube.com/live2");
    assert(captured.stream_key == "key-123");
    assert(server.requests().size() == 3);
    assert(server.requests().at(0).startsWith("POST /liveBroadcasts?"));
    assert(server.requests().at(0).contains("Authorization: Bearer access-token"));
    assert(server.requests().at(0).contains("\"privacyStatus\":\"unlisted\""));
    assert(server.requests().at(1).startsWith("POST /liveStreams?"));
    assert(server.requests().at(1).contains("\"ingestionType\":\"rtmp\""));
    assert(server.requests().at(2).startsWith("POST /liveBroadcasts/bind?"));
    assert(server.requests().at(2).contains("streamId=stream-1"));

    std::cout << "youtube provider tests passed\n";
    return 0;
}

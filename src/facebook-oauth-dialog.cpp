// SPDX-License-Identifier: GPL-2.0-or-later

#include "facebook-provider.hpp"
#include "facebook-utils.hpp"
#include "loopback-oauth.hpp"

#include <obs-module.h>

#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

namespace multistream {
namespace {

constexpr const char *kAppId = "1609511030686437";

QString text(const char *key)
{
    return QString::fromUtf8(obs_module_text(key));
}

} // namespace

void FacebookProvider::authenticate(QWidget *parent, ResultCallback callback)
{
    (void)parent;
    cancel_pending_requests();
    oauth_state_ = generate_oauth_state();

    oauth_server_ = std::make_unique<LoopbackOAuthServer>();
    // Facebook uses the implicit grant; the token arrives in the URL fragment,
    // so the loopback server serves a bridge page that reposts it.
    if (!oauth_server_->start(LoopbackOAuthServer::CaptureMode::Fragment)) {
        oauth_server_.reset();
        callback(false, text("FacebookLoginServerError").toStdString());
        return;
    }

    QUrl authorization("https://www.facebook.com/v26.0/dialog/oauth");
    QUrlQuery query;
    query.addQueryItem("client_id", kAppId);
    query.addQueryItem("redirect_uri", oauth_server_->redirect_uri());
    query.addQueryItem("state", QString::fromStdString(oauth_state_));
    query.addQueryItem("response_type", "token");
    query.addQueryItem("scope", "pages_show_list,pages_read_engagement,pages_manage_posts,publish_video");
    authorization.setQuery(query);

    const std::uint64_t auth_generation = session_generation_;
    oauth_server_->set_result_callback([this, auth_generation, callback](const LoopbackOAuthServer::Result &result) {
        if (auth_generation != session_generation_)
            return;
        if (oauth_server_)
            oauth_server_->stop();
        std::string token;
        std::string error;
        if (!parse_facebook_oauth_values(result.access_token, result.state, result.error, result.error_detail,
                                         oauth_state_, token, error)) {
            callback(false, error.empty() ? text("FacebookLoginCancelled").toStdString() : error);
            return;
        }
        user_token_ = std::move(token);
        finish_session(callback, true);
    });

    if (!QDesktopServices::openUrl(authorization)) {
        oauth_server_->stop();
        oauth_server_.reset();
        callback(false, text("FacebookLoginBrowserError").toStdString());
    }
}

} // namespace multistream

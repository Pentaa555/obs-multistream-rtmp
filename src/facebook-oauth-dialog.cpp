// SPDX-License-Identifier: GPL-2.0-or-later

#include "facebook-provider.hpp"
#include "facebook-utils.hpp"
#include "loopback-oauth.hpp"

#include <obs-module.h>

#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <functional>

namespace multistream {
namespace {

constexpr const char *kAppId = "1609511030686437";

// Facebook requires an HTTPS OAuth redirect. The implicit-grant token arrives in
// the URL fragment, which the system browser never sends to a server. This HTTPS
// bridge page reads the fragment (or the error query on denial) client-side and
// forwards it to the plugin's local loopback server. The plugin therefore learns
// the outcome automatically — success OR denial — with no copy/paste.
constexpr const char *kBridgeRedirect = "https://pentamultistream.online/oauth.html";

QString text(const char *key)
{
    return QString::fromUtf8(obs_module_text(key));
}

// Modal "waiting" dialog shown while the user completes sign-in in the browser.
// It closes automatically when the loopback server reports a result, or lets the
// user cancel if they abandon the browser flow.
class FacebookWaitDialog final : public QDialog {
public:
    explicit FacebookWaitDialog(QWidget *parent) : QDialog(parent)
    {
        setWindowTitle(text("FacebookLoginTitle"));
        setModal(true);

        auto *layout = new QVBoxLayout(this);
        auto *instructions = new QLabel(text("FacebookLoginInstructions"), this);
        instructions->setWordWrap(true);
        layout->addWidget(instructions);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        resize(460, height());
    }
};

} // namespace

void FacebookProvider::authenticate(QWidget *parent, ResultCallback callback)
{
    cancel_pending_requests();

    // Start the local loopback server first so we know which port to hand to the
    // bridge page. The bridge forwards the OAuth result here automatically.
    oauth_server_ = std::make_unique<LoopbackOAuthServer>();
    if (!oauth_server_->start(LoopbackOAuthServer::CaptureMode::Query)) {
        oauth_server_.reset();
        callback(false, text("FacebookLoginServerError").toStdString());
        return;
    }

    // The state carries the loopback port so the HTTPS bridge can forward the
    // result to the correct local server. Format: "<random>.<port>". Facebook
    // returns state verbatim, so the redirect_uri stays identical to the one
    // registered in the app settings (no per-attempt query parameters).
    const std::string random_state = generate_oauth_state();
    oauth_state_ = random_state + "." + std::to_string(oauth_server_->port());

    QUrl authorization("https://www.facebook.com/v26.0/dialog/oauth");
    QUrlQuery query;
    query.addQueryItem("client_id", kAppId);
    query.addQueryItem("redirect_uri", QString::fromUtf8(kBridgeRedirect));
    query.addQueryItem("state", QString::fromStdString(oauth_state_));
    query.addQueryItem("response_type", "token");
    query.addQueryItem("scope", "pages_show_list,pages_read_engagement,pages_manage_posts,publish_video");
    authorization.setQuery(query);

    auto dialog = std::make_shared<FacebookWaitDialog>(parent);
    const std::uint64_t auth_generation = session_generation_;

    // The loopback callback only captures the raw OAuth result and closes the
    // modal dialog. The Graph API calls (fetch_account/fetch_pages) MUST run in
    // the main event loop AFTER the modal loop exits — issuing them from inside
    // the modal loop makes the QNetworkReply time out.
    auto captured = std::make_shared<LoopbackOAuthServer::Result>();
    auto have_result = std::make_shared<bool>(false);

    oauth_server_->set_result_callback([this, auth_generation, dialog, captured, have_result](
                                           const LoopbackOAuthServer::Result &result) {
        if (auth_generation != session_generation_)
            return;
        if (oauth_server_)
            oauth_server_->stop();
        *captured = result;
        *have_result = true;
        if (dialog)
            dialog->accept();
    });

    if (!QDesktopServices::openUrl(authorization)) {
        oauth_server_->stop();
        oauth_server_.reset();
        callback(false, text("FacebookLoginBrowserError").toStdString());
        return;
    }

    // Run the modal waiting dialog. It closes when the loopback captures a
    // result (accept) or when the user cancels (reject).
    const bool accepted = dialog->exec() == QDialog::Accepted;
    if (oauth_server_) {
        oauth_server_->stop();
        oauth_server_.reset();
    }

    if (!accepted || !*have_result) {
        callback(false, text("FacebookLoginCancelled").toStdString());
        return;
    }

    // Now we are back in the main event loop: safe to parse and call Graph.
    std::string token;
    std::string error;
    if (!parse_facebook_oauth_values(captured->access_token, captured->state, captured->error,
                                     captured->error_detail, oauth_state_, token, error)) {
        callback(false, error.empty() ? text("FacebookLoginCancelled").toStdString() : error);
        return;
    }
    user_token_ = std::move(token);
    finish_session(std::move(callback), true);
}

} // namespace multistream

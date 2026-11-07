// SPDX-License-Identifier: GPL-3.0-only
#include "googleauth.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

using namespace drivesync;

static constexpr int REFRESH_MARGIN_SECS = 60;
static constexpr int TOKEN_TIMEOUT_MS = 30000;
static constexpr int MAX_REQUEST_SIZE = 16 * 1024;

QString SettingsTokenStore::loadRefreshToken() const
{
    return QSettings().value(m_key).toString();
}

void SettingsTokenStore::saveRefreshToken(const QString& token)
{
    QSettings settings;
    if (token.isEmpty()) {
        settings.remove(m_key);
    } else {
        settings.setValue(m_key, token);
    }
    settings.sync();
}

static QString randomUrlSafe(int bytes)
{
    QByteArray data(bytes, Qt::Uninitialized);
    for (char& c : data) {
        c = char(QRandomGenerator::system()->bounded(256));
    }
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

static QString codeChallenge(const QString& verifier)
{
    const QByteArray hash = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

static QByteArray formEncode(const QList<QPair<QString, QString> >& params)
{
    QByteArray body;
    for (const auto& [key, value] : params) {
        if (!body.isEmpty()) {
            body += '&';
        }
        body += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    return body;
}

static QByteArray htmlPage(const QString& text)
{
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Google Drive</title></head>"
           "<body style=\"font-family:sans-serif;font-size:1.2em;margin:2em\">"
           + text.toHtmlEscaped().toUtf8() + "</body></html>";
}

static void sendHttp(QTcpSocket* socket, const QByteArray& status, const QByteArray& body)
{
    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
}

GoogleAuth::GoogleAuth(QNetworkAccessManager* nam, ClientConfig config, ITokenStore* store, QObject* parent)
    : QObject(parent), m_nam(nam), m_config(std::move(config)), m_store(store),
    m_donePageText(QStringLiteral("Signed in. You can close this page and go back to the app."))
{
    if (m_store) {
        m_refreshToken = m_store->loadRefreshToken();
    }
}

GoogleAuth::~GoogleAuth()
{
    if (m_refreshReply) {
        m_refreshReply->abort();
    }
}

bool GoogleAuth::isConfigured() const
{
    return !m_config.clientId.isEmpty();
}

bool GoogleAuth::isSignedIn() const
{
    return !m_refreshToken.isEmpty();
}

bool GoogleAuth::isSigningIn() const
{
    return m_signingIn;
}

void GoogleAuth::setDonePageText(const QString& text)
{
    m_donePageText = text;
}

void GoogleAuth::signIn()
{
    if (!isConfigured()) {
        emit signInFailed(QStringLiteral("No OAuth client configured"));
        return;
    }

    stopServer();
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        const QString error = m_server->errorString();
        stopServer();
        emit signInFailed(QStringLiteral("Cannot listen on the loopback address: ") + error);
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &GoogleAuth::onConnection);

    m_signingIn = true;
    m_redirectUri = QStringLiteral("http://127.0.0.1:%1/").arg(m_server->serverPort());
    m_state = randomUrlSafe(16);
    m_codeVerifier = randomUrlSafe(48);

    QUrlQuery query;
    query.addQueryItem("client_id", m_config.clientId);
    query.addQueryItem("redirect_uri", m_redirectUri);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("scope", m_config.scopes.join(' '));
    query.addQueryItem("state", m_state);
    query.addQueryItem("code_challenge", codeChallenge(m_codeVerifier));
    query.addQueryItem("code_challenge_method", "S256");
    // A refresh token is only issued with offline access, and again only on consent
    query.addQueryItem("access_type", "offline");
    query.addQueryItem("prompt", "consent");

    QUrl url = m_config.authUrl;
    url.setQuery(query);
    emit openBrowser(url);
}

void GoogleAuth::cancelSignIn()
{
    if (m_signingIn) {
        finishSignIn(QStringLiteral("Cancelled"));
    }
}

void GoogleAuth::signOut()
{
    cancelSignIn();
    m_refreshToken.clear();
    m_accessToken.clear();
    m_expiresAt = QDateTime();
    if (m_store) {
        m_store->saveRefreshToken(QString());
    }
    flushPending(QString(), DriveError { 0, "notSignedIn", "Signed out" });
    emit signedOut();
}

void GoogleAuth::stopServer()
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

void GoogleAuth::onConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        socket->setParent(this); // outlives the server, which stops right after the redirect, until the reply is sent
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            if (buffer->size() > MAX_REQUEST_SIZE) {
                socket->abort();
                return;
            }
            if (buffer->contains("\r\n\r\n")) {
                onRedirect(socket, *buffer);
            }
        });
    }
}

void GoogleAuth::onRedirect(QTcpSocket* socket, const QByteArray& request)
{
    // "GET /?state=...&code=... HTTP/1.1"
    const QList<QByteArray> requestLine = request.left(request.indexOf("\r\n")).split(' ');
    const QUrl target = requestLine.size() >= 2 ? QUrl(QString::fromLatin1(requestLine.at(1))) : QUrl();
    if (requestLine.value(0) != "GET" || target.path() != "/") {
        sendHttp(socket, "404 Not Found", QByteArray()); // e.g. /favicon.ico
        return;
    }

    if (!m_signingIn) {
        sendHttp(socket, "410 Gone", htmlPage(QStringLiteral("This sign-in is no longer active.")));
        return;
    }

    const QUrlQuery query(target);
    if (query.queryItemValue("state") != m_state) {
        sendHttp(socket, "400 Bad Request", htmlPage(QStringLiteral("Invalid sign-in response.")));
        return; // not ours: keep waiting for the real one
    }

    const QString error = query.queryItemValue("error");
    const QString code = query.queryItemValue("code", QUrl::FullyDecoded);
    if (!error.isEmpty() || code.isEmpty()) {
        sendHttp(socket, "200 OK", htmlPage(QStringLiteral("Sign-in failed: ") + (error.isEmpty() ? "no code" : error)));
        finishSignIn(error.isEmpty() ? QStringLiteral("No authorization code") : error);
        return;
    }

    sendHttp(socket, "200 OK", htmlPage(m_donePageText));
    stopServer();

    requestTokens({
        { "grant_type", "authorization_code" },
        { "code", code },
        { "redirect_uri", m_redirectUri },
        { "code_verifier", m_codeVerifier },
    }, true);
}

void GoogleAuth::requestTokens(const QList<QPair<QString, QString> >& params, bool signingIn)
{
    QList<QPair<QString, QString> > all = params;
    all.append(qMakePair(QStringLiteral("client_id"), m_config.clientId));
    if (!m_config.clientSecret.isEmpty()) {
        all.append(qMakePair(QStringLiteral("client_secret"), m_config.clientSecret));
    }

    QNetworkRequest request(m_config.tokenUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(TOKEN_TIMEOUT_MS);

    QNetworkReply* reply = m_nam->post(request, formEncode(all));
    if (!signingIn) {
        m_refreshReply = reply;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, signingIn]() {
        onTokenReply(reply, signingIn);
    });
}

void GoogleAuth::onTokenReply(QNetworkReply* reply, bool signingIn)
{
    reply->deleteLater();
    if (!signingIn) {
        m_refreshReply = nullptr;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
    const QString accessToken = json.value("access_token").toString();

    if (reply->error() != QNetworkReply::NoError || accessToken.isEmpty()) {
        const QString reason = json.value("error").toString();
        QString message = json.value("error_description").toString();
        if (message.isEmpty()) {
            message = reason.isEmpty() ? reply->errorString() : reason;
        }

        if (signingIn) {
            finishSignIn(message);
            return;
        }

        if (reason == "invalid_grant") {
            // The refresh token expired or was revoked: a new sign-in is needed
            m_refreshToken.clear();
            if (m_store) {
                m_store->saveRefreshToken(QString());
            }
            flushPending(QString(), DriveError { status, "notSignedIn", "Google access expired: sign in again" });
            emit signedOut();
            return;
        }

        flushPending(QString(), DriveError { status, reason.isEmpty() ? "auth" : reason, message });
        return;
    }

    m_accessToken = accessToken;
    m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(json.value("expires_in").toInt(3600));
    const QString refreshToken = json.value("refresh_token").toString();
    if (!refreshToken.isEmpty()) {
        m_refreshToken = refreshToken;
        if (m_store) {
            m_store->saveRefreshToken(refreshToken);
        }
    }

    if (signingIn) {
        finishSignIn(m_refreshToken.isEmpty() ? QStringLiteral("Google returned no refresh token") : QString());
    }
    flushPending(m_accessToken, DriveError());
}

void GoogleAuth::finishSignIn(const QString& error)
{
    stopServer();
    m_signingIn = false;
    m_state.clear();
    m_codeVerifier.clear();
    if (error.isEmpty()) {
        emit signedIn();
    } else {
        emit signInFailed(error);
    }
}

void GoogleAuth::accessToken(TokenCallback callback)
{
    if (!m_accessToken.isEmpty() && m_expiresAt.isValid()
        && QDateTime::currentDateTimeUtc().secsTo(m_expiresAt) > REFRESH_MARGIN_SECS) {
        callback(m_accessToken, DriveError());
        return;
    }

    if (m_refreshToken.isEmpty()) {
        callback(QString(), DriveError { 0, "notSignedIn", "Not signed in to Google" });
        return;
    }

    m_pending.append(std::move(callback));
    if (!m_refreshReply) {
        requestTokens({
            { "grant_type", "refresh_token" },
            { "refresh_token", m_refreshToken },
        }, false);
    }
}

void GoogleAuth::invalidate(const QString& token)
{
    if (m_accessToken == token) {
        m_accessToken.clear();
        m_expiresAt = QDateTime();
    }
}

void GoogleAuth::flushPending(const QString& token, const DriveError& error)
{
    const QList<TokenCallback> pending = std::move(m_pending);
    m_pending.clear();
    for (const TokenCallback& callback : pending) {
        callback(token, error);
    }
}

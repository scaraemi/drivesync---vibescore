// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include "itokenprovider.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTcpSocket;

namespace drivesync {
//! Where the refresh token is kept between runs.
class ITokenStore
{
public:
    virtual ~ITokenStore() = default;
    virtual QString loadRefreshToken() const = 0;
    virtual void saveRefreshToken(const QString& token) = 0;
};

//! QSettings-backed store. On Android the settings are in the app's private storage; on Windows
//! they are in the user's registry, in plain text: use Credential Manager before shipping there.
class SettingsTokenStore : public ITokenStore
{
public:
    explicit SettingsTokenStore(QString key = QStringLiteral("drivesync/refreshToken"))
        : m_key(std::move(key)) {}
    QString loadRefreshToken() const override;
    void saveRefreshToken(const QString& token) override;

private:
    QString m_key;
};

//! Google OAuth 2.0 for installed apps: authorization code flow with PKCE and a loopback redirect
//! (http://127.0.0.1:<random port>/). Needs an OAuth client of type "Desktop app" in Google Cloud.
//! Only QtNetwork: no QtNetworkAuth, so it builds wherever the app builds (Windows, Android).
//! On Android the system browser reaches the app's loopback server while the app is in the background.
class GoogleAuth : public QObject, public ITokenProvider
{
    Q_OBJECT

public:
    struct ClientConfig {
        QString clientId;
        QString clientSecret; // Google issues one to desktop clients too; it is not confidential
        QStringList scopes { QStringLiteral("https://www.googleapis.com/auth/drive") };
        QUrl authUrl { QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth") };
        QUrl tokenUrl { QStringLiteral("https://oauth2.googleapis.com/token") };
    };

    GoogleAuth(QNetworkAccessManager* nam, ClientConfig config, ITokenStore* store, QObject* parent = nullptr);
    ~GoogleAuth() override;

    bool isConfigured() const;
    bool isSignedIn() const;
    bool isSigningIn() const;

    //! Starts the browser sign-in: connect openBrowser() to QDesktopServices::openUrl.
    void signIn();
    void cancelSignIn();
    void signOut();

    //! Text of the page the browser shows after the sign-in
    void setDonePageText(const QString& text);

    // ITokenProvider
    void accessToken(TokenCallback callback) override;
    void invalidate(const QString& token) override;

signals:
    void openBrowser(const QUrl& url);
    void signedIn();
    void signInFailed(const QString& error);
    //! Signed out by the user, or Google revoked the access (the refresh token stopped working)
    void signedOut();

private:
    void onConnection();
    void onRedirect(QTcpSocket* socket, const QByteArray& request);
    void requestTokens(const QList<QPair<QString, QString> >& params, bool signingIn);
    void onTokenReply(QNetworkReply* reply, bool signingIn);
    void finishSignIn(const QString& error);
    void flushPending(const QString& token, const DriveError& error);
    void stopServer();

    QNetworkAccessManager* m_nam = nullptr;
    ClientConfig m_config;
    ITokenStore* m_store = nullptr;

    QString m_refreshToken;
    QString m_accessToken;
    QDateTime m_expiresAt;

    QTcpServer* m_server = nullptr;
    QString m_redirectUri;
    QString m_state;
    QString m_codeVerifier;
    QString m_donePageText;
    bool m_signingIn = false;

    QPointer<QNetworkReply> m_refreshReply;
    QList<TokenCallback> m_pending;
};
}

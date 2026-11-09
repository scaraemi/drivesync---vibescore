// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QUrl>

#include <functional>

#include "drivetypes.h"
#include "itokenprovider.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace drivesync {
//! Minimal Google Drive API v3 client: the calls the sync engine needs.
//! All calls work on shared drives too (supportsAllDrives=true).
//! Callbacks are invoked on the thread that owns the client.
class DriveClient : public QObject
{
    Q_OBJECT

public:
    DriveClient(QNetworkAccessManager* nam, ITokenProvider* tokens, QObject* parent = nullptr);

    using FileCallback = std::function<void (Result<DriveFile>)>;
    using ListCallback = std::function<void (Result<QList<DriveFile> >)>;
    using Md5Callback = std::function<void (Result<QString>)>;

    //! For tests: point the client at a fake server.
    void setBaseUrl(const QUrl& base);
    void setPageSize(int size);
    void setRetryPolicy(int maxRetries, int baseDelayMs);

    void getFile(const QString& fileId, FileCallback callback);

    //! Non-trashed children of a folder; pages through the results.
    void listChildren(const QString& folderId, ListCallback callback);

    //! Streams the content to localPath, replacing it atomically. Returns the md5 of what was written.
    void download(const QString& fileId, const QString& localPath, Md5Callback callback);

    void createFile(const QString& parentId, const QString& name, const QString& localPath, FileCallback callback);
    void updateContent(const QString& fileId, const QString& localPath, FileCallback callback);
    void createFolder(const QString& parentId, const QString& name, FileCallback callback);
    void trash(const QString& fileId, FileCallback callback);

    //! The signed-in Google account: { displayName, emailAddress }
    using AboutCallback = std::function<void (Result<QPair<QString, QString> >)>;
    void about(AboutCallback callback);
    //! Renames (newName not empty) and/or moves (fromParentId -> toParentId, both set) a file or folder
    void moveRename(const QString& fileId, const QString& newName, const QString& fromParentId, const QString& toParentId,
                    FileCallback callback);

    //! Drive's native lock (contentRestrictions.readOnly). While set, nobody can change the content.
    void setLocked(const QString& fileId, bool locked, const QString& reason, FileCallback callback);

    static QString mimeTypeFor(const QString& fileName);

private:
    using RequestFactory = std::function<QNetworkReply* (const QString& token)>;
    using ReplyHandler = std::function<void (QNetworkReply* reply)>;
    using ErrorHandler = std::function<void (const DriveError& error)>;
    using Query = QList<QPair<QString, QString> >;

    void execute(RequestFactory makeReply, ReplyHandler onOk, ErrorHandler onError, int attempt = 0, bool authRetried = false,
                 bool retryable = true);

    QUrl apiUrl(const QString& path, Query query = {}) const;
    QUrl uploadUrl(const QString& path, Query query = {}) const;

    void jsonRequest(const QByteArray& verb, const QUrl& url, const QJsonObject& body, FileCallback callback);
    void uploadResumable(const QByteArray& verb, const QUrl& initUrl, const QJsonObject& metadata, const QString& localPath,
                         FileCallback callback, int attempt = 0);

    bool isRetryable(const DriveError& error) const;
    static DriveError parseError(QNetworkReply* reply, int status);

    QNetworkAccessManager* m_nam = nullptr;
    ITokenProvider* m_tokens = nullptr;
    QUrl m_base = QUrl("https://www.googleapis.com");
    int m_pageSize = 1000;
    int m_maxRetries = 4;
    int m_retryBaseDelayMs = 500;
};
}

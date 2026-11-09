// SPDX-License-Identifier: GPL-3.0-only
#include "driveclient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>

#include <memory>

using namespace drivesync;

static const QString FIELDS = QStringLiteral(
    "id,name,mimeType,md5Checksum,headRevisionId,modifiedTime,size,trashed,parents,"
    "contentRestrictions,lastModifyingUser(displayName,me)");

static int httpStatus(QNetworkReply* reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

static QNetworkRequest authorized(const QUrl& url, const QString& token)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    return request;
}

DriveClient::DriveClient(QNetworkAccessManager* nam, ITokenProvider* tokens, QObject* parent)
    : QObject(parent), m_nam(nam), m_tokens(tokens)
{
}

void DriveClient::setBaseUrl(const QUrl& base)
{
    m_base = base;
}

void DriveClient::setPageSize(int size)
{
    m_pageSize = size;
}

void DriveClient::setRetryPolicy(int maxRetries, int baseDelayMs)
{
    m_maxRetries = maxRetries;
    m_retryBaseDelayMs = baseDelayMs;
}

QString DriveClient::mimeTypeFor(const QString& fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == "mscz") {
        return QStringLiteral("application/x-musescore");
    }
    if (suffix == "mscx") {
        return QStringLiteral("application/x-musescore+xml");
    }

    const QMimeType type = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    return type.isDefault() ? QStringLiteral("application/octet-stream") : type.name();
}

// ---------------------------------------------------------------------------------------------------------------------
// Transport: token, 401 refresh, retries with exponential backoff

QUrl DriveClient::apiUrl(const QString& path, Query query) const
{
    query.append(qMakePair(QStringLiteral("supportsAllDrives"), QStringLiteral("true")));

    QStringList parts;
    for (const auto& item : query) {
        parts << QString::fromLatin1(QUrl::toPercentEncoding(item.first)) + "="
            + QString::fromLatin1(QUrl::toPercentEncoding(item.second));
    }

    QUrl url = m_base;
    url.setPath(path);
    url.setQuery(parts.join('&'));
    return url;
}

QUrl DriveClient::uploadUrl(const QString& path, Query query) const
{
    return apiUrl("/upload" + path, std::move(query));
}

bool DriveClient::isRetryable(const DriveError& e) const
{
    if (e.httpStatus == 0) {
        return e.reason == QLatin1String("network");
    }
    if (e.httpStatus == 429 || e.httpStatus >= 500) {
        return true;
    }
    return e.httpStatus == 403
           && (e.reason == QLatin1String("rateLimitExceeded") || e.reason == QLatin1String("userRateLimitExceeded"));
}

DriveError DriveClient::parseError(QNetworkReply* reply, int status)
{
    DriveError e;
    e.httpStatus = status;

    const QJsonObject err = QJsonDocument::fromJson(reply->readAll()).object().value("error").toObject();
    e.message = err.value("message").toString();
    const QJsonArray errors = err.value("errors").toArray();
    if (!errors.isEmpty()) {
        e.reason = errors.first().toObject().value("reason").toString();
    }

    if (status == 0) {
        e.reason = "network";
        e.message = reply->errorString();
    } else if (e.message.isEmpty()) {
        e.message = reply->errorString();
    }

    return e;
}

void DriveClient::execute(RequestFactory makeReply, ReplyHandler onOk, ErrorHandler onError, int attempt, bool authRetried,
                          bool retryable)
{
    m_tokens->accessToken([=](const QString& token, const DriveError& tokenError) {
        if (token.isEmpty()) {
            onError(tokenError);
            return;
        }

        QNetworkReply* reply = makeReply(token);
        if (!reply) {
            onError(DriveError { 0, "localIo", "Cannot read the local file" });
            return;
        }

        connect(reply, &QNetworkReply::finished, this, [=]() {
            reply->deleteLater();

            const int status = httpStatus(reply);
            if (status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError) {
                onOk(reply);
                return;
            }

            const DriveError error = parseError(reply, status);

            if (status == 401 && !authRetried) {
                m_tokens->invalidate(token);
                execute(makeReply, onOk, onError, attempt, true, retryable);
                return;
            }

            if (retryable && isRetryable(error) && attempt < m_maxRetries) {
                const int delay = m_retryBaseDelayMs * (1 << attempt);
                QTimer::singleShot(delay, this, [=]() {
                    execute(makeReply, onOk, onError, attempt + 1, authRetried, retryable);
                });
                return;
            }

            onError(error);
        });
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Metadata

void DriveClient::getFile(const QString& fileId, FileCallback callback)
{
    const QUrl url = apiUrl("/drive/v3/files/" + fileId, { { "fields", FIELDS } });

    execute([=](const QString& token) {
        return m_nam->get(authorized(url, token));
    }, [=](QNetworkReply* reply) {
        callback(Result<DriveFile>::success(DriveFile::fromJson(QJsonDocument::fromJson(reply->readAll()).object())));
    }, [=](const DriveError& e) {
        callback(Result<DriveFile>::failure(e));
    });
}

void DriveClient::listChildren(const QString& folderId, ListCallback callback)
{
    auto files = std::make_shared<QList<DriveFile> >();
    auto fetchPage = std::make_shared<std::function<void(const QString&)> >();

    *fetchPage = [=](const QString& pageToken) {
        Query query {
            { "q", QStringLiteral("'%1' in parents and trashed = false").arg(folderId) },
            { "fields", "nextPageToken,files(" + FIELDS + ")" },
            { "pageSize", QString::number(m_pageSize) },
            { "includeItemsFromAllDrives", "true" },
        };
        if (!pageToken.isEmpty()) {
            query.append(qMakePair(QStringLiteral("pageToken"), pageToken));
        }
        const QUrl url = apiUrl("/drive/v3/files", query);

        execute([=](const QString& token) {
            return m_nam->get(authorized(url, token));
        }, [=](QNetworkReply* reply) {
            const QJsonObject page = QJsonDocument::fromJson(reply->readAll()).object();
            for (const QJsonValue& v : page.value("files").toArray()) {
                files->append(DriveFile::fromJson(v.toObject()));
            }

            const QString next = page.value("nextPageToken").toString();
            if (next.isEmpty()) {
                callback(Result<QList<DriveFile> >::success(*files));
                *fetchPage = nullptr; // break the self-reference
            } else {
                (*fetchPage)(next);
            }
        }, [=](const DriveError& e) {
            callback(Result<QList<DriveFile> >::failure(e));
            *fetchPage = nullptr;
        });
    };

    (*fetchPage)(QString());
}

// ---------------------------------------------------------------------------------------------------------------------
// Content

void DriveClient::download(const QString& fileId, const QString& localPath, Md5Callback callback)
{
    const QUrl url = apiUrl("/drive/v3/files/" + fileId, { { "alt", "media" } });
    QDir().mkpath(QFileInfo(localPath).absolutePath());

    // One output per attempt; the target is replaced only when the whole file has arrived.
    struct Output {
        std::unique_ptr<QSaveFile> file;
        QCryptographicHash md5 { QCryptographicHash::Md5 };
        bool failed = false;
    };
    auto output = std::make_shared<Output>();

    execute([=](const QString& token) -> QNetworkReply* {
        output->file = std::make_unique<QSaveFile>(localPath);
        output->md5.reset();
        output->failed = !output->file->open(QIODevice::WriteOnly);
        if (output->failed) {
            return nullptr;
        }

        QNetworkReply* reply = m_nam->get(authorized(url, token));
        connect(reply, &QNetworkReply::readyRead, this, [=]() {
            if (httpStatus(reply) / 100 != 2) {
                return; // leave the error body for parseError
            }
            const QByteArray chunk = reply->readAll();
            output->md5.addData(chunk);
            if (output->file->write(chunk) != chunk.size()) {
                output->failed = true;
                reply->abort();
            }
        });
        return reply;
    }, [=](QNetworkReply* reply) {
        const QByteArray rest = reply->readAll();
        output->md5.addData(rest);
        output->file->write(rest);

        if (output->failed || !output->file->commit()) {
            callback(Result<QString>::failure(DriveError { 0, "localIo", "Cannot write " + localPath }));
            return;
        }
        callback(Result<QString>::success(QString::fromLatin1(output->md5.result().toHex())));
    }, [=](const DriveError& e) {
        if (output->file) {
            output->file->cancelWriting();
        }
        callback(Result<QString>::failure(output->failed
                                          ? DriveError { 0, "localIo", "Cannot write " + localPath }
                                          : e));
    });
}

void DriveClient::uploadResumable(const QByteArray& verb, const QUrl& initUrl, const QJsonObject& metadata,
                                  const QString& localPath, FileCallback callback, int attempt)
{
    const QFileInfo info(localPath);
    if (!info.isFile()) {
        callback(Result<DriveFile>::failure(DriveError { 0, "localIo", "File not found: " + localPath }));
        return;
    }

    const QString mime = mimeTypeFor(localPath);
    const QByteArray body = QJsonDocument(metadata).toJson(QJsonDocument::Compact);

    auto fail = [=](const DriveError& e) {
        callback(Result<DriveFile>::failure(e));
    };

    // Step 1: open an upload session
    execute([=](const QString& token) {
        QNetworkRequest request = authorized(initUrl, token);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
        request.setRawHeader("X-Upload-Content-Type", mime.toUtf8());
        request.setRawHeader("X-Upload-Content-Length", QByteArray::number(QFileInfo(localPath).size()));
        return m_nam->sendCustomRequest(request, verb, body);
    }, [=](QNetworkReply* reply) {
        const QUrl session = QUrl(QString::fromUtf8(reply->rawHeader("Location")));
        if (!session.isValid() || session.isEmpty()) {
            fail(DriveError { httpStatus(reply), "noSession", "No upload session URL" });
            return;
        }

        // Step 2: send the content. A failed transfer restarts from step 1 instead of
        // resuming, which is simpler and safe for the file sizes of scores and audio.
        execute([=](const QString& token) -> QNetworkReply* {
            auto* file = new QFile(localPath);
            if (!file->open(QIODevice::ReadOnly)) {
                delete file;
                return nullptr;
            }
            QNetworkRequest request = authorized(session, token);
            request.setHeader(QNetworkRequest::ContentTypeHeader, mime);
            request.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
            QNetworkReply* put = m_nam->put(request, file);
            file->setParent(put);
            return put;
        }, [=](QNetworkReply* put) {
            callback(Result<DriveFile>::success(DriveFile::fromJson(QJsonDocument::fromJson(put->readAll()).object())));
        }, [=](const DriveError& e) {
            if (isRetryable(e) && attempt < m_maxRetries) {
                QTimer::singleShot(m_retryBaseDelayMs * (1 << attempt), this, [=]() {
                    uploadResumable(verb, initUrl, metadata, localPath, callback, attempt + 1);
                });
                return;
            }
            fail(e);
        }, 0, false, /*retryable*/ false);
    }, fail);
}

void DriveClient::createFile(const QString& parentId, const QString& name, const QString& localPath, FileCallback callback)
{
    QJsonObject metadata {
        { "name", name },
        { "parents", QJsonArray { parentId } },
        { "mimeType", mimeTypeFor(name) },
    };
    const QUrl url = uploadUrl("/drive/v3/files", { { "uploadType", "resumable" }, { "fields", FIELDS } });
    uploadResumable("POST", url, metadata, localPath, std::move(callback));
}

void DriveClient::updateContent(const QString& fileId, const QString& localPath, FileCallback callback)
{
    const QUrl url = uploadUrl("/drive/v3/files/" + fileId, { { "uploadType", "resumable" }, { "fields", FIELDS } });
    uploadResumable("PATCH", url, QJsonObject(), localPath, std::move(callback));
}

// ---------------------------------------------------------------------------------------------------------------------
// Folders, trash, lock

void DriveClient::jsonRequest(const QByteArray& verb, const QUrl& url, const QJsonObject& body, FileCallback callback)
{
    const QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);

    execute([=](const QString& token) {
        QNetworkRequest request = authorized(url, token);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
        return m_nam->sendCustomRequest(request, verb, data);
    }, [=](QNetworkReply* reply) {
        callback(Result<DriveFile>::success(DriveFile::fromJson(QJsonDocument::fromJson(reply->readAll()).object())));
    }, [=](const DriveError& e) {
        callback(Result<DriveFile>::failure(e));
    });
}

void DriveClient::createFolder(const QString& parentId, const QString& name, FileCallback callback)
{
    jsonRequest("POST", apiUrl("/drive/v3/files", { { "fields", FIELDS } }),
                QJsonObject { { "name", name }, { "mimeType", FOLDER_MIME }, { "parents", QJsonArray { parentId } } },
                std::move(callback));
}

void DriveClient::trash(const QString& fileId, FileCallback callback)
{
    jsonRequest("PATCH", apiUrl("/drive/v3/files/" + fileId, { { "fields", FIELDS } }),
                QJsonObject { { "trashed", true } }, std::move(callback));
}

void DriveClient::about(AboutCallback callback)
{
    const QUrl url = apiUrl("/drive/v3/about", { { "fields", "user(displayName,emailAddress)" } });

    execute([=](const QString& token) {
        return m_nam->get(authorized(url, token));
    }, [=](QNetworkReply* reply) {
        const QJsonObject user = QJsonDocument::fromJson(reply->readAll()).object().value("user").toObject();
        callback(Result<QPair<QString, QString> >::success(
                     qMakePair(user.value("displayName").toString(), user.value("emailAddress").toString())));
    }, [=](const DriveError& e) {
        callback(Result<QPair<QString, QString> >::failure(e));
    });
}

void DriveClient::moveRename(const QString& fileId, const QString& newName, const QString& fromParentId,
                             const QString& toParentId, FileCallback callback)
{
    Query query { { "fields", FIELDS } };
    if (!fromParentId.isEmpty() && !toParentId.isEmpty() && fromParentId != toParentId) {
        query.append(qMakePair(QStringLiteral("addParents"), toParentId));
        query.append(qMakePair(QStringLiteral("removeParents"), fromParentId));
    }
    QJsonObject body;
    if (!newName.isEmpty()) {
        body.insert("name", newName);
    }
    jsonRequest("PATCH", apiUrl("/drive/v3/files/" + fileId, query), body, std::move(callback));
}

void DriveClient::setLocked(const QString& fileId, bool locked, const QString& reason, FileCallback callback)
{
    QJsonObject restriction { { "readOnly", locked } };
    if (locked && !reason.isEmpty()) {
        restriction.insert("reason", reason);
    }

    jsonRequest("PATCH", apiUrl("/drive/v3/files/" + fileId, { { "fields", FIELDS } }),
                QJsonObject { { "contentRestrictions", QJsonArray { restriction } } }, std::move(callback));
}

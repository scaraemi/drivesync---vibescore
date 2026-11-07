// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>

#include "drivetypes.h"

namespace drivesync {
//! Supplies OAuth access tokens to DriveClient.
class ITokenProvider
{
public:
    virtual ~ITokenProvider() = default;

    using TokenCallback = std::function<void (const QString& token, const DriveError& error)>;

    //! Calls back with a valid token (refreshing it if needed), or an empty token and the error.
    virtual void accessToken(TokenCallback callback) = 0;

    //! The server rejected this token (HTTP 401): the next accessToken() must not return it.
    virtual void invalidate(const QString& token) = 0;
};

//! A fixed token, for development and tests.
class StaticTokenProvider : public ITokenProvider
{
public:
    explicit StaticTokenProvider(QString token)
        : m_token(std::move(token)) {}

    void accessToken(TokenCallback callback) override
    {
        if (m_token.isEmpty()) {
            callback(QString(), DriveError { 0, "noToken", "No access token" });
            return;
        }
        callback(m_token, DriveError());
    }

    void invalidate(const QString& token) override
    {
        if (token == m_token) {
            m_token.clear();
        }
    }

private:
    QString m_token;
};
}

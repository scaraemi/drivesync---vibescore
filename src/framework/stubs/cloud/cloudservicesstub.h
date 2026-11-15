/*
 * SPDX-License-Identifier: GPL-3.0-only
 * VibeScore - MuseScore Studio for mobile devices
 *
 * Copyright (C) 2026 VibeScore contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "cloud/musescorecom/imusescorecomservice.h"
#include "cloud/musescorecom/imusescorecomconvertservice.h"
#include "cloud/audiocom/iaudiocomservice.h"

namespace muse::cloud {
//! Used when the cloud module is disabled: every request fails with "not supported",
//! so that callers never have to deal with missing services
class MuseScoreComConvertServiceStub : public IMuseScoreComConvertService
{
public:
    async::Promise<RetVal<ConvertConfig> > fetchConfig() override;
    ProgressPtr uploadToConvert(const ConvertUploadDataPtr& data) override;
    async::Promise<RetVal<ConvertQueueList> > fetchQueue() override;
    async::Promise<RetVal<ConvertResult> > submitReview(ConvertType type, int id, ReviewRating review, const QString& comment) override;
    async::Promise<Ret> submitReviewComment(ConvertType type, int id, const QString& comment) override;
    async::Promise<Ret> deleteConversion(ConvertType type, int id) override;
};

class MuseScoreComServiceStub : public IMuseScoreComService
{
public:
    MuseScoreComServiceStub(IAuthorizationServicePtr authorization);

    IAuthorizationServicePtr authorization() override;
    IMuseScoreComConvertServicePtr convert() override;

    QUrl scoreManagerUrl() const override;

    ProgressPtr uploadScore(DevicePtr scoreData, const QString& title, Visibility visibility, const QUrl& sourceUrl,
                            int revisionId) override;
    ProgressPtr uploadAudio(DevicePtr audioData, const QString& audioFormat, const QUrl& sourceUrl) override;

    RetVal<ScoreInfo> downloadScoreInfo(const QUrl& sourceUrl) override;
    RetVal<ScoreInfo> downloadScoreInfo(int scoreId) override;

    async::Promise<ScoresList> downloadScoresList(int scoresPerBatch, int batchNumber) override;
    ProgressPtr downloadScore(int scoreId, DevicePtr scoreData, const QString& hash, const QString& secret) override;

    async::Promise<Ret> deleteScore(int scoreId) override;

private:
    IAuthorizationServicePtr m_authorization;
    IMuseScoreComConvertServicePtr m_convert;
};

class AudioComServiceStub : public IAudioComService
{
public:
    AudioComServiceStub(IAuthorizationServicePtr authorization);

    IAuthorizationServicePtr authorization() override;
    QUrl projectManagerUrl() const override;
    ProgressPtr uploadAudio(DevicePtr audioData, const QString& audioFormat, const QString& title, const QUrl& existingUrl,
                            Visibility visibility, bool replaceExisting) override;
    CloudInfo cloudInfo() const override;

private:
    IAuthorizationServicePtr m_authorization;
};
}

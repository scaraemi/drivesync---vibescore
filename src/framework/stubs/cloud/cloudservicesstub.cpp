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
#include "cloudservicesstub.h"

#include "global/async/async.h"

using namespace muse;
using namespace muse::cloud;

//! Fails on the next event loop iteration, after the caller has subscribed to the progress
static ProgressPtr failedProgress()
{
    ProgressPtr progress = std::make_shared<Progress>();
    async::Async::call(nullptr, [progress]() {
        progress->start();
        progress->finish(make_ret(Ret::Code::NotSupported));
    });
    return progress;
}

template<typename T>
static async::Promise<RetVal<T> > notSupportedPromise()
{
    return async::make_promise<RetVal<T> >([](auto resolve) {
        return resolve(RetVal<T>::make_ret(Ret::Code::NotSupported));
    });
}

static async::Promise<Ret> notSupportedRetPromise()
{
    return async::make_promise<Ret>([](auto resolve) {
        return resolve(make_ret(Ret::Code::NotSupported));
    });
}

// ---- MuseScoreComConvertServiceStub ----

async::Promise<RetVal<ConvertConfig> > MuseScoreComConvertServiceStub::fetchConfig()
{
    return notSupportedPromise<ConvertConfig>();
}

ProgressPtr MuseScoreComConvertServiceStub::uploadToConvert(const ConvertUploadDataPtr&)
{
    return failedProgress();
}

async::Promise<RetVal<ConvertQueueList> > MuseScoreComConvertServiceStub::fetchQueue()
{
    return notSupportedPromise<ConvertQueueList>();
}

async::Promise<RetVal<ConvertResult> > MuseScoreComConvertServiceStub::submitReview(ConvertType, int, ReviewRating, const QString&)
{
    return notSupportedPromise<ConvertResult>();
}

async::Promise<Ret> MuseScoreComConvertServiceStub::submitReviewComment(ConvertType, int, const QString&)
{
    return notSupportedRetPromise();
}

async::Promise<Ret> MuseScoreComConvertServiceStub::deleteConversion(ConvertType, int)
{
    return notSupportedRetPromise();
}

// ---- MuseScoreComServiceStub ----

MuseScoreComServiceStub::MuseScoreComServiceStub(IAuthorizationServicePtr authorization)
    : m_authorization(authorization), m_convert(std::make_shared<MuseScoreComConvertServiceStub>())
{
}

IAuthorizationServicePtr MuseScoreComServiceStub::authorization()
{
    return m_authorization;
}

IMuseScoreComConvertServicePtr MuseScoreComServiceStub::convert()
{
    return m_convert;
}

QUrl MuseScoreComServiceStub::scoreManagerUrl() const
{
    return QUrl();
}

ProgressPtr MuseScoreComServiceStub::uploadScore(DevicePtr, const QString&, Visibility, const QUrl&, int)
{
    return failedProgress();
}

ProgressPtr MuseScoreComServiceStub::uploadAudio(DevicePtr, const QString&, const QUrl&)
{
    return failedProgress();
}

RetVal<ScoreInfo> MuseScoreComServiceStub::downloadScoreInfo(const QUrl&)
{
    return RetVal<ScoreInfo>::make_ret(Ret::Code::NotSupported);
}

RetVal<ScoreInfo> MuseScoreComServiceStub::downloadScoreInfo(int)
{
    return RetVal<ScoreInfo>::make_ret(Ret::Code::NotSupported);
}

async::Promise<ScoresList> MuseScoreComServiceStub::downloadScoresList(int, int)
{
    return async::make_promise<ScoresList>([](auto, auto reject) {
        const Ret ret = make_ret(Ret::Code::NotSupported);
        return reject(ret.code(), ret.text());
    });
}

ProgressPtr MuseScoreComServiceStub::downloadScore(int, DevicePtr, const QString&, const QString&)
{
    return failedProgress();
}

// ---- AudioComServiceStub ----

AudioComServiceStub::AudioComServiceStub(IAuthorizationServicePtr authorization)
    : m_authorization(authorization)
{
}

IAuthorizationServicePtr AudioComServiceStub::authorization()
{
    return m_authorization;
}

QUrl AudioComServiceStub::projectManagerUrl() const
{
    return QUrl();
}

ProgressPtr AudioComServiceStub::uploadAudio(DevicePtr, const QString&, const QString&, const QUrl&, Visibility, bool)
{
    return failedProgress();
}

CloudInfo AudioComServiceStub::cloudInfo() const
{
    return CloudInfo();
}

async::Promise<Ret> MuseScoreComServiceStub::deleteScore(int)
{
    return notSupportedRetPromise();
}

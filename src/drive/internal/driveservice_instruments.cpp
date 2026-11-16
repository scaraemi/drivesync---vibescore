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
// Instruments made from samples. The samples go to the Drive folder "Nuovo strumento"; a GitHub Actions job
// (services/samplevst) makes a SoundFont and a VST3 of them in "Strumenti"; here they are installed: the SoundFont
// on every device, the VST3 on computers, in the folder where VST3 hosts look for the user's own plug-ins.

#include "driveservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include "core/driveclient.h"
#include "core/drivetypes.h"

#include "global/serialization/zipreader.h"

#include "translation.h"
#include "log.h"

using namespace mu::drive;
using namespace muse;

static const QString MANIFEST_NAME = QStringLiteral("strumento.json");
static const QString INSTALLED_KEY = QStringLiteral("drive/instruments/");   // + file name -> md5 installed

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(Q_OS_WASM)
static constexpr bool HAS_VST3 = false;
#else
static constexpr bool HAS_VST3 = true;
#endif

//! The user's own VST3 folder, searched by MuseScore and the other VST3 hosts without any setting
static QString userVst3Folder()
{
#if defined(Q_OS_WIN)
    // FOLDERID_UserProgramFilesCommon: %LOCALAPPDATA%\Programs\Common
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Programs/Common/VST3");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Audio/Plug-Ins/VST3");
#else
    return QDir::homePath() + QStringLiteral("/.vst3");
#endif
}

static QString cleanName(QString name)
{
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("-"));
    name = name.simplified();
    while (name.endsWith('.')) {
        name.chop(1);
    }
    return name.left(60);
}

QString DriveService::instrumentNameFromFiles(const QStringList& files)
{
    // Tokens that name a note, a dynamic or a take, not the instrument: "A4", "C#3", "sol3", "ff", "02"
    static const QRegularExpression notToken(
        QStringLiteral("^((do|re|mi|fa|sol|la|si|[a-g])[#b]?-?\\d|\\d+|p+|f+|mp|mf|sfz|vel\\d*|v\\d+|rr\\d*|take\\d*)$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sep(QStringLiteral("[\\s_\\-.()\\[\\]]+"));

    QList<QStringList> all;
    for (const QString& f : files) {
        QStringList words;
        for (const QString& w : QFileInfo(f).completeBaseName().split(sep, Qt::SkipEmptyParts)) {
            if (!notToken.match(w).hasMatch()) {
                words << w;
            }
        }
        all << words;
    }
    if (all.isEmpty()) {
        return QStringLiteral("Strumento");
    }
    // the words all the files start with ("Violino" of "Violino A4", "Violino C5"), else the first file's
    QStringList common = all.first();
    for (const QStringList& w : all) {
        int n = 0;
        while (n < common.size() && n < w.size() && common[n].compare(w[n], Qt::CaseInsensitive) == 0) {
            ++n;
        }
        common = common.mid(0, n);
    }
    if (common.isEmpty()) {
        common = all.first();
    }
    QString name = cleanName(common.join(' '));
    if (name.isEmpty()) {
        return QStringLiteral("Strumento");
    }
    name[0] = name[0].toUpper();
    return name;
}

void DriveService::makeInstrument(const QString& name)
{
    const std::string title = muse::trc("drive", "Instrument from samples");
    if (m_status.state == State::NotSignedIn || m_status.state == State::SigningIn || m_status.state == State::NoFolder
        || !m_client) {
        const std::string text = muse::trc("drive",
                                           "Instruments are made through Drive: sign in and choose the Drive folder first, in the Drive tab of the Scores page.");
        interactive()->info(title, IInteractive::Text(text));
        return;
    }

    // No name filter: Android would hide the files whose type it does not know
    const io::paths_t paths = interactive()->selectOpeningFilesSync(
        muse::trc("drive", "Samples for the instrument: one note, several notes, drums or effects"), io::path_t(), {});
    QStringList files;
    for (const io::path_t& p : paths) {
        files << p.toQString();
    }
    if (files.isEmpty()) {
        return;
    }
    const QString instrument = cleanName(name).isEmpty() ? instrumentNameFromFiles(files) : cleanName(name);
    const QString root = folderId();

    withFolder(root, instrumentRequestsFolderName(), [this, files, instrument](const QString& inbox, const QString& error) {
        if (inbox.isEmpty()) {
            showError(muse::trc("drive", "The samples were not sent"), error);
            return;
        }
        m_client->createFolder(inbox, instrument, [this, files, instrument](drivesync::Result<drivesync::DriveFile> folder) {
            if (!folder.ok()) {
                showError(muse::trc("drive", "The samples were not sent"), folder.error.toString());
                return;
            }
            const QString folderId = folder.value->id;
            uploadFiles(folderId, files, {}, {}, [this, folderId, instrument](const QStringList& sent, const QStringList& failed) {
                if (sent.isEmpty()) {
                    showError(muse::trc("drive", "The samples were not sent"), failed.join("\n"));
                    return;
                }
                // Written last: the request is complete
                const QString manifest = QDir::tempPath() + QStringLiteral("/vs-") + MANIFEST_NAME;
                QFile f(manifest);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(QJsonDocument(QJsonObject { { QStringLiteral("nome"), instrument } }).toJson());
                    f.close();
                }
                m_client->createFile(folderId, MANIFEST_NAME, manifest,
                                     [this, manifest, instrument, sent, failed](drivesync::Result<drivesync::DriveFile> r) {
                    QFile::remove(manifest);
                    if (!r.ok()) {
                        showError(muse::trc("drive", "The samples were not sent"), r.error.toString());
                        return;
                    }
                    //: %1 is the instrument's name, %2 a list of file names
                    QString text = muse::qtrc("drive",
                                              "“%1” is being made from %2.\n\nIn about 10–20 minutes it is installed on this device and the others: as a SoundFont, and on computers as a VST3 instrument too. Choose it in the Mixer, as the sound of an instrument.")
                                   .arg(instrument, sent.join(", "));
                    if (!failed.isEmpty()) {
                        text += "\n\n" + muse::qtrc("drive", "Not sent:") + " " + failed.join(", ");
                    }
                    interactive()->info(muse::trc("drive", "Samples sent"), IInteractive::Text(text.toStdString()));
                    m_instrumentsChecked = QDateTime();   // look for it as soon as the next sync is done
                });
            });
        });
    });
}

void DriveService::withFolder(const QString& parentId, const QString& name,
                              std::function<void(const QString& id, const QString& error)> done)
{
    m_client->listChildren(parentId, [this, parentId, name, done](drivesync::Result<QList<drivesync::DriveFile> > list) {
        if (!list.ok()) {
            done(QString(), list.error.toString());
            return;
        }
        for (const drivesync::DriveFile& f : *list.value) {
            if (f.isFolder() && !f.trashed && f.name == name) {
                done(f.id, QString());
                return;
            }
        }
        m_client->createFolder(parentId, name, [done](drivesync::Result<drivesync::DriveFile> folder) {
            done(folder.ok() ? folder.value->id : QString(), folder.ok() ? QString() : folder.error.toString());
        });
    });
}

void DriveService::uploadFiles(const QString& folderId, QStringList files, QStringList sent, QStringList failed,
                               std::function<void(const QStringList& sent, const QStringList& failed)> done)
{
    if (files.isEmpty()) {
        done(sent, failed);
        return;
    }
    const QString path = files.takeFirst();
    const QString name = QFileInfo(path).fileName();
    m_client->createFile(folderId, name, path,
                         [this, folderId, files, sent, failed, name, done](drivesync::Result<drivesync::DriveFile> r) mutable {
        if (r.ok()) {
            sent << name;
        } else {
            failed << name + ": " + r.error.toString();
        }
        uploadFiles(folderId, files, sent, failed, done);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Installing what is in "Strumenti"

void DriveService::checkInstruments(bool force)
{
    if (m_instrumentsBusy || !m_client || folderId().isEmpty()) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!force && m_instrumentsChecked.isValid() && m_instrumentsChecked.secsTo(now) < 300) {
        return;
    }
    m_instrumentsChecked = now;
    m_instrumentsBusy = true;

    m_client->listChildren(folderId(), [this](drivesync::Result<QList<drivesync::DriveFile> > list) {
        QString outId;
        if (list.ok()) {
            for (const drivesync::DriveFile& f : *list.value) {
                if (f.isFolder() && !f.trashed && f.name == instrumentsFolderName()) {
                    outId = f.id;
                }
            }
        }
        if (outId.isEmpty()) {   // no instrument made yet (or offline): nothing to do
            m_instrumentsBusy = false;
            return;
        }
        m_client->listChildren(outId, [this](drivesync::Result<QList<drivesync::DriveFile> > files) {
            if (!files.ok()) {
                m_instrumentsBusy = false;
                return;
            }
            QSettings settings;
            for (const drivesync::DriveFile& f : *files.value) {
                if (f.isFolder() || f.trashed || f.md5.isEmpty()) {
                    continue;
                }
                const bool sf2 = f.name.endsWith(QStringLiteral(".sf2"), Qt::CaseInsensitive);
                const bool vst3 = HAS_VST3 && f.name.endsWith(QStringLiteral(".vst3.zip"), Qt::CaseInsensitive);
                if ((sf2 || vst3) && settings.value(INSTALLED_KEY + f.name).toString() != f.md5) {
                    m_instrumentQueue.append({ f.id, f.name, f.md5 });
                }
            }
            installNextInstrument();
        });
    });
}

void DriveService::installNextInstrument()
{
    if (m_instrumentQueue.isEmpty()) {
#ifdef MUSE_MODULE_AUDIOPLUGINS
        if (!m_newVst3Bundles.isEmpty()) {
            // The app's list of plug-ins: what the Mixer offers
            if (auto scenario = registerAudioPluginsScenario()) {
                io::paths_t paths;
                for (const QString& p : m_newVst3Bundles) {
                    paths.push_back(io::path_t(p));
                }
                scenario->registerNewPlugins(paths);
            }
        }
#endif
        m_newVst3Bundles.clear();
        if (!m_instrumentsInstalled.isEmpty()) {
            QStringList names = m_instrumentsInstalled;
            names.removeDuplicates();
            //: %1 is a list of instrument names
            QString text = muse::qtrc("drive",
                                      "From the Drive folder: %1.\n\nChoose them in the Mixer, as the sound of an instrument (SoundFont, or VST3 instrument on a computer).")
                           .arg(names.join(", "));
            if (!m_instrumentsFailed.isEmpty()) {
                text += "\n\n" + muse::qtrc("drive", "Not installed:") + "\n" + m_instrumentsFailed.join("\n");
            }
            interactive()->info(muse::trc("drive", "New instruments"), IInteractive::Text(text.toStdString()));
        } else if (!m_instrumentsFailed.isEmpty()) {
            LOGW() << "Drive instruments not installed: " << m_instrumentsFailed.join("; ").toStdString();
        }
        m_instrumentsInstalled.clear();
        m_instrumentsFailed.clear();
        m_instrumentsBusy = false;
        return;
    }

    const InstrumentFile file = m_instrumentQueue.takeFirst();
    // the file keeps its name: the SoundFont is installed under it
    const QString tmp = QDir::tempPath() + QStringLiteral("/vs-instruments/") + file.name;
    m_client->download(file.id, tmp, [this, file, tmp](drivesync::Result<QString> md5) {
        QString error;
        if (!md5.ok()) {
            error = md5.error.toString();
        } else if (installInstrument(file.name, tmp, error)) {
            QSettings().setValue(INSTALLED_KEY + file.name, file.md5);
            QString name = file.name;
            name.chop(name.endsWith(QStringLiteral(".vst3.zip"), Qt::CaseInsensitive) ? 9 : 4);
            m_instrumentsInstalled << name;
        }
        if (!error.isEmpty()) {
            LOGW() << "Drive instrument " << file.name.toStdString() << ": " << error.toStdString();
            m_instrumentsFailed << file.name + ": " + error;
        }
        QFile::remove(tmp);
        installNextInstrument();
    });
}

bool DriveService::installInstrument(const QString& name, const QString& downloaded, QString& error)
{
    if (name.endsWith(QStringLiteral(".sf2"), Qt::CaseInsensitive)) {
        auto soundFonts = soundFontController();
        if (!soundFonts) {
            error = muse::qtrc("drive", "SoundFonts are not available in this app");
            return false;
        }
        RetVal<io::path_t> installed = soundFonts->installSoundFont(io::path_t(downloaded));
        if (!installed.ret) {
            error = QString::fromStdString(installed.ret.toString());
            return false;
        }
        LOGI() << "Drive instrument installed: " << installed.val;
        return true;
    }
    QString bundle;
    if (!installVst3(downloaded, bundle, error)) {
        return false;
    }
    LOGI() << "Drive VST3 instrument installed: " << bundle.toStdString();
    return true;
}

bool DriveService::installVst3(const QString& zipPath, QString& bundlePath, QString& error)
{
    const QString dir = userVst3Folder();
    muse::ZipReader zip { io::path_t(zipPath) };
    if (zip.hasError()) {
        error = muse::qtrc("drive", "The VST3 archive cannot be read");
        return false;
    }
    QString bundleName;
    QSet<QString> inZip;
    QList<muse::ZipReader::FileInfo> entries;
    for (const auto& fi : zip.fileInfoList()) {
        const QString rel = fi.filePath.toQString();
        // only "<Name>.vst3/..." entries, nothing that climbs out of the folder
        if (rel.startsWith('/') || rel.contains(QStringLiteral("..")) || rel.contains(':')) {
            continue;
        }
        const QString top = rel.section('/', 0, 0);
        if (!top.endsWith(QStringLiteral(".vst3"))) {
            continue;
        }
        if (bundleName.isEmpty()) {
            bundleName = top;
        } else if (top != bundleName) {
            continue;
        }
        if (fi.isFile) {
            inZip.insert(rel);
            entries << fi;
        }
    }
    if (bundleName.isEmpty()) {
        error = muse::qtrc("drive", "The archive holds no VST3 plug-in");
        return false;
    }
    bundlePath = dir + '/' + bundleName;
    const bool isNew = !QFileInfo::exists(bundlePath);

    for (const auto& fi : entries) {
        const QString rel = fi.filePath.toQString();
        const QString dst = dir + '/' + rel;
        const QByteArray data = zip.fileData(rel.toStdString()).toQByteArray();
        // Unchanged files are left alone: the plug-in's module may be loaded (Windows locks it)
        QFile old(dst);
        if (old.size() == data.size() && old.open(QIODevice::ReadOnly) && old.readAll() == data) {
            continue;
        }
        old.close();
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QSaveFile out(dst);
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
            error = muse::qtrc("drive", "Cannot write %1 (close the programs using the instrument and sync again)").arg(dst);
            return false;
        }
        if (rel.contains(QStringLiteral("/Contents/MacOS/")) || rel.endsWith(QStringLiteral(".so"))) {
            QFile::setPermissions(dst, QFile::permissions(dst) | QFileDevice::ExeOwner | QFileDevice::ExeGroup
                                  | QFileDevice::ExeOther | QFileDevice::ReadOwner);
        }
    }
    // Samples of an older version that the new one does not use
    const QString samples = bundlePath + QStringLiteral("/Contents/Resources/samples");
    for (const QFileInfo& f : QDir(samples).entryInfoList(QDir::Files)) {
        if (!inZip.contains(bundleName + QStringLiteral("/Contents/Resources/samples/") + f.fileName())) {
            QFile::remove(f.absoluteFilePath());
        }
    }
    if (isNew) {
        m_newVst3Bundles << bundlePath;
    }
    return true;
}

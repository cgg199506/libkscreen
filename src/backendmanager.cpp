/*
 * Copyright (C) 2014  Daniel Vratil <dvratil@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "backendmanager_p.h"

#include "abstractbackend.h"
#include "backendinterface.h"
#include "debug_p.h"
#include "getconfigoperation.h"
#include "configserializer_p.h"

#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusConnectionInterface>
#include <QStandardPaths>
#include <QThread>
#include <QX11Info>

#include <memory>


using namespace KScreen;

Q_DECLARE_METATYPE(org::kde::kscreen::Backend*)

const int BackendManager::sMaxCrashCount = 4;

BackendManager *BackendManager::sInstance = 0;

BackendManager *BackendManager::instance()
{
    if (!sInstance) {
        sInstance = new BackendManager();
    }

    return sInstance;
}

BackendManager::BackendManager()
    : QObject()
    , mInterface(0)
    , mCrashCount(0)
    , mShuttingDown(false)
    , mRequestsCounter(0)
    , mInProcessBackend(0)
    , mLoader(0)
    , mMode(OutOfProcess)
{
    if (qgetenv("KSCREEN_BACKEND_INPROCESS") == QByteArray("1")) {
        mMode = InProcess;
    }
    initMode(true);
}

void BackendManager::initMode(bool fromctor)
{
//     bool envmode_inprocess  = (qgetenv("KSCREEN_BACKEND_INPROCESS") == QByteArray("1"));
//     if (envmode_inprocess && (mMode == InProcess) && !fromctor) {
//         return;
//     }
    if (mMode == OutOfProcess) {
        qDebug() << "Setting up dbus";
        qRegisterMetaType<org::kde::kscreen::Backend*>("OrgKdeKscreenBackendInterface");

        mServiceWatcher.setConnection(QDBusConnection::sessionBus());
        connect(&mServiceWatcher, &QDBusServiceWatcher::serviceUnregistered,
                this, &BackendManager::backendServiceUnregistered);

        mResetCrashCountTimer.setSingleShot(true);
        mResetCrashCountTimer.setInterval(60000);
        connect(&mResetCrashCountTimer, &QTimer::timeout,
                this, [=]() {
                    mCrashCount = 0;
                });
    } else {

    }
}

void BackendManager::setMode(BackendManager::Mode m)
{
    if (mMode == m) {
        return;
    }
    qDebug() << "Switching mode ============================" << (m == InProcess);
    shutdownBackend();
    mMode = m;
    initMode();
}

BackendManager::Mode BackendManager::mode() const
{
    return mMode;
}

BackendManager::~BackendManager()
{
    qDebug() << "BackendManager gone...";
}

KScreen::AbstractBackend *BackendManager::loadBackend(QPluginLoader *loader, const QString &name,
                                                     const QVariantMap &arguments)
{
    qCDebug(KSCREEN) << "Requested backend:" << name;
    const QString backendFilter = QString::fromLatin1("KSC_%1*").arg(name);
    const QStringList paths = QCoreApplication::libraryPaths();
    //qCDebug(KSCREEN) << "Lookup paths: " << paths;
    Q_FOREACH (const QString &path, paths) {
        const QDir dir(path + QLatin1String("/kf5/kscreen/"),
                       backendFilter,
                       QDir::SortFlags(QDir::QDir::NoSort),
                       QDir::NoDotAndDotDot | QDir::Files);
        const QFileInfoList finfos = dir.entryInfoList();
        Q_FOREACH (const QFileInfo &finfo, finfos) {
            //qDebug() << "path:" << finfo.path();
            // Skip "Fake" backend unless explicitly specified via KSCREEN_BACKEND
            if (name.isEmpty() && (finfo.fileName().contains(QLatin1String("KSC_Fake")) || finfo.fileName().contains(QLatin1String("KSC_FakeUI")))) {
                continue;
            }

            // When on X11, skip the QScreen backend, instead use the XRandR backend,
            // if not specified in KSCREEN_BACKEND
            if (name.isEmpty() &&
                finfo.fileName().contains(QLatin1String("KSC_QScreen")) &&
                QX11Info::isPlatformX11()) {
                continue;
            }
            if (name.isEmpty() &&
                finfo.fileName().contains(QLatin1String("KSC_Wayland"))) {
                continue;
            }

            // When not on X11, skip the XRandR backend, and fall back to QSCreen
            // if not specified in KSCREEN_BACKEND
            if (name.isEmpty() &&
                finfo.fileName().contains(QLatin1String("KSC_XRandR")) &&
                !QX11Info::isPlatformX11()) {
                continue;
            }

            //qCDebug(KSCREEN) << "Trying" << finfo.filePath() << loader->isLoaded();
            // Make sure we unload() and delete the loader whenever it goes out of scope here
//             std::unique_ptr<QPluginLoader, void(*)(QPluginLoader *)> loader(new QPluginLoader(finfo.filePath()), pluginDeleter);
            loader->setFileName(finfo.filePath());
            QObject *instance = loader->instance();
            if (!instance) {
                qCDebug(KSCREEN) << loader->errorString();
                continue;
            }

            auto backend = qobject_cast<KScreen::AbstractBackend*>(instance);
            if (backend) {
                backend->init(arguments);
                if (!backend->isValid()) {
                    qCDebug(KSCREEN) << "Skipping" << backend->name() << "backend";
                    delete backend;
                    continue;
                }

                // This is the only case we don't want to unload() and delete the loader, instead
                // we store it and unload it when the backendloader terminates.
                //mLoader = loader.release();
                //loader.release();
//                     connect(backend, &QObject::destroyed, [loader]() {
//                         //loader.release();
//                     });
                qCDebug(KSCREEN) << "Loading" << backend->name() << "backend";
                return backend;
            } else {
                qCDebug(KSCREEN) << finfo.fileName() << "does not provide valid KScreen backend";
            }
        }
    }

    return Q_NULLPTR;
}

KScreen::AbstractBackend *BackendManager::loadBackend(const QString &name,
                                                      const QVariantMap &arguments)
{
    qDebug() << "BACKENDS CACHED:" << m_inProcessBackends.keys();
    if (mMode == OutOfProcess) {
        qWarning(KSCREEN) << "You are trying to load a backend in process, while the BackendManager is set to use OutOfProcess communication. Use the static version of loadBackend instead.";
        return nullptr;
    }
    Q_ASSERT(mMode == InProcess);
    if (m_inProcessBackends.keys().contains(name)) {
        auto pair = m_inProcessBackends[name];
        auto _backend = pair.first;
        if (pair.second != arguments) {
            _backend->init(arguments);
        }
        qDebug() << " Backend CACHED. :) =========" << name;
        return _backend;

    }
    qDebug() << " Backend fresh. =========" << name;
    if (mLoader == nullptr) {
        qDebug() << "New QPluginLoader...";
        mLoader = new QPluginLoader(this);
    }
    auto backend = BackendManager::loadBackend(mLoader, name, arguments);
    m_inProcessBackends[name] = qMakePair<KScreen::AbstractBackend*, QVariantMap>(backend, arguments);
    return backend;
}

void BackendManager::requestBackend()
{
    if (mInterface && mInterface->isValid()) {
        ++mRequestsCounter;
        QMetaObject::invokeMethod(this, "emitBackendReady", Qt::QueuedConnection);
        return;
    }

    // Another request already pending
    if (mRequestsCounter > 0) {
        return;
    }
    ++mRequestsCounter;

    const QByteArray args = qgetenv("KSCREEN_BACKEND_ARGS");
    QVariantMap arguments;
    if (!args.isEmpty()) {
        QList<QByteArray> arglist = args.split(';');
        Q_FOREACH (const QByteArray &arg, arglist) {
            const int pos = arg.indexOf('=');
            if (pos == -1) {
                continue;
            }
            arguments.insert(arg.left(pos), arg.mid(pos + 1));
        }
    }
    qDebug() << "OOP LOADING BACKEND";
    startBackend(QString::fromLatin1(qgetenv("KSCREEN_BACKEND")), arguments);
}

void BackendManager::emitBackendReady()
{
    Q_EMIT backendReady(mInterface);
    --mRequestsCounter;
    if (mShutdownLoop.isRunning()) {
        mShutdownLoop.quit();
    }
}

void BackendManager::startBackend(const QString &backend, const QVariantMap &arguments)
{
    qDebug() << "startBackend!" << backend;
    // This will autostart the launcher if it's not running already, calling
    // requestBackend(backend) will:
    //   a) if the launcher is started it will force it to load the correct backend,
    //   b) if the launcher is already running it will make sure it's running with
    //      the same backend as the one we requested and send an error otherwise
    QDBusConnection conn = QDBusConnection::sessionBus();
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KScreen"),
                                                       QStringLiteral("/"),
                                                       QStringLiteral("org.kde.KScreen"),
                                                       QStringLiteral("requestBackend"));
    call.setArguments({ backend, arguments });
    QDBusPendingCall pending = conn.asyncCall(call);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pending);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &BackendManager::onBackendRequestDone);
}

void BackendManager::onBackendRequestDone(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();
    QDBusPendingReply<bool> reply = *watcher;
    // Most probably we requested an explicit backend that is different than the
    // one already loaded in the launcher
    if (reply.isError()) {
        qCWarning(KSCREEN) << "Failed to request backend:" << reply.error().name() << ":" << reply.error().message();
        invalidateInterface();
        emitBackendReady();
        return;
    }

    // Most probably request and explicit backend which is not available or failed
    // to initialize, or the launcher did not find any suitable backend for the
    // current platform.
    if (!reply.value()) {
        qCWarning(KSCREEN) << "Failed to request backend: unknown error";
        invalidateInterface();
        emitBackendReady();
        return;
    }

    // The launcher has successfully loaded the backend we wanted and registered
    // it to DBus (hopefuly), let's try to get an interface for the backend.
    if (mInterface) {
        invalidateInterface();
    }
    mInterface = new org::kde::kscreen::Backend(QStringLiteral("org.kde.KScreen"),
                                                QStringLiteral("/backend"),
                                                QDBusConnection::sessionBus());
    if (!mInterface->isValid()) {
        qCWarning(KSCREEN) << "Backend successfully requested, but we failed to obtain a valid DBus interface for it";
        invalidateInterface();
        emitBackendReady();
        return;
    }

    // The backend is GO, so let's watch for it's possible disappearance, so we
    // can invalidate the interface
    mServiceWatcher.addWatchedService(mBackendService);

    // Immediatelly request config
    connect(new GetConfigOperation(GetConfigOperation::NoEDID), &GetConfigOperation::finished,
            [&](ConfigOperation *op) {
                mConfig = qobject_cast<GetConfigOperation*>(op)->config();
            });
    // And listen for its change.
    connect(mInterface, &org::kde::kscreen::Backend::configChanged,
            [&](const QVariantMap &newConfig) {
                mConfig = KScreen::ConfigSerializer::deserializeConfig(newConfig);
            });

    emitBackendReady();
}

void BackendManager::backendServiceUnregistered(const QString &serviceName)
{
    mServiceWatcher.removeWatchedService(serviceName);

    invalidateInterface();
    requestBackend();
}

void BackendManager::invalidateInterface()
{
    delete mInterface;
    mInterface = 0;
    mBackendService.clear();
}

ConfigPtr BackendManager::config() const
{
    return mConfig;
}

void BackendManager::setConfig(ConfigPtr c)
{
    mConfig = c;
}

void BackendManager::setConfigInProcess(ConfigPtr c)
{
    Q_ASSERT(mInProcessBackend);
    mInProcessBackend->setConfig(c);
}

void BackendManager::shutdownBackend()
{
    if (mMode == InProcess) {
        mConfig.clear();
        mLoader->deleteLater();
        mLoader = nullptr;
        mInProcessBackend->deleteLater();
        mInProcessBackend = nullptr;
        for (auto k: m_inProcessBackends.keys()) {
        //foreach (auto pair, m_inProcessBackends.values()) {
            auto pair = m_inProcessBackends[k];
            qDebug() << "Deleting " << k;
            m_inProcessBackends.remove(k);
            delete pair.first;
        }
    } else {

        if (mBackendService.isEmpty() && !mInterface) {
            return;
        }

        // If there are some currently pending requests, then wait for them to
        // finish before quitting
        while (mRequestsCounter > 0) {
            mShutdownLoop.exec();
        }

        mServiceWatcher.removeWatchedService(mBackendService);
        mShuttingDown = true;

        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KScreen"),
                                                        QStringLiteral("/"),
                                                        QStringLiteral("org.kde.KScreen"),
                                                        QStringLiteral("quit"));
        // Call synchronously
        QDBusConnection::sessionBus().call(call);
        invalidateInterface();

        while (QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.KScreen"))) {
            QThread::msleep(100);
        }
    }
}

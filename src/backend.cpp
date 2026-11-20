#include "backend.h"
#include "portalfilepicker.h"
#include "worker.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>

#include <unistd.h>

namespace {

QString mkfsCommandFor(const QString &fs) {
    const QString f = fs.toLower();
    if (f == "fat32") return "mkfs.vfat";
    if (f == "ntfs") return "mkfs.ntfs";
    if (f == "exfat") return "mkfs.exfat";
    if (f == "ext4") return "mkfs.ext4";
    if (f == "ext3") return "mkfs.ext3";
    if (f == "ext2") return "mkfs.ext2";
    if (f == "udf") return "mkudffs";
    return {};
}

QString packageHintFor(const QString &cmd) {
    if (cmd == "mkfs.ntfs") return "ntfs-3g";
    if (cmd == "mkudffs") return "udftools";
    if (cmd == "mkfs.exfat") return "exfatprogs";
    if (cmd == "mkfs.vfat") return "dosfstools";
    return "e2fsprogs";
}

} // namespace

Backend::Backend(FilePicker *filePicker, bool enablePolling, QObject *parent)
    : QObject(parent),
      m_filePicker(filePicker ? filePicker : new PortalFilePicker(this)),
      m_statusText("READY") {
    connect(m_filePicker, &FilePicker::imagePicked, this, &Backend::selectImage);
    connect(m_filePicker, &FilePicker::savePathPicked, this, &Backend::onSavePathPicked);
    connect(m_filePicker, &FilePicker::failed, this, [this](const QString &msg) {
        m_errorText = msg;
        appendLog("ERROR: " + msg);
        emit stateChanged();
    });

    connect(&m_worker, &QProcess::readyReadStandardOutput, this, &Backend::onWorkerStdout);
    connect(&m_worker, &QProcess::readyReadStandardError, this, &Backend::onWorkerStderr);
    connect(&m_worker, &QProcess::finished, this, &Backend::onWorkerFinished);
    connect(&m_worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_busy)
            return;
        m_busy = false;
        m_elapsedTimerTick.stop();
        m_errorText = tr("Failed to launch disk worker: %1").arg(m_worker.errorString());
        m_statusText = tr("FAILED");
        appendLog("ERROR: " + m_errorText);
        emit stateChanged();
    });

    m_elapsedTimerTick.setInterval(500);
    connect(&m_elapsedTimerTick, &QTimer::timeout, this, &Backend::onElapsedTick);

    appendLog(QString("Rufus for Linux %1 (%2)")
                  .arg(QCoreApplication::applicationVersion(), QSysInfo::currentCpuArchitecture()));

    if (enablePolling) {
        refreshDevices();
        m_devicePollTimer.setInterval(2500);
        connect(&m_devicePollTimer, &QTimer::timeout, this, [this] {
            if (!m_busy)
                refreshDevices();
        });
        m_devicePollTimer.start();
    }
}

Backend::~Backend() {
    cancelChecksums();
    if (m_worker.state() != QProcess::NotRunning) {
        m_worker.terminate();
        if (!m_worker.waitForFinished(2000))
            m_worker.kill();
    }
}

QString Backend::formatBytes(qint64 bytes) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kKiB = 1024.0;
    if (bytes >= static_cast<qint64>(kGiB))
        return QString::number(bytes / kGiB, 'f', 2) + " GiB";
    if (bytes >= static_cast<qint64>(kMiB))
        return QString::number(bytes / kMiB, 'f', 1) + " MiB";
    if (bytes >= static_cast<qint64>(kKiB))
        return QString::number(bytes / kKiB, 'f', 0) + " KiB";
    return QString::number(bytes) + " B";
}

QString Backend::imageSizeText() const {
    return m_analysis.fileSize > 0 ? formatBytes(m_analysis.fileSize) : QString();
}

QVariantMap Backend::selectedDevice() const {
    if (m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < m_devices.size())
        return m_devices.at(m_selectedDeviceIndex).toMap();
    return {};
}

void Backend::setSelectedDeviceIndex(int idx) {
    if (idx < -1 || idx >= m_devices.size() || idx == m_selectedDeviceIndex)
        return;
    m_selectedDeviceIndex = idx;
    if (m_persistenceMB > maxPersistenceMB())
        m_persistenceMB = maxPersistenceMB();
    emit selectedDeviceChanged();
    emit optionsChanged();
    emit stateChanged();
}

void Backend::setListUsbHdd(bool v) {
    if (m_listUsbHdd == v)
        return;
    m_listUsbHdd = v;
    emit optionsChanged();
    refreshDevices();
}

void Backend::setBootSelection(const QString &v) {
    if (m_bootSelection == v)
        return;
    m_bootSelection = v;
    if (v == "freedos") {
        m_fileSystem = "fat32";
        m_partitionScheme = "mbr";
        m_uefiNtfs = false;
        m_writeMode = "iso";
        m_volumeLabel = "FREEDOS";
    } else if (v == "non_bootable") {
        m_uefiNtfs = false;
        m_writeMode = "iso";
    }
    emit optionsChanged();
    emit stateChanged();
}

void Backend::setWriteMode(const QString &v) {
    if (m_writeMode == v)
        return;
    m_writeMode = v;
    emit optionsChanged();
    emit imageChanged();
    emit stateChanged();
}

void Backend::setPartitionScheme(const QString &v) {
    if (m_partitionScheme == v)
        return;
    m_partitionScheme = v;
    emit optionsChanged();
}

QString Backend::targetSystemText() const {
    return m_partitionScheme == "gpt" ? tr("UEFI (non CSM)") : tr("BIOS or UEFI");
}

void Backend::setUefiNtfs(bool v) {
    if (m_uefiNtfs == v)
        return;
    m_uefiNtfs = v;
    emit optionsChanged();
}

void Backend::setPersistenceMB(int mb) {
    const int clamped = qBound(0, mb, maxPersistenceMB());
    if (m_persistenceMB == clamped)
        return;
    m_persistenceMB = clamped;
    emit optionsChanged();
}

int Backend::maxPersistenceMB() const {
    const qint64 devBytes = selectedDevice().value("size").toLongLong();
    if (devBytes <= 0)
        return 0;
    const qint64 imageBytes = qMax<qint64>(m_analysis.fileSize, 512LL * 1024 * 1024);
    const qint64 availMB = (devBytes - imageBytes - 256LL * 1024 * 1024) / (1024 * 1024);
    return static_cast<int>(qBound(qint64(0), availMB, qint64(32768)));
}

void Backend::setVolumeLabel(const QString &v) {
    if (m_volumeLabel == v)
        return;
    m_volumeLabel = v;
    emit optionsChanged();
}

void Backend::setFileSystem(const QString &v) {
    if (m_fileSystem == v)
        return;
    m_fileSystem = v;
    if (m_analysis.valid && m_analysis.hasUefiBoot && (v == "ntfs" || v == "exfat"))
        m_uefiNtfs = true;
    else if (v != "ntfs" && v != "exfat")
        m_uefiNtfs = false;
    emit optionsChanged();
    emit stateChanged();
}

void Backend::setClusterSize(int bytes) {
    if (m_clusterSize == bytes)
        return;
    m_clusterSize = bytes;
    emit optionsChanged();
}

void Backend::setQuickFormat(bool v) {
    if (m_quickFormat == v)
        return;
    m_quickFormat = v;
    emit optionsChanged();
}

void Backend::setCheckBadBlocks(bool v) {
    if (m_checkBadBlocks == v)
        return;
    m_checkBadBlocks = v;
    emit optionsChanged();
}

void Backend::setBadBlocksPasses(int passes) {
    const int clamped = qBound(1, passes, 4);
    if (m_badBlocksPasses == clamped)
        return;
    m_badBlocksPasses = clamped;
    emit optionsChanged();
}

QString Backend::missingToolWarning() const {
    if (m_bootSelection == "image" && m_writeMode == "dd")
        return {};
    const QString cmd = mkfsCommandFor(m_fileSystem);
    if (!cmd.isEmpty() && QStandardPaths::findExecutable(cmd).isEmpty()) {
        return tr("Formatting as %1 requires '%2' (install package '%3').")
            .arg(m_fileSystem.toUpper(), cmd, packageHintFor(cmd));
    }
    if (m_checkBadBlocks && QStandardPaths::findExecutable("badblocks").isEmpty())
        return tr("Bad blocks check requires 'badblocks' (install package 'e2fsprogs').");
    return {};
}

void Backend::setWueEnabled(bool v) {
    if (m_wue.enabled == v) return;
    m_wue.enabled = v;
    emit wueChanged();
}

void Backend::setWueBypassTpm(bool v) {
    if (m_wue.bypassTpmSecureBootRam == v) return;
    m_wue.bypassTpmSecureBootRam = v;
    emit wueChanged();
}

void Backend::setWueBypassMsa(bool v) {
    if (m_wue.bypassOnlineAccount == v) return;
    m_wue.bypassOnlineAccount = v;
    emit wueChanged();
}

void Backend::setWueCreateLocalAccount(bool v) {
    if (m_wue.createLocalAccount == v) return;
    m_wue.createLocalAccount = v;
    emit wueChanged();
}

void Backend::setWueLocalUsername(const QString &v) {
    if (m_wue.localUsername == v) return;
    m_wue.localUsername = v;
    emit wueChanged();
}

void Backend::setWueDisableDataCollection(bool v) {
    if (m_wue.disableDataCollection == v) return;
    m_wue.disableDataCollection = v;
    emit wueChanged();
}

void Backend::setWueDisableBitLocker(bool v) {
    if (m_wue.disableBitLocker == v) return;
    m_wue.disableBitLocker = v;
    emit wueChanged();
}

void Backend::setWueQolEnhancements(bool v) {
    if (m_wue.qolEnhancements == v) return;
    m_wue.qolEnhancements = v;
    emit wueChanged();
}

bool Backend::canStart() const {
    if (m_busy)
        return false;
    if (m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= m_devices.size())
        return false;
    if (selectedDevice().value("readOnly").toBool())
        return false;
    if (m_bootSelection == "image" && !m_analysis.valid)
        return false;
    if (!missingToolWarning().isEmpty())
        return false;
    return true;
}

QString Backend::confirmationSummary() const {
    const QVariantMap dev = selectedDevice();
    if (dev.isEmpty())
        return {};

    QString modeDesc;
    if (m_bootSelection == "image") {
        modeDesc = (m_writeMode == "dd")
                       ? tr("DD Image mode — %1").arg(m_analysis.fileName)
                       : tr("ISO Image mode (%1, %2) — %3")
                             .arg(m_partitionScheme.toUpper(), m_fileSystem.toUpper(), m_analysis.fileName);
    } else if (m_bootSelection == "freedos") {
        modeDesc = tr("FreeDOS (%1, FAT32)").arg(m_partitionScheme.toUpper());
    } else {
        modeDesc = tr("Non-bootable format (%1, %2)")
                       .arg(m_partitionScheme.toUpper(), m_fileSystem.toUpper());
    }

    return tr("Device: %1\nAction: %2\n\nALL DATA ON THIS DEVICE WILL BE PERMANENTLY DESTROYED.")
        .arg(dev.value("displayName").toString(), modeDesc);
}

void Backend::refreshDevices() {
    const QString prevPath = selectedDevice().value("path").toString();
    QString err;
    const QJsonArray arr = DiskWorker::listDevices(m_listUsbHdd, &err);
    QVariantList next;
    next.reserve(arr.size());
    for (const QJsonValue &v : arr)
        next.append(v.toObject().toVariantMap());

    if (next == m_devices)
        return;

    m_devices = next;
    int newIdx = m_devices.isEmpty() ? -1 : 0;
    if (!prevPath.isEmpty()) {
        for (int i = 0; i < m_devices.size(); ++i) {
            if (m_devices.at(i).toMap().value("path").toString() == prevPath) {
                newIdx = i;
                break;
            }
        }
    }
    m_selectedDeviceIndex = newIdx;
    emit devicesChanged();
    emit selectedDeviceChanged();
    emit stateChanged();
}

void Backend::setDevicesForTest(const QVariantList &devices) {
    m_devices = devices;
    m_selectedDeviceIndex = m_devices.isEmpty() ? -1 : 0;
    emit devicesChanged();
    emit selectedDeviceChanged();
    emit stateChanged();
}

void Backend::openImageDialog() {
    if (m_busy) return;
    m_filePicker->openImage(tr("Select Disk or ISO Image"));
}

void Backend::openSaveDriveDialog() {
    if (m_busy || selectedDevice().isEmpty()) return;
    const QString name = selectedDevice().value("name").toString() + "-backup.img";
    m_filePicker->saveFile(tr("Save Drive to Raw Image"), name, "save_drive");
}

void Backend::openSaveLogDialog() {
    m_filePicker->saveFile(tr("Save Rufus Log"), "rufus-linux.log", "save_log");
}

void Backend::selectImage(const QString &pathOrUrl) {
    QString localPath = pathOrUrl.trimmed();
    if (localPath.startsWith("file://"))
        localPath = QUrl(localPath).toLocalFile();
    if (localPath.isEmpty())
        return;

    cancelChecksums();
    m_md5.clear();
    m_sha1.clear();
    m_sha256.clear();
    m_sha512.clear();
    m_hashProgress = 0.0;
    emit hashChanged();

    m_errorText.clear();
    m_successText.clear();
    m_analysis = IsoAnalyzer::analyze(localPath);
    if (!m_analysis.valid) {
        m_errorText = m_analysis.error;
        appendLog("ERROR: " + m_analysis.error);
        emit imageChanged();
        emit stateChanged();
        return;
    }

    m_bootSelection = "image";
    m_writeMode = m_analysis.recommendedWriteMode;
    m_partitionScheme = m_analysis.recommendedScheme;
    m_fileSystem = m_analysis.recommendedFileSystem;
    m_uefiNtfs = m_analysis.recommendUefiNtfs;
    m_volumeLabel = IsoAnalyzer::sanitizeLabel(m_analysis.volumeLabel, m_fileSystem);
    m_persistenceMB = 0;

    m_wue.arch = m_analysis.windowsArch;
    m_wue.enabled = m_analysis.isWindows;

    appendLog(QString("Selected image: %1 (%2) — %3")
                  .arg(m_analysis.fileName, formatBytes(m_analysis.fileSize), m_analysis.summary));
    if (m_analysis.hasLargeFile)
        appendLog("Detected file >= 4 GiB inside ISO; defaulting to NTFS + UEFI:NTFS.");

    emit imageChanged();
    emit optionsChanged();
    emit wueChanged();
    emit stateChanged();
}

void Backend::clearImage() {
    cancelChecksums();
    m_analysis = IsoAnalysis{};
    m_md5.clear();
    m_sha1.clear();
    m_sha256.clear();
    m_sha512.clear();
    m_hashProgress = 0.0;
    emit hashChanged();
    emit imageChanged();
    emit stateChanged();
}

void Backend::computeChecksums() {
    if (!m_analysis.valid || m_hashing)
        return;

    const QString path = m_analysis.path;
    m_hashing = true;
    m_hashProgress = 0.0;
    m_md5.clear();
    m_sha1.clear();
    m_sha256.clear();
    m_sha512.clear();
    m_hashCancel = std::make_shared<std::atomic_bool>(false);
    auto cancelToken = m_hashCancel;
    emit hashChanged();

    m_hashThread = QThread::create([this, path, cancelToken]() {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(this, [this]() {
                m_hashing = false;
                emit hashChanged();
            }, Qt::QueuedConnection);
            return;
        }

        const qint64 total = file.size();
        QCryptographicHash hMd5(QCryptographicHash::Md5);
        QCryptographicHash hSha1(QCryptographicHash::Sha1);
        QCryptographicHash hSha256(QCryptographicHash::Sha256);
        QCryptographicHash hSha512(QCryptographicHash::Sha512);

        QByteArray buf(1024 * 1024, Qt::Uninitialized);
        qint64 readTotal = 0;
        int lastPct = -1;

        while (!cancelToken->load()) {
            const qint64 n = file.read(buf.data(), buf.size());
            if (n <= 0)
                break;
            const QByteArrayView view(buf.constData(), n);
            hMd5.addData(view);
            hSha1.addData(view);
            hSha256.addData(view);
            hSha512.addData(view);
            readTotal += n;
            const int pct = total > 0 ? static_cast<int>(100.0 * readTotal / total) : 100;
            if (pct != lastPct) {
                lastPct = pct;
                QMetaObject::invokeMethod(this, [this, pct]() {
                    m_hashProgress = pct;
                    emit hashChanged();
                }, Qt::QueuedConnection);
            }
        }

        const bool cancelled = cancelToken->load();
        const QString md5Hex = cancelled ? QString() : QString::fromLatin1(hMd5.result().toHex());
        const QString sha1Hex = cancelled ? QString() : QString::fromLatin1(hSha1.result().toHex());
        const QString sha256Hex = cancelled ? QString() : QString::fromLatin1(hSha256.result().toHex());
        const QString sha512Hex = cancelled ? QString() : QString::fromLatin1(hSha512.result().toHex());

        QMetaObject::invokeMethod(this, [this, cancelled, md5Hex, sha1Hex, sha256Hex, sha512Hex]() {
            m_hashing = false;
            if (!cancelled) {
                m_hashProgress = 100.0;
                m_md5 = md5Hex;
                m_sha1 = sha1Hex;
                m_sha256 = sha256Hex;
                m_sha512 = sha512Hex;
                appendLog("SHA-256: " + m_sha256);
            }
            emit hashChanged();
        }, Qt::QueuedConnection);
    });

    connect(m_hashThread, &QThread::finished, m_hashThread, &QObject::deleteLater);
    connect(m_hashThread, &QThread::finished, this, [this]() { m_hashThread = nullptr; });
    m_hashThread->start();
}

void Backend::cancelChecksums() {
    if (m_hashCancel)
        m_hashCancel->store(true);
    if (m_hashThread) {
        m_hashThread->wait(3000);
        m_hashThread = nullptr;
    }
    if (m_hashing) {
        m_hashing = false;
        emit hashChanged();
    }
}

QString Backend::verifyHash(const QString &expected) const {
    const QString cleaned = expected.trimmed().toLower();
    if (cleaned.isEmpty())
        return {};
    if (m_md5.isEmpty() && m_sha1.isEmpty() && m_sha256.isEmpty() && m_sha512.isEmpty())
        return {};
    if (cleaned == m_md5 || cleaned == m_sha1 || cleaned == m_sha256 || cleaned == m_sha512)
        return "match";
    return "mismatch";
}

QJsonObject Backend::buildJobPayload() const {
    QJsonObject job{
        {"action", "write"},
        {"device", selectedDevice().value("path").toString()},
        {"includeUsbHdd", m_listUsbHdd},
        {"bootSelection", m_bootSelection},
        {"imagePath", m_analysis.path},
        {"writeMode", m_writeMode},
        {"partitionScheme", m_partitionScheme},
        {"fileSystem", m_fileSystem},
        {"volumeLabel", m_volumeLabel},
        {"clusterSize", m_clusterSize},
        {"quickFormat", m_quickFormat},
        {"checkBadBlocks", m_checkBadBlocks},
        {"badBlocksPasses", m_badBlocksPasses},
        {"uefiNtfs", m_uefiNtfs},
        {"persistenceMB", supportsPersistence() ? m_persistenceMB : 0},
        {"persistenceType", m_analysis.persistenceType},
        {"unattend", m_wue.toJson()},
    };
    return job;
}

void Backend::startOperation() {
    if (!canStart())
        return;
    launchWorkerJob(buildJobPayload());
}

void Backend::launchWorkerJob(const QJsonObject &job) {
    m_errorText.clear();
    m_successText.clear();
    m_busy = true;
    m_cancelRequested = false;
    m_progress = 0.0;
    m_speedText.clear();
    m_statusText = tr("Starting...");
    m_workerStdoutBuf.clear();
    m_operationClock.start();
    m_elapsedText = "00:00:00";
    m_elapsedTimerTick.start();
    emit stateChanged();

    const QString selfBin = QCoreApplication::applicationFilePath();
    QString program;
    QStringList args;

    if (::geteuid() == 0) {
        program = selfBin;
        args << "--worker";
    } else {
        // Use non-interactive sudo when passwordless elevation is configured,
        // otherwise prompt cleanly via PolicyKit (pkexec).
        QProcess sudoCheck;
        sudoCheck.start("sudo", {"-n", "true"});
        const bool nopasswd = sudoCheck.waitForFinished(1500) && sudoCheck.exitCode() == 0;
        if (nopasswd) {
            program = "sudo";
            args << "-n" << selfBin << "--worker";
        } else {
            program = QStandardPaths::findExecutable("pkexec");
            if (program.isEmpty()) {
                m_busy = false;
                m_elapsedTimerTick.stop();
                m_errorText = tr("Neither pkexec nor passwordless sudo is available to access raw block devices.");
                m_statusText = tr("FAILED");
                emit stateChanged();
                return;
            }
            args << selfBin << "--worker";
        }
    }

    appendLog(QString("Launching privileged worker (%1)...").arg(program));
    m_worker.start(program, args);
    if (!m_worker.waitForStarted(10000)) {
        m_busy = false;
        m_elapsedTimerTick.stop();
        m_errorText = tr("Could not start worker: %1").arg(m_worker.errorString());
        m_statusText = tr("FAILED");
        appendLog("ERROR: " + m_errorText);
        emit stateChanged();
        return;
    }

    m_worker.write(QJsonDocument(job).toJson(QJsonDocument::Compact));
    m_worker.closeWriteChannel();
}

void Backend::cancelOperation() {
    if (!m_busy || m_worker.state() == QProcess::NotRunning)
        return;
    m_cancelRequested = true;
    m_statusText = tr("Cancelling...");
    appendLog("Cancellation requested by user.");
    m_worker.terminate();
    QTimer::singleShot(3000, this, [this]() {
        if (m_worker.state() != QProcess::NotRunning)
            m_worker.kill();
    });
    emit stateChanged();
}

void Backend::onWorkerStdout() {
    m_workerStdoutBuf += m_worker.readAllStandardOutput();
    while (true) {
        const int nl = m_workerStdoutBuf.indexOf('\n');
        if (nl < 0)
            break;
        const QByteArray line = m_workerStdoutBuf.left(nl).trimmed();
        m_workerStdoutBuf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            appendLog(QString::fromUtf8(line));
            continue;
        }
        const QJsonObject ev = doc.object();
        const QString type = ev.value("type").toString();
        if (type == "log") {
            appendLog(ev.value("message").toString());
        } else if (type == "progress") {
            m_statusText = ev.value("stage").toString();
            m_progress = ev.value("percent").toDouble(m_progress);
            if (ev.contains("speedMBps"))
                m_speedText = QString("%1 MB/s").arg(ev.value("speedMBps").toDouble(), 0, 'f', 1);
            emit stateChanged();
        } else if (type == "error") {
            m_errorText = ev.value("message").toString();
            appendLog("ERROR: " + m_errorText);
            emit stateChanged();
        } else if (type == "done") {
            m_successText = ev.value("message").toString();
            m_progress = 100.0;
            m_statusText = tr("READY");
            appendLog("DONE: " + m_successText);
            emit stateChanged();
        }
    }
}

void Backend::onWorkerStderr() {
    const QString text = QString::fromUtf8(m_worker.readAllStandardError()).trimmed();
    if (!text.isEmpty())
        appendLog(text);
}

void Backend::onWorkerFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    onWorkerStdout();
    m_busy = false;
    m_elapsedTimerTick.stop();
    updateElapsedText();

    if (m_cancelRequested) {
        m_statusText = tr("CANCELLED");
        m_errorText = tr("Operation cancelled.");
    } else if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        m_statusText = tr("FAILED");
        if (m_errorText.isEmpty()) {
            if (exitCode == 126 || exitCode == 127)
                m_errorText = tr("Authentication was dismissed or denied.");
            else
                m_errorText = tr("Operation failed (exit code %1). Check the log for details.").arg(exitCode);
        }
    } else {
        m_statusText = tr("READY");
        m_progress = 100.0;
    }

    emit stateChanged();
    refreshDevices();
}

void Backend::onElapsedTick() {
    updateElapsedText();
    emit stateChanged();
}

void Backend::updateElapsedText() {
    const qint64 secs = m_operationClock.elapsed() / 1000;
    const int h = static_cast<int>(secs / 3600);
    const int m = static_cast<int>((secs % 3600) / 60);
    const int s = static_cast<int>(secs % 60);
    m_elapsedText = QString("%1:%2:%3")
                        .arg(h, 2, 10, QChar('0'))
                        .arg(m, 2, 10, QChar('0'))
                        .arg(s, 2, 10, QChar('0'));
}

void Backend::onSavePathPicked(const QString &path, const QString &tag) {
    if (tag == "save_log") {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write((logText() + "\n").toUtf8());
            file.close();
            m_successText = tr("Saved log to %1").arg(path);
        } else {
            m_errorText = tr("Could not save log: %1").arg(file.errorString());
        }
        emit stateChanged();
        return;
    }

    if (tag == "save_drive" && !selectedDevice().isEmpty()) {
        QJsonObject job{
            {"action", "save_image"},
            {"device", selectedDevice().value("path").toString()},
            {"includeUsbHdd", m_listUsbHdd},
            {"outputPath", path},
        };
        launchWorkerJob(job);
    }
}

void Backend::clearBanner() {
    m_errorText.clear();
    m_successText.clear();
    emit stateChanged();
}

void Backend::copyToClipboard(const QString &text) {
    if (QClipboard *cb = QGuiApplication::clipboard())
        cb->setText(text);
}

void Backend::clearLog() {
    m_logs.clear();
    emit logChanged();
}

void Backend::appendLog(const QString &line) {
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logs.append(QString("[%1] %2").arg(ts, line));
    if (m_logs.size() > 1000)
        m_logs.removeFirst();
    emit logChanged();
}

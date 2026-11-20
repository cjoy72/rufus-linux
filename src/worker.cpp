#include "worker.h"
#include "isoanalyzer.h"
#include "unattend.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void emitEvent(const QJsonObject &obj) {
    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    fflush(stdout);
}

void emitLog(const QString &msg) {
    emitEvent({{"type", "log"}, {"message", msg}});
}

void emitProgress(const QString &stage, double percent, double speedMBps = -1.0,
                  qint64 bytesDone = -1, qint64 bytesTotal = -1) {
    QJsonObject obj{
        {"type", "progress"},
        {"stage", stage},
        {"percent", qBound(0.0, percent, 100.0)},
    };
    if (speedMBps >= 0.0) obj.insert("speedMBps", speedMBps);
    if (bytesDone >= 0) obj.insert("bytesDone", bytesDone);
    if (bytesTotal >= 0) obj.insert("bytesTotal", bytesTotal);
    emitEvent(obj);
}

void emitError(const QString &msg) {
    emitEvent({{"type", "error"}, {"message", msg}});
}

void emitDone(const QString &msg) {
    emitEvent({{"type", "done"}, {"message", msg}});
}

bool isCriticalMountPoint(const QString &mp) {
    const QString m = mp.trimmed();
    return m == "/" || m == "/boot" || m == "/boot/efi" || m == "/home" ||
           m == "/var" || m == "/usr" || m == "/etc" || m.startsWith("/var/log");
}

bool nodeHasCriticalMount(const QJsonObject &node) {
    const QJsonArray mounts = node.value("mountpoints").toArray();
    for (const QJsonValue &v : mounts) {
        if (v.isString() && isCriticalMountPoint(v.toString()))
            return true;
    }
    const QString singleMp = node.value("mountpoint").toString();
    if (!singleMp.isEmpty() && isCriticalMountPoint(singleMp))
        return true;

    const QJsonArray children = node.value("children").toArray();
    for (const QJsonValue &child : children) {
        if (child.isObject() && nodeHasCriticalMount(child.toObject()))
            return true;
    }
    return false;
}

void collectMounts(const QJsonObject &node, QStringList &mountPoints, QStringList &partPaths) {
    const QString path = node.value("path").toString();
    const QString type = node.value("type").toString();
    if ((type == "part" || type == "crypt") && !path.isEmpty())
        partPaths << path;

    for (const QJsonValue &v : node.value("mountpoints").toArray()) {
        const QString mp = v.toString();
        if (!mp.isEmpty() && mp != "[SWAP]")
            mountPoints << mp;
    }
    for (const QJsonValue &child : node.value("children").toArray()) {
        if (child.isObject())
            collectMounts(child.toObject(), mountPoints, partPaths);
    }
}

QString formatHumanSize(qint64 bytes) {
    constexpr double kGB = 1000.0 * 1000.0 * 1000.0;
    constexpr double kMB = 1000.0 * 1000.0;
    if (bytes >= static_cast<qint64>(kGB))
        return QString::number(bytes / kGB, 'f', 1) + " GB";
    if (bytes >= static_cast<qint64>(kMB))
        return QString::number(bytes / kMB, 'f', 0) + " MB";
    return QString::number(bytes) + " B";
}

bool runCommand(const QString &program, const QStringList &args, const QByteArray &stdinData,
                QString *stdoutOut, QString *stderrOut, int timeoutMs = 600000) {
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) {
        if (stderrOut)
            *stderrOut = QString("Failed to start %1: %2").arg(program, proc.errorString());
        return false;
    }
    if (!stdinData.isEmpty()) {
        proc.write(stdinData);
        proc.closeWriteChannel();
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(3000);
        if (stderrOut)
            *stderrOut = QString("%1 timed out").arg(program);
        return false;
    }
    if (stdoutOut)
        *stdoutOut = QString::fromUtf8(proc.readAllStandardOutput());
    if (stderrOut)
        *stderrOut = QString::fromUtf8(proc.readAllStandardError());
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

QString partitionDevicePath(const QString &diskPath, int partNum) {
    if (diskPath.back().isDigit())
        return QString("%1p%2").arg(diskPath).arg(partNum);
    return QString("%1%2").arg(diskPath).arg(partNum);
}

bool unmountTargetDevice(const QString &devicePath) {
    QString out, err;
    if (!runCommand("lsblk", {"-J", "-b", "-o", "NAME,PATH,TYPE,MOUNTPOINTS", devicePath}, {}, &out, &err, 15000))
        return true;

    const QJsonArray devs = QJsonDocument::fromJson(out.toUtf8()).object().value("blockdevices").toArray();
    QStringList mounts, parts;
    for (const QJsonValue &v : devs) {
        if (v.isObject())
            collectMounts(v.toObject(), mounts, parts);
    }

    // Unmount deepest mount points first.
    std::sort(mounts.begin(), mounts.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    for (const QString &mp : mounts) {
        emitLog(QString("Unmounting %1...").arg(mp));
        QString uOut, uErr;
        if (!runCommand("umount", {"-f", mp}, {}, &uOut, &uErr, 15000)) {
            if (!runCommand("umount", {"-l", mp}, {}, &uOut, &uErr, 15000)) {
                emitLog(QString("Warning: failed to unmount %1: %2").arg(mp, uErr.trimmed()));
                return false;
            }
        }
    }
    for (const QString &part : parts) {
        runCommand("swapoff", {part}, {}, nullptr, nullptr, 10000);
    }
    return true;
}

qint64 getDeviceSizeBytes(const QString &devicePath) {
    QString out, err;
    if (runCommand("blockdev", {"--getsize64", devicePath}, {}, &out, &err, 10000)) {
        bool ok = false;
        const qint64 sz = out.trimmed().toLongLong(&ok);
        if (ok && sz > 0)
            return sz;
    }
    QFileInfo fi(devicePath);
    return fi.size();
}

bool runBadBlocks(const QString &devicePath, int passes, QString *errorOut) {
    const QString badblocksBin = QStandardPaths::findExecutable("badblocks");
    if (badblocksBin.isEmpty()) {
        *errorOut = "badblocks utility not found (install e2fsprogs).";
        return false;
    }

    // Rufus's classic 1..4 test patterns: 0x55, 0xAA, 0xFF, 0x00.
    const QStringList allPatterns = {"0x55", "0xaa", "0xff", "0x00"};
    const int count = qBound(1, passes, 4);

    for (int i = 0; i < count; ++i) {
        emitLog(QString("Bad blocks check: pass %1 of %2 (pattern %3)...")
                    .arg(i + 1).arg(count).arg(allPatterns[i]));
        QProcess proc;
        proc.start(badblocksBin, {"-w", "-s", "-b", "4096", "-t", allPatterns[i], devicePath});
        if (!proc.waitForStarted(10000)) {
            *errorOut = "Failed to start badblocks: " + proc.errorString();
            return false;
        }

        const QRegularExpression pctRe(R"((\d+(?:\.\d+)?)%\s+done)");
        QString stderrBuf;
        while (proc.state() != QProcess::NotRunning) {
            if (proc.waitForReadyRead(250)) {
                const QString chunk = QString::fromUtf8(proc.readAllStandardError());
                stderrBuf += chunk;
                auto it = pctRe.globalMatch(chunk);
                double lastPct = -1.0;
                while (it.hasNext())
                    lastPct = it.next().captured(1).toDouble();
                if (lastPct >= 0.0) {
                    const double overall = ((i * 100.0) + lastPct) / count;
                    emitProgress(QString("Checking bad blocks (pass %1/%2)...").arg(i + 1).arg(count), overall);
                }
            }
        }
        const QString stdoutData = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (proc.exitCode() != 0) {
            *errorOut = QString("Bad blocks check failed: %1").arg(stderrBuf.right(300).trimmed());
            return false;
        }
        if (!stdoutData.isEmpty()) {
            const int badCount = stdoutData.split('\n', Qt::SkipEmptyParts).size();
            *errorOut = QString("Found %1 bad block(s) on %2.").arg(badCount).arg(devicePath);
            return false;
        }
    }
    emitLog("Bad blocks check passed with 0 errors.");
    return true;
}

bool writeRawOrCompressedImage(const QString &imagePath, const QString &targetPath,
                               bool isCompressed, QString *errorOut) {
    const qint64 imageSize = QFileInfo(imagePath).size();
    const int outFd = ::open(QFile::encodeName(targetPath).constData(), O_WRONLY | O_CLOEXEC);
    if (outFd < 0) {
        *errorOut = QString("Failed to open %1 for writing: %2").arg(targetPath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    constexpr size_t kBufSize = 4 * 1024 * 1024;
    QByteArray buffer(static_cast<qsizetype>(kBufSize), Qt::Uninitialized);
    QElapsedTimer timer;
    timer.start();
    qint64 totalWritten = 0;
    qint64 lastEmitMs = 0;

    auto writeAll = [&](const char *data, size_t len) -> bool {
        size_t offset = 0;
        while (offset < len) {
            const ssize_t n = ::write(outFd, data + offset, len - offset);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                *errorOut = QString("Write error on %1: %2").arg(targetPath, QString::fromLocal8Bit(strerror(errno)));
                return false;
            }
            offset += static_cast<size_t>(n);
        }
        totalWritten += static_cast<qint64>(len);
        return true;
    };

    bool ok = true;
    if (!isCompressed) {
        const int inFd = ::open(QFile::encodeName(imagePath).constData(), O_RDONLY | O_CLOEXEC);
        if (inFd < 0) {
            ::close(outFd);
            *errorOut = QString("Failed to open image %1: %2").arg(imagePath, QString::fromLocal8Bit(strerror(errno)));
            return false;
        }

        emitLog(QString("Writing image directly (DD mode): %1 -> %2").arg(imagePath, targetPath));
        while (true) {
            const ssize_t n = ::read(inFd, buffer.data(), kBufSize);
            if (n < 0) {
                if (errno == EINTR) continue;
                *errorOut = QString("Read error on %1: %2").arg(imagePath, QString::fromLocal8Bit(strerror(errno)));
                ok = false;
                break;
            }
            if (n == 0)
                break;
            if (!writeAll(buffer.constData(), static_cast<size_t>(n))) {
                ok = false;
                break;
            }
            const qint64 now = timer.elapsed();
            if (now - lastEmitMs >= 200 || totalWritten == imageSize) {
                const double secs = qMax(0.001, now / 1000.0);
                const double speed = (totalWritten / (1024.0 * 1024.0)) / secs;
                const double pct = imageSize > 0 ? (100.0 * totalWritten / imageSize) : 0.0;
                emitProgress("Writing image (DD mode)...", pct, speed, totalWritten, imageSize);
                lastEmitMs = now;
            }
        }
        ::close(inFd);
    } else {
        emitLog(QString("Streaming compressed image (DD mode): %1 -> %2").arg(imagePath, targetPath));
        struct archive *a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_raw(a);
        archive_read_support_format_zip(a);
        if (archive_read_open_filename(a, QFile::encodeName(imagePath).constData(), 65536) != ARCHIVE_OK) {
            *errorOut = QString("Failed to open compressed image: %1").arg(QString::fromUtf8(archive_error_string(a)));
            archive_read_free(a);
            ::close(outFd);
            return false;
        }
        struct archive_entry *entry = nullptr;
        if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
            *errorOut = QString("Failed to read compressed image header: %1").arg(QString::fromUtf8(archive_error_string(a)));
            archive_read_free(a);
            ::close(outFd);
            return false;
        }
        const qint64 expectedUncompressed = archive_entry_size_is_set(entry)
                                                ? static_cast<qint64>(archive_entry_size(entry))
                                                : 0;
        while (true) {
            const la_ssize_t n = archive_read_data(a, buffer.data(), kBufSize);
            if (n < 0) {
                *errorOut = QString("Decompression error: %1").arg(QString::fromUtf8(archive_error_string(a)));
                ok = false;
                break;
            }
            if (n == 0)
                break;
            if (!writeAll(buffer.constData(), static_cast<size_t>(n))) {
                ok = false;
                break;
            }
            const qint64 now = timer.elapsed();
            if (now - lastEmitMs >= 200) {
                const double secs = qMax(0.001, now / 1000.0);
                const double speed = (totalWritten / (1024.0 * 1024.0)) / secs;
                const qint64 compressedPos = archive_filter_bytes(a, -1);
                double pct = 0.0;
                if (expectedUncompressed > 0)
                    pct = 100.0 * totalWritten / expectedUncompressed;
                else if (imageSize > 0)
                    pct = 100.0 * compressedPos / imageSize;
                emitProgress("Decompressing & writing image...", qMin(99.0, pct), speed, totalWritten,
                             expectedUncompressed > 0 ? expectedUncompressed : imageSize);
                lastEmitMs = now;
            }
        }
        archive_read_free(a);
    }

    if (ok) {
        emitProgress("Flushing buffers to device...", 99.0);
        emitLog("Syncing data to disk (fdatasync)...");
        if (::fdatasync(outFd) != 0 && errno != EINVAL) {
            *errorOut = QString("Sync error: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            ok = false;
        }
    }
    ::close(outFd);
    return ok;
}

bool saveDriveToImage(const QString &devicePath, const QString &outputPath, QString *errorOut) {
    const qint64 totalBytes = getDeviceSizeBytes(devicePath);
    if (totalBytes <= 0) {
        *errorOut = QString("Cannot determine size of %1.").arg(devicePath);
        return false;
    }

    const int inFd = ::open(QFile::encodeName(devicePath).constData(), O_RDONLY | O_CLOEXEC);
    if (inFd < 0) {
        *errorOut = QString("Cannot open %1 for reading: %2").arg(devicePath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    const int outFd = ::open(QFile::encodeName(outputPath).constData(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (outFd < 0) {
        ::close(inFd);
        *errorOut = QString("Cannot create %1: %2").arg(outputPath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    emitLog(QString("Saving drive %1 (%2) -> %3").arg(devicePath, formatHumanSize(totalBytes), outputPath));

    constexpr size_t kBufSize = 4 * 1024 * 1024;
    QByteArray buffer(static_cast<qsizetype>(kBufSize), Qt::Uninitialized);
    QElapsedTimer timer;
    timer.start();
    qint64 totalRead = 0;
    qint64 lastEmitMs = 0;
    bool ok = true;

    while (totalRead < totalBytes) {
        const ssize_t n = ::read(inFd, buffer.data(), kBufSize);
        if (n < 0) {
            if (errno == EINTR) continue;
            *errorOut = QString("Read error on %1: %2").arg(devicePath, QString::fromLocal8Bit(strerror(errno)));
            ok = false;
            break;
        }
        if (n == 0)
            break;

        size_t off = 0;
        while (off < static_cast<size_t>(n)) {
            const ssize_t w = ::write(outFd, buffer.constData() + off, static_cast<size_t>(n) - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                *errorOut = QString("Write error on %1: %2").arg(outputPath, QString::fromLocal8Bit(strerror(errno)));
                ok = false;
                break;
            }
            off += static_cast<size_t>(w);
        }
        if (!ok) break;

        totalRead += n;
        const qint64 now = timer.elapsed();
        if (now - lastEmitMs >= 200 || totalRead >= totalBytes) {
            const double secs = qMax(0.001, now / 1000.0);
            const double speed = (totalRead / (1024.0 * 1024.0)) / secs;
            const double pct = 100.0 * totalRead / totalBytes;
            emitProgress("Saving drive image...", pct, speed, totalRead, totalBytes);
            lastEmitMs = now;
        }
    }

    ::fdatasync(outFd);
    ::close(outFd);
    ::close(inFd);
    return ok;
}

bool formatPartition(const QString &partPath, const QString &fileSystem, const QString &label,
                     int clusterSize, bool quickFormat, QString *errorOut) {
    const QString fs = fileSystem.toLower();
    const QString cleanLabel = IsoAnalyzer::sanitizeLabel(label, fs);
    QString cmd;
    QStringList args;

    if (fs == "fat32") {
        cmd = "mkfs.vfat";
        args << "-F" << "32" << "-n" << cleanLabel;
        if (clusterSize >= 512 && (clusterSize % 512) == 0) {
            const int spc = qBound(1, clusterSize / 512, 128);
            args << "-s" << QString::number(spc);
        }
        args << partPath;
    } else if (fs == "ntfs") {
        cmd = "mkfs.ntfs";
        if (quickFormat) args << "-Q";
        args << "-L" << cleanLabel;
        if (clusterSize >= 512)
            args << "-c" << QString::number(clusterSize);
        args << partPath;
    } else if (fs == "exfat") {
        cmd = "mkfs.exfat";
        args << "-L" << cleanLabel;
        if (clusterSize >= 4096)
            args << "-c" << QString::number(clusterSize);
        args << partPath;
    } else if (fs == "ext4" || fs == "ext3" || fs == "ext2") {
        cmd = "mkfs." + fs;
        args << "-F" << "-L" << cleanLabel;
        if (clusterSize == 1024 || clusterSize == 2048 || clusterSize == 4096)
            args << "-b" << QString::number(clusterSize);
        args << partPath;
    } else if (fs == "udf") {
        cmd = "mkudffs";
        args << "--label" << cleanLabel << partPath;
    } else {
        *errorOut = QString("Unsupported filesystem: %1").arg(fileSystem);
        return false;
    }

    const QString bin = QStandardPaths::findExecutable(cmd);
    if (bin.isEmpty()) {
        *errorOut = QString("Formatting tool '%1' is not installed on this machine.").arg(cmd);
        return false;
    }

    emitLog(QString("Formatting %1 as %2 (label '%3'): %4 %5")
                .arg(partPath, fs.toUpper(), cleanLabel, cmd, args.join(' ')));
    QString out, err;
    if (!runCommand(bin, args, {}, &out, &err, 600000)) {
        *errorOut = QString("%1 failed: %2").arg(cmd, err.isEmpty() ? out : err).trimmed();
        return false;
    }
    return true;
}

bool extractIsoToMount(const QString &isoPath, const QString &mountDir, qint64 expectedBytes,
                       QString *errorOut) {
    struct archive *a = archive_read_new();
    archive_read_support_format_iso9660(a);
    archive_read_support_format_all(a);
    archive_read_support_filter_none(a);

    if (archive_read_open_filename(a, QFile::encodeName(isoPath).constData(), 131072) != ARCHIVE_OK) {
        *errorOut = QString("Failed to open ISO for extraction: %1").arg(QString::fromUtf8(archive_error_string(a)));
        archive_read_free(a);
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    qint64 bytesWritten = 0;
    qint64 lastEmitMs = 0;
    int filesExtracted = 0;
    QByteArray copyBuf(1024 * 1024, Qt::Uninitialized);
    struct archive_entry *entry = nullptr;
    bool ok = true;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *rawName = archive_entry_pathname(entry);
        if (!rawName) {
            archive_read_data_skip(a);
            continue;
        }
        QString relPath = QString::fromUtf8(rawName);
        while (relPath.startsWith("./"))
            relPath.remove(0, 2);
        while (relPath.startsWith('/'))
            relPath.remove(0, 1);
        if (relPath.isEmpty() || relPath == "." || relPath.contains("..")) {
            archive_read_data_skip(a);
            continue;
        }

        const QString fullPath = mountDir + "/" + relPath;
        const mode_t ftype = archive_entry_filetype(entry);

        if (ftype == AE_IFDIR) {
            QDir().mkpath(fullPath);
            archive_read_data_skip(a);
            continue;
        }
        if (ftype == AE_IFLNK) {
            // FAT32/exFAT don't support POSIX symlinks; try silently and continue.
            QDir().mkpath(QFileInfo(fullPath).absolutePath());
            const char *target = archive_entry_symlink(entry);
            if (target)
                (void)::symlink(target, QFile::encodeName(fullPath).constData());
            archive_read_data_skip(a);
            continue;
        }
        if (ftype != AE_IFREG) {
            archive_read_data_skip(a);
            continue;
        }

        QDir().mkpath(QFileInfo(fullPath).absolutePath());
        QFile outFile(fullPath);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *errorOut = QString("Failed to create %1: %2").arg(relPath, outFile.errorString());
            ok = false;
            break;
        }

        while (true) {
            const la_ssize_t n = archive_read_data(a, copyBuf.data(), static_cast<size_t>(copyBuf.size()));
            if (n < 0) {
                *errorOut = QString("ISO read error on %1: %2").arg(relPath, QString::fromUtf8(archive_error_string(a)));
                ok = false;
                break;
            }
            if (n == 0)
                break;
            if (outFile.write(copyBuf.constData(), n) != n) {
                *errorOut = QString("Write error on %1: %2").arg(relPath, outFile.errorString());
                ok = false;
                break;
            }
            bytesWritten += n;
            const qint64 now = timer.elapsed();
            if (now - lastEmitMs >= 200) {
                const double secs = qMax(0.001, now / 1000.0);
                const double speed = (bytesWritten / (1024.0 * 1024.0)) / secs;
                const double pct = expectedBytes > 0 ? 25.0 + (70.0 * bytesWritten / expectedBytes) : 60.0;
                emitProgress(QString("Copying ISO files (%1)...").arg(QFileInfo(relPath).fileName()),
                             qMin(95.0, pct), speed, bytesWritten, expectedBytes);
                lastEmitMs = now;
            }
        }
        outFile.close();
        if (!ok)
            break;
        filesExtracted++;
    }

    archive_read_free(a);
    if (ok)
        emitLog(QString("Extracted %1 files (%2).").arg(filesExtracted).arg(formatHumanSize(bytesWritten)));
    return ok;
}

bool copyFreeDosAssets(const QString &mountDir, QString *errorOut) {
    const QDir assetDir(":/assets/freedos");
    const QStringList entries = assetDir.entryList(QDir::Files);
    if (entries.isEmpty()) {
        *errorOut = "Bundled FreeDOS assets are missing from binary resources.";
        return false;
    }
    int copied = 0;
    for (const QString &name : entries) {
        QFile src(":/assets/freedos/" + name);
        if (!src.open(QIODevice::ReadOnly))
            continue;
        const QString destPath = (name == "KERNEL.SYS" || name == "COMMAND.COM")
                                     ? (mountDir + "/" + name)
                                     : (mountDir + "/LOCALE/" + name);
        QDir().mkpath(QFileInfo(destPath).absolutePath());
        QFile dst(destPath);
        if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *errorOut = QString("Failed to write FreeDOS file %1: %2").arg(name, dst.errorString());
            return false;
        }
        dst.write(src.readAll());
        dst.close();
        copied++;
    }
    emitLog(QString("Copied %1 FreeDOS system files from Rufus assets.").arg(copied));
    return true;
}

bool writeUefiNtfsPartition(const QString &partPath, QString *errorOut) {
    QFile img(":/assets/uefi/uefi-ntfs.img");
    if (!img.open(QIODevice::ReadOnly)) {
        *errorOut = "Bundled uefi-ntfs.img resource is missing.";
        return false;
    }
    const QByteArray data = img.readAll();
    const int fd = ::open(QFile::encodeName(partPath).constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        *errorOut = QString("Cannot open UEFI:NTFS partition %1: %2")
                        .arg(partPath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    const ssize_t written = ::write(fd, data.constData(), static_cast<size_t>(data.size()));
    ::fdatasync(fd);
    ::close(fd);
    if (written != data.size()) {
        *errorOut = QString("Failed to write uefi-ntfs.img to %1.").arg(partPath);
        return false;
    }
    emitLog(QString("Wrote Rufus UEFI:NTFS bootloader (%1 KiB) to %2.").arg(data.size() / 1024).arg(partPath));
    return true;
}

} // namespace

QJsonArray DiskWorker::filterDevices(const QJsonObject &lsblkJson, bool includeUsbHdd) {
    QJsonArray out;
    const QJsonArray devices = lsblkJson.value("blockdevices").toArray();
    for (const QJsonValue &val : devices) {
        if (!val.isObject())
            continue;
        const QJsonObject dev = val.toObject();
        const QString type = dev.value("type").toString();
        const QString path = dev.value("path").toString();
        const QString tran = dev.value("tran").toString().toLower();
        const bool rm = dev.value("rm").toBool(false);
        const bool ro = dev.value("ro").toBool(false);
        const bool hotplug = dev.value("hotplug").toBool(false);
        const qint64 size = dev.value("size").toVariant().toLongLong();

        if (type != "disk" || path.isEmpty() || size <= 0)
            continue;
        if (path.startsWith("/dev/zram") || path.startsWith("/dev/loop"))
            continue;
        if (nodeHasCriticalMount(dev))
            continue;

        const bool isUsb = (tran == "usb");
        const bool isRemovable = rm || hotplug;
        if (!isRemovable && !(includeUsbHdd && isUsb))
            continue;

        const QString vendor = dev.value("vendor").toString().trimmed();
        const QString model = dev.value("model").toString().trimmed();
        QString label;
        if (!vendor.isEmpty() && !model.isEmpty())
            label = vendor + " " + model;
        else if (!model.isEmpty())
            label = model;
        else
            label = dev.value("name").toString();

        const QString displayName = QString("%1 [%2] (%3)").arg(label, path, formatHumanSize(size));

        out.append(QJsonObject{
            {"name", dev.value("name").toString()},
            {"path", path},
            {"model", label},
            {"size", size},
            {"sizeText", formatHumanSize(size)},
            {"transport", tran.isEmpty() ? QString("unknown") : tran},
            {"removable", isRemovable},
            {"readOnly", ro},
            {"displayName", displayName},
        });
    }
    return out;
}

QJsonArray DiskWorker::listDevices(bool includeUsbHdd, QString *errorOut) {
    QString out, err;
    if (!runCommand("lsblk", {"-J", "-b", "-o", "NAME,PATH,SIZE,MODEL,VENDOR,TRAN,RM,RO,TYPE,MOUNTPOINTS,HOTPLUG"},
                    {}, &out, &err, 10000)) {
        if (errorOut)
            *errorOut = err.isEmpty() ? "Failed to run lsblk." : err.trimmed();
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    if (!doc.isObject()) {
        if (errorOut)
            *errorOut = "Invalid JSON output from lsblk.";
        return {};
    }
    return filterDevices(doc.object(), includeUsbHdd);
}

bool DiskWorker::validateTargetDevice(const QString &devicePath, bool includeUsbHdd,
                                      bool allowRegularFile, QString *reasonOut) {
    if (devicePath.isEmpty()) {
        if (reasonOut) *reasonOut = "No target device specified.";
        return false;
    }

    if (allowRegularFile) {
        QFileInfo fi(devicePath);
        if (fi.exists() && fi.isFile())
            return true;
    }

    if (!devicePath.startsWith("/dev/")) {
        if (reasonOut) *reasonOut = QString("Refusing non-device path: %1").arg(devicePath);
        return false;
    }

    QString err;
    const QJsonArray safeDevices = listDevices(includeUsbHdd, &err);
    for (const QJsonValue &v : safeDevices) {
        const QJsonObject obj = v.toObject();
        if (obj.value("path").toString() == devicePath) {
            if (obj.value("readOnly").toBool(false)) {
                if (reasonOut) *reasonOut = QString("Device %1 is read-only.").arg(devicePath);
                return false;
            }
            return true;
        }
    }

    if (reasonOut)
        *reasonOut = QString("Refusing target %1: not a safe removable/USB disk (system disks are protected).").arg(devicePath);
    return false;
}

int DiskWorker::runJob(const QJsonObject &job, bool allowRegularFile) {
    const QString action = job.value("action").toString("write");
    const QString devicePath = job.value("device").toString();
    const bool includeUsbHdd = job.value("includeUsbHdd").toBool(false);

    QString reason;
    if (!validateTargetDevice(devicePath, includeUsbHdd, allowRegularFile, &reason)) {
        emitError(reason);
        return 1;
    }

    const bool isRegularFile = allowRegularFile && QFileInfo(devicePath).isFile();

    // Action: Save drive to raw .img file.
    if (action == "save_image") {
        const QString outputPath = job.value("outputPath").toString();
        if (outputPath.isEmpty()) {
            emitError("No output file specified for drive backup.");
            return 1;
        }
        QString err;
        if (!saveDriveToImage(devicePath, outputPath, &err)) {
            emitError(err);
            return 1;
        }
        emitProgress("Completed", 100.0);
        emitDone(QString("Saved drive %1 to %2.").arg(devicePath, outputPath));
        return 0;
    }

    const QString bootSelection = job.value("bootSelection").toString("image"); // "image", "non_bootable", "freedos"
    const QString imagePath = job.value("imagePath").toString();
    const QString writeMode = job.value("writeMode").toString("iso"); // "iso" or "dd"
    const QString scheme = job.value("partitionScheme").toString("gpt").toLower(); // "gpt" or "mbr"
    const QString fileSystem = job.value("fileSystem").toString("fat32").toLower();
    const QString label = job.value("volumeLabel").toString("RUFUS_USB");
    const int clusterSize = job.value("clusterSize").toInt(0);
    const bool quickFormat = job.value("quickFormat").toBool(true);
    const bool checkBadBlocks = job.value("checkBadBlocks").toBool(false);
    const int badBlocksPasses = job.value("badBlocksPasses").toInt(1);
    const bool uefiNtfs = job.value("uefiNtfs").toBool(false);
    const int persistenceMB = job.value("persistenceMB").toInt(0);
    const QString persistenceType = job.value("persistenceType").toString("casper");
    const WindowsUnattendOptions unattend = WindowsUnattendOptions::fromJson(job.value("unattend").toObject());

    if (!isRegularFile) {
        emitProgress("Unmounting partitions...", 2.0);
        if (!unmountTargetDevice(devicePath)) {
            emitError(QString("Could not unmount partitions on %1. Close any open files on the drive and retry.").arg(devicePath));
            return 1;
        }
    }

    if (checkBadBlocks) {
        QString err;
        if (!runBadBlocks(devicePath, badBlocksPasses, &err)) {
            emitError(err);
            return 1;
        }
    }

    // DD Image mode: direct raw or decompressed stream write to device.
    if (bootSelection == "image" && writeMode == "dd") {
        const IsoAnalysis info = IsoAnalyzer::analyze(imagePath);
        if (!info.valid) {
            emitError(info.error.isEmpty() ? "Invalid image file." : info.error);
            return 1;
        }
        QString err;
        if (!writeRawOrCompressedImage(imagePath, devicePath, info.isCompressed, &err)) {
            emitError(err);
            return 1;
        }
        if (!isRegularFile) {
            runCommand("partprobe", {devicePath}, {}, nullptr, nullptr, 15000);
            runCommand("udevadm", {"settle"}, {}, nullptr, nullptr, 15000);
        }
        emitProgress("Ready", 100.0);
        emitDone(QString("Successfully wrote %1 to %2 in DD mode.").arg(info.fileName, devicePath));
        return 0;
    }

    // Partition + Format (+ optional UEFI:NTFS, Persistence, ISO extract / FreeDOS / autounattend.xml).
    const qint64 diskBytes = getDeviceSizeBytes(devicePath);
    if (diskBytes < 16LL * 1024 * 1024) {
        emitError(QString("Target %1 is too small (%2 bytes).").arg(devicePath).arg(diskBytes));
        return 1;
    }

    const qint64 diskMB = diskBytes / (1024 * 1024);
    const bool needUefiNtfs = (bootSelection == "image") && uefiNtfs && (fileSystem == "ntfs" || fileSystem == "exfat");
    const bool needPersistence = (bootSelection == "image") && persistenceMB > 0;
    const qint64 reserveTailMB = (needUefiNtfs ? 2 : 0) + (needPersistence ? persistenceMB : 0) + 2;
    if (diskMB <= reserveTailMB + 8) {
        emitError("Not enough space on target device for selected partitions.");
        return 1;
    }

    emitProgress("Wiping existing partition signatures...", 6.0);
    emitLog(QString("Wiping filesystem/partition signatures on %1...").arg(devicePath));
    QString out, err;
    runCommand("wipefs", {"--all", "--force", devicePath}, {}, &out, &err, 30000);

    // Build sfdisk partition script.
    emitProgress("Creating partition table...", 12.0);
    QString sfdiskScript;
    sfdiskScript += (scheme == "gpt") ? "label: gpt\n" : "label: dos\n";

    QString mainType;
    if (scheme == "gpt") {
        mainType = (fileSystem == "ext2" || fileSystem == "ext3" || fileSystem == "ext4")
                       ? "0FC63DAF-8483-4772-8E79-3D69D8477DE4" // Linux filesystem
                       : "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"; // Microsoft basic data
    } else {
        if (fileSystem == "fat32") mainType = "c";
        else if (fileSystem == "ntfs" || fileSystem == "exfat" || fileSystem == "udf") mainType = "7";
        else mainType = "83";
    }

    const qint64 mainSizeMB = diskMB - reserveTailMB;
    if (needPersistence || needUefiNtfs) {
        sfdiskScript += QString("start=1MiB, size=%1MiB, type=%2%3\n")
                            .arg(mainSizeMB)
                            .arg(mainType)
                            .arg(scheme == "mbr" ? ", bootable" : "");
    } else {
        sfdiskScript += QString("start=1MiB, type=%1%2\n")
                            .arg(mainType)
                            .arg(scheme == "mbr" ? ", bootable" : "");
    }

    int nextPartNum = 2;
    int persistPartNum = -1;
    int uefiPartNum = -1;

    if (needPersistence) {
        persistPartNum = nextPartNum++;
        const QString linuxType = (scheme == "gpt") ? "0FC63DAF-8483-4772-8E79-3D69D8477DE4" : "83";
        sfdiskScript += QString("size=%1MiB, type=%2\n").arg(persistenceMB).arg(linuxType);
    }

    if (needUefiNtfs) {
        uefiPartNum = nextPartNum++;
        // Rufus's UEFI:NTFS partition is a 1 MiB FAT partition at the end of the disk.
        const QString uefiType = (scheme == "gpt") ? "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" : "ef";
        sfdiskScript += QString("size=1MiB, type=%1\n").arg(uefiType);
    }

    emitLog(QString("Applying %1 partition table via sfdisk:\n%2").arg(scheme.toUpper(), sfdiskScript.trimmed()));
    if (!runCommand("sfdisk", {"--wipe", "always", "--wipe-partitions", "always", devicePath},
                    sfdiskScript.toUtf8(), &out, &err, 30000)) {
        emitError(QString("sfdisk failed: %1").arg(err.isEmpty() ? out : err).trimmed());
        return 1;
    }

    if (isRegularFile) {
        // In test mode against a regular image file, partition creation succeeded;
        // kernel partition device nodes don't exist without losetup, so finish here cleanly.
        emitProgress("Completed (image file mode)", 100.0);
        emitDone(QString("Partitioned test image %1 (%2).").arg(devicePath, scheme.toUpper()));
        return 0;
    }

    runCommand("partprobe", {devicePath}, {}, nullptr, nullptr, 15000);
    runCommand("udevadm", {"settle"}, {}, nullptr, nullptr, 15000);

    const QString part1 = partitionDevicePath(devicePath, 1);
    for (int tries = 0; tries < 20 && !QFileInfo::exists(part1); ++tries)
        usleep(150000);
    if (!QFileInfo::exists(part1)) {
        emitError(QString("Partition device %1 did not appear after partitioning.").arg(part1));
        return 1;
    }

    emitProgress("Formatting main partition...", 20.0);
    if (!formatPartition(part1, fileSystem, label, clusterSize, quickFormat, &err)) {
        emitError(err);
        return 1;
    }

    if (uefiPartNum > 0) {
        const QString uefiPart = partitionDevicePath(devicePath, uefiPartNum);
        emitProgress("Installing Rufus UEFI:NTFS bootloader...", 23.0);
        if (!writeUefiNtfsPartition(uefiPart, &err)) {
            emitError(err);
            return 1;
        }
    }

    if (persistPartNum > 0) {
        const QString persistPart = partitionDevicePath(devicePath, persistPartNum);
        const QString persistLabel = (persistenceType == "debian") ? "persistence" : "casper-rw";
        emitProgress("Creating persistence partition...", 24.0);
        if (!formatPartition(persistPart, "ext4", persistLabel, 4096, true, &err)) {
            emitError(err);
            return 1;
        }
        if (persistenceType == "debian") {
            QTemporaryDir pMount;
            if (pMount.isValid() && runCommand("mount", {persistPart, pMount.path()}, {}, nullptr, nullptr, 15000)) {
                QFile conf(pMount.filePath("persistence.conf"));
                if (conf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    conf.write("/ union\n");
                    conf.close();
                }
                runCommand("umount", {pMount.path()}, {}, nullptr, nullptr, 15000);
                emitLog("Wrote / union to persistence.conf on persistence partition.");
            }
        }
    }

    // Mount and populate if ISO or FreeDOS boot selection.
    if (bootSelection == "image" || bootSelection == "freedos") {
        QTemporaryDir mountDir;
        if (!mountDir.isValid()) {
            emitError("Failed to create temporary mount directory.");
            return 1;
        }

        emitProgress("Mounting target filesystem...", 25.0);
        if (!runCommand("mount", {part1, mountDir.path()}, {}, &out, &err, 30000)) {
            emitError(QString("Failed to mount %1: %2").arg(part1, err.trimmed()));
            return 1;
        }

        bool populateOk = true;
        if (bootSelection == "image") {
            const IsoAnalysis info = IsoAnalyzer::analyze(imagePath);
            if (!info.valid) {
                err = info.error;
                populateOk = false;
            } else {
                populateOk = extractIsoToMount(imagePath, mountDir.path(), info.totalExtractedBytes, &err);
                if (populateOk && info.isWindows && unattend.hasAnyOption()) {
                    const QString xml = UnattendGenerator::generate(unattend);
                    if (!xml.isEmpty()) {
                        QFile uFile(mountDir.filePath("autounattend.xml"));
                        if (uFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                            uFile.write(xml.toUtf8());
                            uFile.close();
                            emitLog("Wrote Windows User Experience autounattend.xml to root of drive.");
                        } else {
                            emitLog("Warning: failed to write autounattend.xml: " + uFile.errorString());
                        }
                    }
                }
            }
        } else if (bootSelection == "freedos") {
            emitProgress("Copying FreeDOS system files...", 60.0);
            populateOk = copyFreeDosAssets(mountDir.path(), &err);
        }

        emitProgress("Finalizing and syncing drive...", 96.0);
        runCommand("sync", {}, {}, nullptr, nullptr, 120000);
        QString uOut, uErr;
        if (!runCommand("umount", {mountDir.path()}, {}, &uOut, &uErr, 60000)) {
            runCommand("umount", {"-l", mountDir.path()}, {}, nullptr, nullptr, 15000);
        }

        if (!populateOk) {
            emitError(err);
            return 1;
        }
    }

    runCommand("sync", {}, {}, nullptr, nullptr, 60000);
    emitProgress("Ready", 100.0);
    emitDone(QString("Operation completed on %1.").arg(devicePath));
    return 0;
}

int DiskWorker::runFromStdin(bool allowRegularFile) {
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) {
        emitError("Worker failed to open stdin.");
        return 1;
    }
    const QByteArray payload = in.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        emitError("Worker received invalid JSON job specification.");
        return 1;
    }
    return runJob(doc.object(), allowRegularFile);
}

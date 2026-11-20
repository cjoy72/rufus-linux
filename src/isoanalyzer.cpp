#include "isoanalyzer.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <archive.h>
#include <archive_entry.h>

namespace {

constexpr qint64 kFourGiB = 4LL * 1024LL * 1024LL * 1024LL;

QString detectCompressionByMagic(const QByteArray &header, const QString &fileNameLower) {
    if (header.size() >= 2 && static_cast<unsigned char>(header[0]) == 0x1F &&
        static_cast<unsigned char>(header[1]) == 0x8B)
        return "gzip";
    if (header.size() >= 6 &&
        static_cast<unsigned char>(header[0]) == 0xFD && header[1] == '7' &&
        header[2] == 'z' && header[3] == 'X' && header[4] == 'Z' && header[5] == 0x00)
        return "xz";
    if (header.size() >= 3 && header[0] == 'B' && header[1] == 'Z' && header[2] == 'h')
        return "bzip2";
    if (header.size() >= 4 &&
        static_cast<unsigned char>(header[0]) == 0x28 &&
        static_cast<unsigned char>(header[1]) == 0xB5 &&
        static_cast<unsigned char>(header[2]) == 0x2F &&
        static_cast<unsigned char>(header[3]) == 0xFD)
        return "zstd";
    if (header.size() >= 4 && header[0] == 'P' && header[1] == 'K' &&
        static_cast<unsigned char>(header[2]) == 0x03 &&
        static_cast<unsigned char>(header[3]) == 0x04)
        return "zip";

    if (fileNameLower.endsWith(".gz")) return "gzip";
    if (fileNameLower.endsWith(".xz")) return "xz";
    if (fileNameLower.endsWith(".bz2")) return "bzip2";
    if (fileNameLower.endsWith(".zst")) return "zstd";
    if (fileNameLower.endsWith(".zip")) return "zip";
    return {};
}

bool hasMbrSignature(const QByteArray &sector0) {
    if (sector0.size() < 512)
        return false;
    const bool sig = static_cast<unsigned char>(sector0[510]) == 0x55 &&
                     static_cast<unsigned char>(sector0[511]) == 0xAA;
    if (!sig)
        return false;
    // Check that at least one partition entry (446..509) is non-zero or boot code exists.
    for (int i = 446; i < 510; ++i) {
        if (sector0[i] != 0)
            return true;
    }
    return false;
}

bool hasGptHeader(const QByteArray &sector1) {
    return sector1.size() >= 8 && sector1.left(8) == QByteArray("EFI PART", 8);
}

} // namespace

QJsonObject IsoAnalysis::toJson() const {
    return QJsonObject{
        {"valid", valid},
        {"path", path},
        {"fileName", fileName},
        {"fileSize", fileSize},
        {"kind", kind},
        {"summary", summary},
        {"volumeLabel", volumeLabel},
        {"isIso9660", isIso9660},
        {"isHybrid", isHybrid},
        {"isCompressed", isCompressed},
        {"compressionFormat", compressionFormat},
        {"isWindows", isWindows},
        {"isWindows11", isWindows11},
        {"isLinux", isLinux},
        {"hasUefiBoot", hasUefiBoot},
        {"hasBiosBoot", hasBiosBoot},
        {"hasLargeFile", hasLargeFile},
        {"supportsPersistence", supportsPersistence},
        {"persistenceType", persistenceType},
        {"windowsArch", windowsArch},
        {"fileCount", fileCount},
        {"totalExtractedBytes", totalExtractedBytes},
        {"recommendedWriteMode", recommendedWriteMode},
        {"recommendedScheme", recommendedScheme},
        {"recommendedFileSystem", recommendedFileSystem},
        {"recommendUefiNtfs", recommendUefiNtfs},
        {"error", error},
    };
}

QVariantMap IsoAnalysis::toVariantMap() const {
    return toJson().toVariantMap();
}

QString IsoAnalyzer::sanitizeLabel(const QString &raw, const QString &fileSystem) {
    QString label = raw.trimmed();
    label.replace(QRegularExpression(R"([^A-Za-z0-9 _\-])"), "_");
    label = label.trimmed();
    if (label.isEmpty())
        label = "RUFUS_USB";

    const QString fs = fileSystem.toLower();
    if (fs == "fat32" || fs == "fat16") {
        label = label.toUpper().left(11).trimmed();
        if (label.isEmpty())
            label = "RUFUS_USB";
    } else if (fs == "ext2" || fs == "ext3" || fs == "ext4") {
        label = label.left(16).trimmed();
    } else if (fs == "exfat") {
        label = label.left(15).trimmed();
    } else {
        label = label.left(32).trimmed();
    }
    return label;
}

IsoAnalysis IsoAnalyzer::analyze(const QString &path) {
    IsoAnalysis result;
    result.path = path;

    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        result.error = QObject::tr("Image file does not exist.");
        return result;
    }

    result.fileName = info.fileName();
    result.fileSize = info.size();
    const QString lowerName = result.fileName.toLower();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QObject::tr("Cannot read image file: %1").arg(file.errorString());
        return result;
    }

    const QByteArray first1K = file.read(1024);
    const QString compression = detectCompressionByMagic(first1K, lowerName);
    if (!compression.isEmpty()) {
        result.valid = true;
        result.isCompressed = true;
        result.compressionFormat = compression;
        result.kind = "compressed_image";
        result.recommendedWriteMode = "dd";
        result.recommendedScheme = "gpt";
        result.recommendedFileSystem = "fat32";
        result.volumeLabel = sanitizeLabel(info.completeBaseName(), "fat32");
        result.summary = QObject::tr("Compressed disk image (%1) — DD Image mode").arg(compression.toUpper());
        return result;
    }

    const bool mbrBoot = hasMbrSignature(first1K.left(512));
    const bool gptBoot = hasGptHeader(first1K.mid(512, 512));

    // Check ISO-9660 Primary Volume Descriptor at byte offset 0x8000 (sector 16).
    if (result.fileSize >= 0x8050 && file.seek(0x8000)) {
        const QByteArray pvd = file.read(128);
        if (pvd.size() >= 72 && pvd.mid(1, 5) == "CD001") {
            result.isIso9660 = true;
            const QByteArray rawVol = pvd.mid(40, 32);
            const int nulIdx = rawVol.indexOf('\0');
            const QString volId = QString::fromLatin1(nulIdx >= 0 ? rawVol.left(nulIdx) : rawVol).trimmed();
            if (!volId.isEmpty())
                result.volumeLabel = volId;
        }
    }
    file.close();

    if (result.isIso9660) {
        result.isHybrid = mbrBoot || gptBoot;
        if (mbrBoot)
            result.hasBiosBoot = true;
        if (gptBoot)
            result.hasUefiBoot = true;

        // Inspect ISO contents cleanly through libarchive without extracting.
        struct archive *a = archive_read_new();
        archive_read_support_format_iso9660(a);
        archive_read_support_format_all(a);
        archive_read_support_filter_none(a);

        bool hasSourcesBootWim = false;
        bool hasSourcesInstall = false;
        bool hasBootmgr = false;
        bool hasArm64Efi = false;
        bool hasX64Efi = false;
        bool hasIa32Efi = false;
        bool hasCasper = false;
        bool hasDebianLive = false;
        bool hasLinuxKernel = false;
        bool hasIsolinux = false;
        bool hasGrub = false;

        if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 65536) == ARCHIVE_OK) {
            struct archive_entry *entry = nullptr;
            while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
                const char *rawPath = archive_entry_pathname(entry);
                const qint64 entrySize = static_cast<qint64>(archive_entry_size(entry));
                if (archive_entry_filetype(entry) == AE_IFREG) {
                    result.fileCount++;
                    result.totalExtractedBytes += entrySize;
                    if (entrySize >= kFourGiB)
                        result.hasLargeFile = true;
                }
                if (rawPath) {
                    QString p = QString::fromUtf8(rawPath).toLower();
                    while (p.startsWith("./") || p.startsWith('/'))
                        p.remove(0, p.startsWith("./") ? 2 : 1);

                    if (p == "sources/boot.wim") hasSourcesBootWim = true;
                    if (p == "sources/install.wim" || p == "sources/install.esd" || p.startsWith("sources/install.swm"))
                        hasSourcesInstall = true;
                    if (p == "bootmgr" || p == "bootmgr.efi") hasBootmgr = true;

                    if (p == "efi/boot/bootx64.efi") { result.hasUefiBoot = true; hasX64Efi = true; }
                    if (p == "efi/boot/bootaa64.efi") { result.hasUefiBoot = true; hasArm64Efi = true; }
                    if (p == "efi/boot/bootia32.efi") { result.hasUefiBoot = true; hasIa32Efi = true; }
                    if (p.startsWith("efi/boot/") && p.endsWith(".efi")) result.hasUefiBoot = true;

                    if (p.startsWith("casper/")) { hasCasper = true; hasLinuxKernel = true; }
                    if (p.startsWith("live/")) { hasDebianLive = true; hasLinuxKernel = true; }
                    if (p.startsWith("arch/") || p.contains("vmlinuz") || p.contains("initramfs") || p.contains("squashfs"))
                        hasLinuxKernel = true;
                    if (p.startsWith("isolinux/") || p.startsWith("syslinux/")) { hasIsolinux = true; result.hasBiosBoot = true; }
                    if (p.startsWith("boot/grub/")) hasGrub = true;
                }
                archive_read_data_skip(a);
            }
        }
        archive_read_free(a);

        result.isWindows = (hasSourcesBootWim && (hasSourcesInstall || hasBootmgr)) ||
                           (hasSourcesInstall && hasBootmgr);
        result.isLinux = !result.isWindows && (hasLinuxKernel || hasIsolinux || hasGrub || result.isHybrid);

        if (result.isWindows) {
            if (hasArm64Efi && !hasX64Efi)
                result.windowsArch = "arm64";
            else if (hasIa32Efi && !hasX64Efi)
                result.windowsArch = "x86";
            else
                result.windowsArch = "amd64";

            const QString upperLabel = result.volumeLabel.toUpper();
            result.isWindows11 = lowerName.contains("win11") || lowerName.contains("windows11") ||
                                 lowerName.contains("windows_11") || lowerName.contains("windows 11") ||
                                 upperLabel.contains("22000") || upperLabel.contains("22621") ||
                                 upperLabel.contains("22631") || upperLabel.contains("26100") ||
                                 upperLabel.contains("CCCOMA") || result.hasLargeFile;

            result.kind = "windows_iso";
            result.recommendedWriteMode = "iso";
            result.recommendedScheme = result.hasUefiBoot ? "gpt" : "mbr";
            if (result.hasLargeFile) {
                result.recommendedFileSystem = "ntfs";
                result.recommendUefiNtfs = result.hasUefiBoot;
            } else {
                result.recommendedFileSystem = "fat32";
                result.recommendUefiNtfs = false;
            }
            result.summary = result.hasLargeFile
                ? QObject::tr("Windows ISO (%1) — contains >4 GiB file (NTFS + UEFI:NTFS recommended)").arg(result.windowsArch)
                : QObject::tr("Windows ISO (%1) — ISO Image mode").arg(result.windowsArch);
        } else if (result.isHybrid) {
            result.kind = "hybrid_iso";
            result.recommendedWriteMode = "dd";
            result.recommendedScheme = result.hasUefiBoot ? "gpt" : "mbr";
            result.recommendedFileSystem = "fat32";
            if (hasCasper) {
                result.supportsPersistence = true;
                result.persistenceType = "casper";
            } else if (hasDebianLive) {
                result.supportsPersistence = true;
                result.persistenceType = "debian";
            }
            result.summary = result.supportsPersistence
                ? QObject::tr("Hybrid Linux ISO (DD or ISO mode, persistence supported)")
                : QObject::tr("Hybrid ISO image (DD Image mode recommended, ISO mode available)");
        } else {
            result.kind = result.isLinux ? "linux_iso" : "iso";
            result.recommendedWriteMode = "iso";
            result.recommendedScheme = result.hasUefiBoot ? "gpt" : "mbr";
            result.recommendedFileSystem = result.hasLargeFile ? "ntfs" : "fat32";
            result.recommendUefiNtfs = result.hasLargeFile && result.hasUefiBoot;
            if (hasCasper) {
                result.supportsPersistence = true;
                result.persistenceType = "casper";
            } else if (hasDebianLive) {
                result.supportsPersistence = true;
                result.persistenceType = "debian";
            }
            result.summary = QObject::tr("ISO-9660 image — ISO Image mode");
        }

        if (result.volumeLabel.isEmpty())
            result.volumeLabel = sanitizeLabel(info.completeBaseName(), result.recommendedFileSystem);
        result.valid = true;
        return result;
    }

    // Raw disk image (.img, .raw, .bin, or any file with MBR/GPT header).
    if (mbrBoot || gptBoot || lowerName.endsWith(".img") || lowerName.endsWith(".raw") || lowerName.endsWith(".bin")) {
        result.valid = true;
        result.kind = "raw_image";
        result.hasUefiBoot = gptBoot;
        result.hasBiosBoot = mbrBoot;
        result.recommendedWriteMode = "dd";
        result.recommendedScheme = gptBoot ? "gpt" : "mbr";
        result.recommendedFileSystem = "fat32";
        result.volumeLabel = sanitizeLabel(info.completeBaseName(), "fat32");
        result.summary = gptBoot
            ? QObject::tr("Raw GPT disk image — DD Image mode")
            : mbrBoot ? QObject::tr("Raw MBR disk image — DD Image mode")
                      : QObject::tr("Raw disk image — DD Image mode");
        return result;
    }

    result.error = QObject::tr("Unsupported image format. Select an ISO, raw disk image (.img/.raw), or compressed image (.gz/.xz/.bz2/.zst).");
    return result;
}

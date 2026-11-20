#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

struct IsoAnalysis {
    bool valid = false;
    QString path;
    QString fileName;
    qint64 fileSize = 0;

    // Raw classification: "iso", "hybrid_iso", "windows_iso", "linux_iso", "raw_image", "compressed_image"
    QString kind = "unknown";
    QString summary;
    QString volumeLabel;

    bool isIso9660 = false;
    bool isHybrid = false;
    bool isCompressed = false;
    QString compressionFormat; // "gzip", "xz", "bzip2", "zstd", "zip", or ""

    bool isWindows = false;
    bool isWindows11 = false;
    bool isLinux = false;
    bool hasUefiBoot = false;
    bool hasBiosBoot = false;
    bool hasLargeFile = false; // >= 4 GiB file inside ISO (requires NTFS/exFAT + UEFI:NTFS)
    bool supportsPersistence = false;
    QString persistenceType;   // "casper" or "debian"
    QString windowsArch = "amd64"; // "amd64", "arm64", "x86"

    int fileCount = 0;
    qint64 totalExtractedBytes = 0;

    // Recommended defaults derived from inspection (mirroring Rufus heuristics).
    QString recommendedWriteMode = "iso"; // "iso" or "dd"
    QString recommendedScheme = "gpt";    // "gpt" or "mbr"
    QString recommendedFileSystem = "fat32";
    bool recommendUefiNtfs = false;

    QString error;

    QJsonObject toJson() const;
    QVariantMap toVariantMap() const;
};

class IsoAnalyzer {
public:
    static IsoAnalysis analyze(const QString &path);
    static QString sanitizeLabel(const QString &raw, const QString &fileSystem);
};

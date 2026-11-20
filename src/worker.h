#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Privileged disk worker. Reads a single JSON job specification from stdin and
// emits line-delimited JSON events ("log", "progress", "done", "error") on
// stdout. Also exposes static helpers for lsblk device discovery and safety
// validation so the GUI, CLI, and unit tests share one source of truth.
class DiskWorker {
public:
    // Parses `lsblk -J -b` output and returns safe removable / USB disks.
    // Never returns disks containing system mountpoints (/, /boot, /boot/efi, /home, /var).
    static QJsonArray filterDevices(const QJsonObject &lsblkJson, bool includeUsbHdd);

    // Queries the live system via lsblk and returns filtered devices.
    static QJsonArray listDevices(bool includeUsbHdd, QString *errorOut = nullptr);

    // Verifies that a target device path is a safe non-system USB/removable disk
    // (or regular file when allowRegularFile is true for automated tests).
    static bool validateTargetDevice(const QString &devicePath, bool includeUsbHdd,
                                     bool allowRegularFile, QString *reasonOut = nullptr);

    // Executes a job described by JSON. Writes JSON progress events to stdout.
    // Returns 0 on success, non-zero on failure.
    static int runJob(const QJsonObject &job, bool allowRegularFile = false);

    // Entry point for `rufus-linux --worker`.
    static int runFromStdin(bool allowRegularFile = false);
};

#pragma once

#include "filepicker.h"
#include "isoanalyzer.h"
#include "unattend.h"

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>

class Backend : public QObject {
    Q_OBJECT

    // Devices
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged)
    Q_PROPERTY(int selectedDeviceIndex READ selectedDeviceIndex WRITE setSelectedDeviceIndex NOTIFY selectedDeviceChanged)
    Q_PROPERTY(QVariantMap selectedDevice READ selectedDevice NOTIFY selectedDeviceChanged)
    Q_PROPERTY(bool listUsbHdd READ listUsbHdd WRITE setListUsbHdd NOTIFY optionsChanged)

    // Boot selection & ISO analysis
    Q_PROPERTY(QString bootSelection READ bootSelection WRITE setBootSelection NOTIFY optionsChanged)
    Q_PROPERTY(QString imagePath READ imagePath NOTIFY imageChanged)
    Q_PROPERTY(QString imageName READ imageName NOTIFY imageChanged)
    Q_PROPERTY(QString imageSizeText READ imageSizeText NOTIFY imageChanged)
    Q_PROPERTY(QString imageSummary READ imageSummary NOTIFY imageChanged)
    Q_PROPERTY(bool imageValid READ imageValid NOTIFY imageChanged)
    Q_PROPERTY(bool isWindowsIso READ isWindowsIso NOTIFY imageChanged)
    Q_PROPERTY(bool isHybridIso READ isHybridIso NOTIFY imageChanged)
    Q_PROPERTY(bool isCompressedImage READ isCompressedImage NOTIFY imageChanged)
    Q_PROPERTY(bool hasLargeFile READ hasLargeFile NOTIFY imageChanged)
    Q_PROPERTY(bool supportsPersistence READ supportsPersistence NOTIFY imageChanged)
    Q_PROPERTY(QString windowsArch READ windowsArch NOTIFY imageChanged)

    // Partition & format options
    Q_PROPERTY(QString writeMode READ writeMode WRITE setWriteMode NOTIFY optionsChanged)
    Q_PROPERTY(QString partitionScheme READ partitionScheme WRITE setPartitionScheme NOTIFY optionsChanged)
    Q_PROPERTY(QString targetSystemText READ targetSystemText NOTIFY optionsChanged)
    Q_PROPERTY(bool uefiNtfs READ uefiNtfs WRITE setUefiNtfs NOTIFY optionsChanged)
    Q_PROPERTY(int persistenceMB READ persistenceMB WRITE setPersistenceMB NOTIFY optionsChanged)
    Q_PROPERTY(int maxPersistenceMB READ maxPersistenceMB NOTIFY selectedDeviceChanged)

    Q_PROPERTY(QString volumeLabel READ volumeLabel WRITE setVolumeLabel NOTIFY optionsChanged)
    Q_PROPERTY(QString fileSystem READ fileSystem WRITE setFileSystem NOTIFY optionsChanged)
    Q_PROPERTY(int clusterSize READ clusterSize WRITE setClusterSize NOTIFY optionsChanged)
    Q_PROPERTY(bool quickFormat READ quickFormat WRITE setQuickFormat NOTIFY optionsChanged)
    Q_PROPERTY(bool checkBadBlocks READ checkBadBlocks WRITE setCheckBadBlocks NOTIFY optionsChanged)
    Q_PROPERTY(int badBlocksPasses READ badBlocksPasses WRITE setBadBlocksPasses NOTIFY optionsChanged)
    Q_PROPERTY(QString missingToolWarning READ missingToolWarning NOTIFY optionsChanged)

    // Windows User Experience (autounattend.xml)
    Q_PROPERTY(bool wueEnabled READ wueEnabled WRITE setWueEnabled NOTIFY wueChanged)
    Q_PROPERTY(bool wueBypassTpm READ wueBypassTpm WRITE setWueBypassTpm NOTIFY wueChanged)
    Q_PROPERTY(bool wueBypassMsa READ wueBypassMsa WRITE setWueBypassMsa NOTIFY wueChanged)
    Q_PROPERTY(bool wueCreateLocalAccount READ wueCreateLocalAccount WRITE setWueCreateLocalAccount NOTIFY wueChanged)
    Q_PROPERTY(QString wueLocalUsername READ wueLocalUsername WRITE setWueLocalUsername NOTIFY wueChanged)
    Q_PROPERTY(bool wueDisableDataCollection READ wueDisableDataCollection WRITE setWueDisableDataCollection NOTIFY wueChanged)
    Q_PROPERTY(bool wueDisableBitLocker READ wueDisableBitLocker WRITE setWueDisableBitLocker NOTIFY wueChanged)
    Q_PROPERTY(bool wueQolEnhancements READ wueQolEnhancements WRITE setWueQolEnhancements NOTIFY wueChanged)
    Q_PROPERTY(QString wuePreviewXml READ wuePreviewXml NOTIFY wueChanged)

    // Checksums
    Q_PROPERTY(bool hashing READ hashing NOTIFY hashChanged)
    Q_PROPERTY(double hashProgress READ hashProgress NOTIFY hashChanged)
    Q_PROPERTY(QString md5 READ md5 NOTIFY hashChanged)
    Q_PROPERTY(QString sha1 READ sha1 NOTIFY hashChanged)
    Q_PROPERTY(QString sha256 READ sha256 NOTIFY hashChanged)
    Q_PROPERTY(QString sha512 READ sha512 NOTIFY hashChanged)

    // Operation state & logs
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canStart READ canStart NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString speedText READ speedText NOTIFY stateChanged)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString successText READ successText NOTIFY stateChanged)
    Q_PROPERTY(QString logText READ logText NOTIFY logChanged)
    Q_PROPERTY(QString confirmationSummary READ confirmationSummary NOTIFY stateChanged)

public:
    explicit Backend(FilePicker *filePicker = nullptr, bool enablePolling = true, QObject *parent = nullptr);
    ~Backend() override;

    QVariantList devices() const { return m_devices; }
    int deviceCount() const { return m_devices.size(); }
    int selectedDeviceIndex() const { return m_selectedDeviceIndex; }
    void setSelectedDeviceIndex(int idx);
    QVariantMap selectedDevice() const;
    bool listUsbHdd() const { return m_listUsbHdd; }
    void setListUsbHdd(bool v);

    QString bootSelection() const { return m_bootSelection; }
    void setBootSelection(const QString &v);
    QString imagePath() const { return m_analysis.path; }
    QString imageName() const { return m_analysis.fileName; }
    QString imageSizeText() const;
    QString imageSummary() const { return m_analysis.summary; }
    bool imageValid() const { return m_analysis.valid; }
    bool isWindowsIso() const { return m_analysis.isWindows; }
    bool isHybridIso() const { return m_analysis.isHybrid; }
    bool isCompressedImage() const { return m_analysis.isCompressed; }
    bool hasLargeFile() const { return m_analysis.hasLargeFile; }
    bool supportsPersistence() const { return m_analysis.supportsPersistence && m_writeMode == "iso"; }
    QString windowsArch() const { return m_analysis.windowsArch; }

    QString writeMode() const { return m_writeMode; }
    void setWriteMode(const QString &v);
    QString partitionScheme() const { return m_partitionScheme; }
    void setPartitionScheme(const QString &v);
    QString targetSystemText() const;
    bool uefiNtfs() const { return m_uefiNtfs; }
    void setUefiNtfs(bool v);
    int persistenceMB() const { return m_persistenceMB; }
    void setPersistenceMB(int mb);
    int maxPersistenceMB() const;

    QString volumeLabel() const { return m_volumeLabel; }
    void setVolumeLabel(const QString &v);
    QString fileSystem() const { return m_fileSystem; }
    void setFileSystem(const QString &v);
    int clusterSize() const { return m_clusterSize; }
    void setClusterSize(int bytes);
    bool quickFormat() const { return m_quickFormat; }
    void setQuickFormat(bool v);
    bool checkBadBlocks() const { return m_checkBadBlocks; }
    void setCheckBadBlocks(bool v);
    int badBlocksPasses() const { return m_badBlocksPasses; }
    void setBadBlocksPasses(int passes);
    QString missingToolWarning() const;

    bool wueEnabled() const { return m_wue.enabled; }
    void setWueEnabled(bool v);
    bool wueBypassTpm() const { return m_wue.bypassTpmSecureBootRam; }
    void setWueBypassTpm(bool v);
    bool wueBypassMsa() const { return m_wue.bypassOnlineAccount; }
    void setWueBypassMsa(bool v);
    bool wueCreateLocalAccount() const { return m_wue.createLocalAccount; }
    void setWueCreateLocalAccount(bool v);
    QString wueLocalUsername() const { return m_wue.localUsername; }
    void setWueLocalUsername(const QString &v);
    bool wueDisableDataCollection() const { return m_wue.disableDataCollection; }
    void setWueDisableDataCollection(bool v);
    bool wueDisableBitLocker() const { return m_wue.disableBitLocker; }
    void setWueDisableBitLocker(bool v);
    bool wueQolEnhancements() const { return m_wue.qolEnhancements; }
    void setWueQolEnhancements(bool v);
    QString wuePreviewXml() const { return UnattendGenerator::generate(m_wue); }

    bool hashing() const { return m_hashing; }
    double hashProgress() const { return m_hashProgress; }
    QString md5() const { return m_md5; }
    QString sha1() const { return m_sha1; }
    QString sha256() const { return m_sha256; }
    QString sha512() const { return m_sha512; }

    bool busy() const { return m_busy; }
    bool canStart() const;
    double progress() const { return m_progress; }
    QString statusText() const { return m_statusText; }
    QString speedText() const { return m_speedText; }
    QString elapsedText() const { return m_elapsedText; }
    QString errorText() const { return m_errorText; }
    QString successText() const { return m_successText; }
    QString logText() const { return m_logs.join('\n'); }
    QString confirmationSummary() const;

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void openImageDialog();
    Q_INVOKABLE void openSaveDriveDialog();
    Q_INVOKABLE void openSaveLogDialog();
    Q_INVOKABLE void selectImage(const QString &pathOrUrl);
    Q_INVOKABLE void clearImage();
    Q_INVOKABLE void computeChecksums();
    Q_INVOKABLE void cancelChecksums();
    Q_INVOKABLE QString verifyHash(const QString &expected) const;
    Q_INVOKABLE void startOperation();
    Q_INVOKABLE void cancelOperation();
    Q_INVOKABLE void clearBanner();
    Q_INVOKABLE void copyToClipboard(const QString &text);
    Q_INVOKABLE void clearLog();

    // Used by tests to inject mock device lists without calling lsblk.
    void setDevicesForTest(const QVariantList &devices);
    QJsonObject buildJobPayload() const;

signals:
    void devicesChanged();
    void selectedDeviceChanged();
    void imageChanged();
    void optionsChanged();
    void wueChanged();
    void hashChanged();
    void stateChanged();
    void logChanged();

private slots:
    void onWorkerStdout();
    void onWorkerStderr();
    void onWorkerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onElapsedTick();
    void onSavePathPicked(const QString &path, const QString &tag);

private:
    void appendLog(const QString &line);
    void launchWorkerJob(const QJsonObject &job);
    void updateElapsedText();
    static QString formatBytes(qint64 bytes);

    FilePicker *m_filePicker = nullptr;
    QTimer m_devicePollTimer;
    QTimer m_elapsedTimerTick;
    QElapsedTimer m_operationClock;

    QVariantList m_devices;
    int m_selectedDeviceIndex = -1;
    bool m_listUsbHdd = false;

    QString m_bootSelection = "image";
    IsoAnalysis m_analysis;

    QString m_writeMode = "iso";
    QString m_partitionScheme = "gpt";
    bool m_uefiNtfs = false;
    int m_persistenceMB = 0;

    QString m_volumeLabel = "RUFUS_USB";
    QString m_fileSystem = "fat32";
    int m_clusterSize = 0;
    bool m_quickFormat = true;
    bool m_checkBadBlocks = false;
    int m_badBlocksPasses = 1;

    WindowsUnattendOptions m_wue;

    // Hashing state
    bool m_hashing = false;
    double m_hashProgress = 0.0;
    QString m_md5, m_sha1, m_sha256, m_sha512;
    QThread *m_hashThread = nullptr;
    std::shared_ptr<std::atomic_bool> m_hashCancel;

    // Worker process state
    QProcess m_worker;
    QByteArray m_workerStdoutBuf;
    bool m_busy = false;
    bool m_cancelRequested = false;
    double m_progress = 0.0;
    QString m_statusText;
    QString m_speedText;
    QString m_elapsedText = "00:00:00";
    QString m_errorText;
    QString m_successText;
    QStringList m_logs;
};

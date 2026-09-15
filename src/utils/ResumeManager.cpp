#include "ResumeManager.h"
#include <QCryptographicHash>

static QString makeKey(const QString& filePath) {
    QByteArray hash = QCryptographicHash::hash(
        filePath.toUtf8(), QCryptographicHash::Md5).toHex();
    return QStringLiteral("resume/%1").arg(QString::fromLatin1(hash));
}

ResumeManager::ResumeManager()
    : settings_("qt_player", "resume") {}

void ResumeManager::saveResumePoint(const QString& filePath, double position) {
    if (position < 5.0) return; // 太短不记录
    settings_.setValue(makeKey(filePath), position);
    settings_.sync();
}

std::optional<double> ResumeManager::getResumePoint(const QString& filePath) {
    QString key = makeKey(filePath);
    if (!settings_.contains(key)) return std::nullopt;
    return settings_.value(key).toDouble();
}

void ResumeManager::clearResumePoint(const QString& filePath) {
    settings_.remove(makeKey(filePath));
    settings_.sync();
}
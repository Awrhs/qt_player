#ifndef RESUME_MANAGER_H
#define RESUME_MANAGER_H

#include <QString>
#include <QSettings>
#include <optional>

/**
 * @brief 断点续播管理
 *
 * 使用 QSettings 持久化文件路径 → 播放位置的映射。
 * 存储位置：QSettings("qt_player", "resume")
 */
class ResumeManager {
public:
    ResumeManager();

    /**
     * @brief 保存断点
     * @param filePath 文件路径
     * @param position 播放位置（秒）
     */
    void saveResumePoint(const QString& filePath, double position);

    /**
     * @brief 获取断点
     * @param filePath 文件路径
     * @return 播放位置（秒），无记录时返回 nullopt
     */
    std::optional<double> getResumePoint(const QString& filePath);

    /**
     * @brief 清除断点
     * @param filePath 文件路径
     */
    void clearResumePoint(const QString& filePath);

private:
    QSettings settings_;
};

#endif // RESUME_MANAGER_H
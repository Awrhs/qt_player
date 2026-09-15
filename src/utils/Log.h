#ifndef LOG_H
#define LOG_H

#include <QString>
#include <QDebug>
#include <QDateTime>

/**
 * @brief 简单日志工具
 *
 * 提供分级日志输出，格式：时间 [级别] 消息
 * 后续可扩展写入文件
 */
namespace Log {

enum Level {
    Debug,
    Info,
    Warning,
    Error
};

inline QString levelString(Level lv) {
    switch (lv) {
        case Debug:   return QStringLiteral("D");
        case Info:    return QStringLiteral("I");
        case Warning: return QStringLiteral("W");
        case Error:   return QStringLiteral("E");
    }
    return QStringLiteral("?");
}

inline void write(Level lv, const QString& tag, const QString& msg) {
    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString line = QString("[%1][%2] %3").arg(levelString(lv), tag, msg);
    qDebug().noquote() << line;
    // TODO: 写入日志文件
}

template <typename... Args>
void d(const char* tag, const QString& fmt, Args&&... args) {
    write(Debug, tag, fmt.arg(std::forward<Args>(args)...));
}

template <typename... Args>
void i(const char* tag, const QString& fmt, Args&&... args) {
    write(Info, tag, fmt.arg(std::forward<Args>(args)...));
}

template <typename... Args>
void w(const char* tag, const QString& fmt, Args&&... args) {
    write(Warning, tag, fmt.arg(std::forward<Args>(args)...));
}

template <typename... Args>
void e(const char* tag, const QString& fmt, Args&&... args) {
    write(Error, tag, fmt.arg(std::forward<Args>(args)...));
}

} // namespace Log

#define LOG_TAG(tag) static constexpr const char* LOG_TAG_ = tag;
#define LOG_D(msg, ...)  Log::d(LOG_TAG_, msg, ##__VA_ARGS__)
#define LOG_I(msg, ...)  Log::i(LOG_TAG_, msg, ##__VA_ARGS__)
#define LOG_W(msg, ...)  Log::w(LOG_TAG_, msg, ##__VA_ARGS__)
#define LOG_E(msg, ...)  Log::e(LOG_TAG_, msg, ##__VA_ARGS__)

#endif // LOG_H
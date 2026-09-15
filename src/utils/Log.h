#ifndef LOG_H
#define LOG_H

#include <QString>
#include <QDebug>
#include <QDateTime>

#include <string>
#include <type_traits>

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

namespace detail {

// 将日志参数统一转换为 QString，兼容 Qt 6.4 的 QString::arg
// （Qt 6.4 的变参 arg 要求每个参数可转换为 QString/QStringView/QLatin1StringView，
//   不接受数值类型，且要求至少 2 个参数）
template <typename T>
QString toArgString(T&& v) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, QString>)
        return std::forward<T>(v);
    else if constexpr (std::is_same_v<D, std::string>)
        return QString::fromStdString(v);
    else if constexpr (std::is_same_v<D, QByteArray>)
        return QString::fromUtf8(v);
    else if constexpr (std::is_same_v<D, bool>)
        return v ? QStringLiteral("true") : QStringLiteral("false");
    else if constexpr (std::is_integral_v<D> || std::is_floating_point_v<D>)
        return QString::number(v);
    else if constexpr (std::is_enum_v<D>)
        return QString::number(static_cast<std::underlying_type_t<D>>(v));
    else if constexpr (std::is_same_v<D, void*>)
        return QStringLiteral("0x") + QString::number(reinterpret_cast<quintptr>(v), 16);
    else if constexpr (std::is_convertible_v<D, QStringView>)
        return QString(QStringView(v));
    else if constexpr (std::is_convertible_v<D, QLatin1StringView>)
        return QString(QLatin1StringView(v));
    else // const char*、char[] 等
        return QString::fromUtf8(v);
}

template <typename... Args>
QString format(const QString& fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0)
        return fmt;
    else
        return fmt.arg(toArgString(std::forward<Args>(args))...);
}

} // namespace detail

template <typename... Args>
void d(const char* tag, const QString& fmt, Args&&... args) {
    write(Debug, tag, detail::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void i(const char* tag, const QString& fmt, Args&&... args) {
    write(Info, tag, detail::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void w(const char* tag, const QString& fmt, Args&&... args) {
    write(Warning, tag, detail::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void e(const char* tag, const QString& fmt, Args&&... args) {
    write(Error, tag, detail::format(fmt, std::forward<Args>(args)...));
}

} // namespace Log

#define LOG_TAG(tag) static constexpr const char* LOG_TAG_ = tag;
#define LOG_D(msg, ...)  Log::d(LOG_TAG_, msg, ##__VA_ARGS__)
#define LOG_I(msg, ...)  Log::i(LOG_TAG_, msg, ##__VA_ARGS__)
#define LOG_W(msg, ...)  Log::w(LOG_TAG_, msg, ##__VA_ARGS__)
#define LOG_E(msg, ...)  Log::e(LOG_TAG_, msg, ##__VA_ARGS__)

#endif // LOG_H
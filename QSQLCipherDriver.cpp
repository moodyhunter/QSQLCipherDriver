/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtSql module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "QSQLCipherDriver.hpp"

#include <QDateTime>
#include <QDebug>
#include <QMetaType>
#include <QScopedValueRollback>
#include <QSqlError>
#include <QSqlField>
#include <QSqlIndex>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <QtSql/private/qsqlcachedresult_p.h>
#include <QtSql/private/qsqldriver_p.h>

#if QT_CONFIG(regularexpression)
#include <QCache>
#include <QRegularExpression>
#endif

#if defined Q_OS_WIN
#include <qt_windows.h>
#else
#include <unistd.h>
#endif

#define SQLITE_HAS_CODEC

#ifdef Q_OS_MAC
#include <sqlite3.h>
#else
#include <sqlcipher/sqlite3.h>
#endif

Q_DECLARE_OPAQUE_POINTER(sqlite3 *)
Q_DECLARE_METATYPE(sqlite3 *)

Q_DECLARE_OPAQUE_POINTER(sqlite3_stmt *)
Q_DECLARE_METATYPE(sqlite3_stmt *)

#define DISABLE_COLUMN_METADATA

static QString _q_escapeIdentifier(const QString &identifier, QSqlDriver::IdentifierType type)
{
    QString res = identifier;
    // If it contains [ and ] then we assume it to be escaped properly already as this indicates
    // the syntax is exactly how it should be
    if (identifier.contains(u'[') && identifier.contains(u']'))
        return res;
    if (!identifier.isEmpty() && !identifier.startsWith(u'"') && !identifier.endsWith(u'"'))
    {
        res.replace(u'"', u"\"\""_qs);
        res.prepend(u'"').append(u'"');
        if (type == QSqlDriver::TableName)
            res.replace(u'.', u"\".\""_qs);
    }
    return res;
}

static int qGetColumnType(const QString &tpName)
{
    const QString typeName = tpName.toLower();

    if (typeName == QStringLiteral("integer") || typeName == QStringLiteral("int"))
        return QMetaType::Int;
    if (typeName == QStringLiteral("double") || typeName == QStringLiteral("float") || typeName == QStringLiteral("real") ||
        typeName.startsWith(QStringLiteral("numeric")))
        return QMetaType::Double;
    if (typeName == QStringLiteral("blob"))
        return QMetaType::QByteArray;
    if (typeName == QStringLiteral("boolean") || typeName == QStringLiteral("bool"))
        return QMetaType::Bool;
    return QMetaType::QString;
}

static QSqlError qMakeError(sqlite3 *access, const QString &descr, QSqlError::ErrorType type, int errorCode)
{
    return QSqlError(descr, QString(reinterpret_cast<const QChar *>(sqlite3_errmsg16(access))), type, QString::number(errorCode));
}

class QSQLCipherResultPrivate;

class QSQLCipherResult : public QSqlCachedResult
{
    Q_DECLARE_PRIVATE(QSQLCipherResult)
    friend class QSQLCipherDriver;

  public:
    explicit QSQLCipherResult(const QSQLCipherDriver *db);
    ~QSQLCipherResult();
    QVariant handle() const override;

  protected:
    bool gotoNext(QSqlCachedResult::ValueCache &row, int idx) override;
    bool reset(const QString &query) override;
    bool prepare(const QString &query) override;
    bool execBatch(bool arrayBind) override;
    bool exec() override;
    int size() override;
    int numRowsAffected() override;
    QVariant lastInsertId() const override;
    QSqlRecord record() const override;
    void detachFromResultSet() override;
    void virtual_hook(int id, void *data) override;
};

class QSQLCipherDriverPrivate : public QSqlDriverPrivate
{
    Q_DECLARE_PUBLIC(QSQLCipherDriver)

  public:
    inline QSQLCipherDriverPrivate() : QSqlDriverPrivate(QSqlDriver::SQLite)
    {
    }
    sqlite3 *access = nullptr;
    QList<QSQLCipherResult *> results;
    QStringList notificationid;
};

class QSQLCipherResultPrivate : public QSqlCachedResultPrivate
{
    Q_DECLARE_PUBLIC(QSQLCipherResult)

  public:
    Q_DECLARE_SQLDRIVER_PRIVATE(QSQLCipherDriver)
    using QSqlCachedResultPrivate::QSqlCachedResultPrivate;
    void cleanup();
    bool fetchNext(QSqlCachedResult::ValueCache &values, int idx, bool initialFetch);
    // initializes the recordInfo and the cache
    void initColumns(bool emptyResultset);
    void finalize();

    sqlite3_stmt *stmt = nullptr;
    QSqlRecord rInf;
    QList<QVariant> firstRow;
    bool skippedStatus = false; // the status of the fetchNext() that's skipped
    bool skipRow = false;       // skip the next fetchNext()?
};

void QSQLCipherResultPrivate::cleanup()
{
    Q_Q(QSQLCipherResult);
    finalize();
    rInf.clear();
    skippedStatus = false;
    skipRow = false;
    q->setAt(QSql::BeforeFirstRow);
    q->setActive(false);
    q->cleanup();
}

void QSQLCipherResultPrivate::finalize()
{
    if (!stmt)
        return;

    sqlite3_finalize(stmt);
    stmt = 0;
}

void QSQLCipherResultPrivate::initColumns(bool emptyResultset)
{
    Q_Q(QSQLCipherResult);
    int nCols = sqlite3_column_count(stmt);
    if (nCols <= 0)
        return;

    q->init(nCols);

    for (int i = 0; i < nCols; ++i)
    {
        QString colName = QString(reinterpret_cast<const QChar *>(sqlite3_column_name16(stmt, i))).remove(u'"');
        // must use typeName for resolving the type to match QSQLCipherDriver::record
        QString typeName = QString(reinterpret_cast<const QChar *>(sqlite3_column_decltype16(stmt, i)));
        // sqlite3_column_type is documented to have undefined behavior if the result set is empty
        int stp = emptyResultset ? -1 : sqlite3_column_type(stmt, i);

        int fieldType;

        if (!typeName.isEmpty())
        {
            fieldType = qGetColumnType(typeName);
        }
        else
        {
            // Get the proper type for the field based on stp value
            switch (stp)
            {
                case SQLITE_INTEGER: fieldType = QMetaType::Int; break;
                case SQLITE_FLOAT: fieldType = QMetaType::Double; break;
                case SQLITE_BLOB: fieldType = QMetaType::QByteArray; break;
                case SQLITE_TEXT: fieldType = QMetaType::QString; break;
                case SQLITE_NULL:
                default: fieldType = QMetaType::UnknownType; break;
            }
        }
#ifndef DISABLE_COLUMN_METADATA
        const QString tableName = QString(reinterpret_cast<const QChar *>(sqlite3_column_table_name16(stmt, i))).remove(QLatin1Char('"'));
        QSqlField fld(colName, QMetaType(fieldType), tableName);
#else
        QSqlField fld(colName, QMetaType(fieldType));
#endif
        fld.setSqlType(stp);
        rInf.append(fld);
    }
}

bool QSQLCipherResultPrivate::fetchNext(QSqlCachedResult::ValueCache &values, int idx, bool initialFetch)
{
    Q_Q(QSQLCipherResult);

    if (skipRow)
    {
        // already fetched
        Q_ASSERT(!initialFetch);
        skipRow = false;
        for (int i = 0; i < firstRow.count(); i++)
            values[i] = firstRow[i];
        return skippedStatus;
    }
    skipRow = initialFetch;

    if (initialFetch)
    {
        firstRow.clear();
        firstRow.resize(sqlite3_column_count(stmt));
    }

    if (!stmt)
    {
        q->setLastError(QSqlError(QObject::tr("QSQLiteResult", "Unable to fetch row"), QObject::tr("QSQLiteResult", "No query"), QSqlError::ConnectionError));
        q->setAt(QSql::AfterLastRow);
        return false;
    }
    int res = sqlite3_step(stmt);
    switch (res)
    {
        case SQLITE_ROW:
            // check to see if should fill out columns
            if (rInf.isEmpty())
                // must be first call.
                initColumns(false);
            if (idx < 0 && !initialFetch)
                return true;
            for (int i = 0; i < rInf.count(); ++i)
            {
                switch (sqlite3_column_type(stmt, i))
                {
                    case SQLITE_BLOB: values[i + idx] = QByteArray(static_cast<const char *>(sqlite3_column_blob(stmt, i)), sqlite3_column_bytes(stmt, i)); break;
                    case SQLITE_INTEGER: values[i + idx] = sqlite3_column_int64(stmt, i); break;
                    case SQLITE_FLOAT:
                        switch (q->numericalPrecisionPolicy())
                        {
                            case QSql::LowPrecisionInt32: values[i + idx] = sqlite3_column_int(stmt, i); break;
                            case QSql::LowPrecisionInt64: values[i + idx] = sqlite3_column_int64(stmt, i); break;
                            case QSql::LowPrecisionDouble:
                            case QSql::HighPrecision:
                            default: values[i + idx] = sqlite3_column_double(stmt, i); break;
                        };
                        break;
                    case SQLITE_NULL: values[i + idx] = QVariant(QMetaType::fromType<QString>()); break;
                    default:
                        values[i + idx] = QString(reinterpret_cast<const QChar *>(sqlite3_column_text16(stmt, i)), sqlite3_column_bytes16(stmt, i) / sizeof(QChar));
                        break;
                }
            }
            return true;
        case SQLITE_DONE:
            if (rInf.isEmpty())
                // must be first call.
                initColumns(true);
            q->setAt(QSql::AfterLastRow);
            sqlite3_reset(stmt);
            return false;
        case SQLITE_CONSTRAINT:
        case SQLITE_ERROR:
            // SQLITE_ERROR is a generic error code and we must call sqlite3_reset()
            // to get the specific error message.
            res = sqlite3_reset(stmt);
            q->setLastError(qMakeError(drv_d_func()->access, QObject::tr("QSQLiteResult", "Unable to fetch row"), QSqlError::ConnectionError, res));
            q->setAt(QSql::AfterLastRow);
            return false;
        case SQLITE_MISUSE:
        case SQLITE_BUSY:
        default:
            // something wrong, don't get col info, but still return false
            q->setLastError(qMakeError(drv_d_func()->access, QObject::tr("QSQLiteResult", "Unable to fetch row"), QSqlError::ConnectionError, res));
            sqlite3_reset(stmt);
            q->setAt(QSql::AfterLastRow);
            return false;
    }
    return false;
}

QSQLCipherResult::QSQLCipherResult(const QSQLCipherDriver *db) : QSqlCachedResult(*new QSQLCipherResultPrivate(this, db))
{
    Q_D(QSQLCipherResult);
    const_cast<QSQLCipherDriverPrivate *>(d->drv_d_func())->results.append(this);
}

QSQLCipherResult::~QSQLCipherResult()
{
    Q_D(QSQLCipherResult);
    if (d->drv_d_func())
        const_cast<QSQLCipherDriverPrivate *>(d->drv_d_func())->results.removeOne(this);
    d->cleanup();
}

void QSQLCipherResult::virtual_hook(int id, void *data)
{
    QSqlCachedResult::virtual_hook(id, data);
}

bool QSQLCipherResult::reset(const QString &query)
{
    if (!prepare(query))
        return false;
    return exec();
}

bool QSQLCipherResult::prepare(const QString &query)
{
    Q_D(QSQLCipherResult);
    if (!driver() || !driver()->isOpen() || driver()->isOpenError())
        return false;

    d->cleanup();

    setSelect(false);

    const void *pzTail = nullptr;
    const auto size = int((query.size() + 1) * sizeof(QChar));

#if (SQLITE_VERSION_NUMBER >= 3003011)
    int res = sqlite3_prepare16_v2(d->drv_d_func()->access, query.constData(), size, &d->stmt, &pzTail);
#else
    int res = sqlite3_prepare16(d->access, query.constData(), size, &d->stmt, &pzTail);
#endif

    if (res != SQLITE_OK)
    {
        setLastError(qMakeError(d->drv_d_func()->access, QObject::tr("QSQLiteResult", "Unable to execute statement"), QSqlError::StatementError, res));
        d->finalize();
        return false;
    }
    else if (pzTail && !QString(reinterpret_cast<const QChar *>(pzTail)).trimmed().isEmpty())
    {
        setLastError(qMakeError(d->drv_d_func()->access, QObject::tr("QSQLiteResult", "Unable to execute multiple statements at a time"), QSqlError::StatementError,
                                SQLITE_MISUSE));
        d->finalize();
        return false;
    }
    return true;
}

bool QSQLCipherResult::execBatch(bool arrayBind)
{
    Q_UNUSED(arrayBind);
    Q_D(QSqlResult);
    QScopedValueRollback<QList<QVariant>> valuesScope(d->values);
    QList<QVariant> values = d->values;
    if (values.count() == 0)
        return false;

    for (int i = 0; i < values.at(0).toList().count(); ++i)
    {
        d->values.clear();
        QScopedValueRollback<QHash<QString, QList<int>>> indexesScope(d->indexes);
        auto it = d->indexes.constBegin();
        while (it != d->indexes.constEnd())
        {
            bindValue(it.key(), values.at(it.value().first()).toList().at(i), QSql::In);
            ++it;
        }
        if (!exec())
            return false;
    }
    return true;
}

bool QSQLCipherResult::exec()
{
    Q_D(QSQLCipherResult);
    QList<QVariant> values = boundValues();

    d->skippedStatus = false;
    d->skipRow = false;
    d->rInf.clear();
    clearValues();
    setLastError(QSqlError());

    int res = sqlite3_reset(d->stmt);
    if (res != SQLITE_OK)
    {
        setLastError(qMakeError(d->drv_d_func()->access, QObject::tr("QSQLiteResult", "Unable to reset statement"), QSqlError::StatementError, res));
        d->finalize();
        return false;
    }

    int paramCount = sqlite3_bind_parameter_count(d->stmt);
    bool paramCountIsValid = paramCount == values.count();

#if (SQLITE_VERSION_NUMBER >= 3003011)
    // In the case of the reuse of a named placeholder
    // We need to check explicitly that paramCount is greater than or equal to 1, as sqlite
    // can end up in a case where for virtual tables it returns 0 even though it
    // has parameters
    if (paramCount >= 1 && paramCount < values.count())
    {
        const auto countIndexes = [](int counter, const QList<int> &indexList) { return counter + indexList.length(); };

        const int bindParamCount = std::accumulate(d->indexes.cbegin(), d->indexes.cend(), 0, countIndexes);

        paramCountIsValid = bindParamCount == values.count();
        // When using named placeholders, it will reuse the index for duplicated
        // placeholders. So we need to ensure the QList has only one instance of
        // each value as SQLite will do the rest for us.
        QList<QVariant> prunedValues;
        QList<int> handledIndexes;
        for (int i = 0, currentIndex = 0; i < values.size(); ++i)
        {
            if (handledIndexes.contains(i))
                continue;
            const char *parameterName = sqlite3_bind_parameter_name(d->stmt, currentIndex + 1);
            if (!parameterName)
            {
                paramCountIsValid = false;
                continue;
            }
            const auto placeHolder = QString::fromUtf8(parameterName);
            const auto &indexes = d->indexes.value(placeHolder);
            handledIndexes << indexes;
            prunedValues << values.at(indexes.first());
            ++currentIndex;
        }
        values = prunedValues;
    }
#endif

    if (paramCountIsValid)
    {
        for (int i = 0; i < paramCount; ++i)
        {
            res = SQLITE_OK;
            const QVariant &value = values.at(i);

            if (value.isNull())
            {
                res = sqlite3_bind_null(d->stmt, i + 1);
            }
            else
            {
                switch (value.userType())
                {
                    case QMetaType::QByteArray:
                    {
                        const QByteArray *ba = static_cast<const QByteArray *>(value.constData());
                        res = sqlite3_bind_blob(d->stmt, i + 1, ba->constData(), ba->size(), SQLITE_STATIC);
                        break;
                    }
                    case QMetaType::Int:
                    case QMetaType::Bool: res = sqlite3_bind_int(d->stmt, i + 1, value.toInt()); break;
                    case QMetaType::Double: res = sqlite3_bind_double(d->stmt, i + 1, value.toDouble()); break;
                    case QMetaType::UInt:
                    case QMetaType::LongLong: res = sqlite3_bind_int64(d->stmt, i + 1, value.toLongLong()); break;
                    case QMetaType::QDateTime:
                    {
                        const QDateTime dateTime = value.toDateTime();
                        const QString str = dateTime.toString(Qt::ISODateWithMs);
                        res = sqlite3_bind_text16(d->stmt, i + 1, str.utf16(), int(str.size() * sizeof(ushort)), SQLITE_TRANSIENT);
                        break;
                    }
                    case QMetaType::QTime:
                    {
                        const QTime time = value.toTime();
                        const QString str = time.toString(u"hh:mm:ss.zzz");
                        res = sqlite3_bind_text16(d->stmt, i + 1, str.utf16(), int(str.size() * sizeof(ushort)), SQLITE_TRANSIENT);
                        break;
                    }
                    case QMetaType::QString:
                    {
                        // lifetime of string == lifetime of its qvariant
                        const QString *str = static_cast<const QString *>(value.constData());
                        res = sqlite3_bind_text16(d->stmt, i + 1, str->unicode(), int(str->size()) * sizeof(QChar), SQLITE_STATIC);
                        break;
                    }
                    default:
                    {
                        QString str = value.toString();
                        // SQLITE_TRANSIENT makes sure that sqlite buffers the data
                        res = sqlite3_bind_text16(d->stmt, i + 1, str.utf16(), int(str.size()) * sizeof(QChar), SQLITE_TRANSIENT);
                        break;
                    }
                }
            }
            if (res != SQLITE_OK)
            {
                setLastError(qMakeError(d->drv_d_func()->access, QObject::tr("QSQLiteResult", "Unable to bind parameters"), QSqlError::StatementError, res));
                d->finalize();
                return false;
            }
        }
    }
    else
    {
        setLastError(QSqlError(QObject::tr("QSQLiteResult", "Parameter count mismatch"), QString(), QSqlError::StatementError));
        return false;
    }
    d->skippedStatus = d->fetchNext(d->firstRow, 0, true);
    if (lastError().isValid())
    {
        setSelect(false);
        setActive(false);
        return false;
    }
    setSelect(!d->rInf.isEmpty());
    setActive(true);
    return true;
}

bool QSQLCipherResult::gotoNext(QSqlCachedResult::ValueCache &row, int idx)
{
    Q_D(QSQLCipherResult);
    return d->fetchNext(row, idx, false);
}

int QSQLCipherResult::size()
{
    return -1;
}

int QSQLCipherResult::numRowsAffected()
{
    Q_D(const QSQLCipherResult);
    return sqlite3_changes(d->drv_d_func()->access);
}

QVariant QSQLCipherResult::lastInsertId() const
{
    Q_D(const QSQLCipherResult);
    if (isActive())
    {
        qint64 id = sqlite3_last_insert_rowid(d->drv_d_func()->access);
        if (id)
            return id;
    }
    return QVariant();
}

QSqlRecord QSQLCipherResult::record() const
{
    Q_D(const QSQLCipherResult);
    if (!isActive() || !isSelect())
        return QSqlRecord();
    return d->rInf;
}

void QSQLCipherResult::detachFromResultSet()
{
    Q_D(QSQLCipherResult);
    if (d->stmt)
        sqlite3_reset(d->stmt);
}

QVariant QSQLCipherResult::handle() const
{
    Q_D(const QSQLCipherResult);
    return QVariant::fromValue(d->stmt);
}

/////////////////////////////////////////////////////////

#if QT_CONFIG(regularexpression)
static void _q_regexp(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (Q_UNLIKELY(argc != 2))
    {
        sqlite3_result_int(context, 0);
        return;
    }

    const QString pattern = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_value_text(argv[0])));
    const QString subject = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_value_text(argv[1])));

    auto cache = static_cast<QCache<QString, QRegularExpression> *>(sqlite3_user_data(context));
    auto regexp = cache->object(pattern);
    const bool wasCached = regexp;

    if (!wasCached)
        regexp = new QRegularExpression(pattern, QRegularExpression::DontCaptureOption);

    const bool found = subject.contains(*regexp);

    if (!wasCached)
        cache->insert(pattern, regexp);

    sqlite3_result_int(context, int(found));
}

static void _q_regexp_cleanup(void *cache)
{
    delete static_cast<QCache<QString, QRegularExpression> *>(cache);
}
#endif

QSQLCipherDriver::QSQLCipherDriver(QObject *parent) : QSqlDriver(*new QSQLCipherDriverPrivate, parent)
{
}

QSQLCipherDriver::QSQLCipherDriver(sqlite3 *connection, QObject *parent) : QSqlDriver(*new QSQLCipherDriverPrivate, parent)
{
    Q_D(QSQLCipherDriver);
    d->access = connection;
    setOpen(true);
    setOpenError(false);
}

QSQLCipherDriver::~QSQLCipherDriver()
{
    QSQLCipherDriver::close();
}

bool QSQLCipherDriver::hasFeature(DriverFeature f) const
{
    switch (f)
    {
        case BLOB:
        case Transactions:
        case Unicode:
        case LastInsertId:
        case PreparedQueries:
        case PositionalPlaceholders:
        case SimpleLocking:
        case FinishQuery:
        case LowPrecisionNumbers:
        case EventNotifications: return true;
        case QuerySize:
        case BatchOperations:
        case MultipleResultSets:
        case CancelQuery: return false;
        case NamedPlaceholders:
#if (SQLITE_VERSION_NUMBER < 3003011)
            return false;
#else
            return true;
#endif
    }
    return false;
}

/*
   SQLite dbs have no user name, passwords, hosts or ports.
   just file names.
*/
bool QSQLCipherDriver::open(const QString &db, const QString &, const QString &pass, const QString &, int, const QString &conOpts)
{
    Q_D(QSQLCipherDriver);
    if (isOpen())
        close();

    int timeOut = 5000;
    bool sharedCache = false;
    bool openReadOnlyOption = false;
    bool openUriOption = false;
    bool useExtendedResultCodes = true;
#if QT_CONFIG(regularexpression)
    static const QString regexpConnectOption = QStringLiteral("QSQLITE_ENABLE_REGEXP");
    bool defineRegexp = false;
    int regexpCacheSize = 25;
#endif

    const auto opts = QStringView{ conOpts }.split(u';');
    for (auto option : opts)
    {
        option = option.trimmed();
        if (option.startsWith(QStringLiteral("QSQLITE_BUSY_TIMEOUT")))
        {
            option = option.mid(20).trimmed();
            if (option.startsWith(u'='))
            {
                bool ok;
                const int nt = option.mid(1).trimmed().toInt(&ok);
                if (ok)
                    timeOut = nt;
            }
        }
        else if (option == QStringLiteral("QSQLITE_OPEN_READONLY"))
        {
            openReadOnlyOption = true;
        }
        else if (option == QStringLiteral("QSQLITE_OPEN_URI"))
        {
            openUriOption = true;
        }
        else if (option == QStringLiteral("QSQLITE_ENABLE_SHARED_CACHE"))
        {
            sharedCache = true;
        }
        else if (option == QStringLiteral("QSQLITE_NO_USE_EXTENDED_RESULT_CODES"))
        {
            useExtendedResultCodes = false;
        }
#if QT_CONFIG(regularexpression)
        else if (option.startsWith(regexpConnectOption))
        {
            option = option.mid(regexpConnectOption.size()).trimmed();
            if (option.isEmpty())
            {
                defineRegexp = true;
            }
            else if (option.startsWith(u'='))
            {
                bool ok = false;
                const int cacheSize = option.mid(1).trimmed().toInt(&ok);
                if (ok)
                {
                    defineRegexp = true;
                    if (cacheSize > 0)
                        regexpCacheSize = cacheSize;
                }
            }
        }
#endif
    }

    int openMode = (openReadOnlyOption ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
    openMode |= (sharedCache ? SQLITE_OPEN_SHAREDCACHE : SQLITE_OPEN_PRIVATECACHE);
    if (openUriOption)
        openMode |= SQLITE_OPEN_URI;

    openMode |= SQLITE_OPEN_NOMUTEX;

    const int res = sqlite3_open_v2(db.toUtf8().constData(), &d->access, openMode, nullptr);

    if (res == SQLITE_OK)
    {
        sqlite3_busy_timeout(d->access, timeOut);
        sqlite3_extended_result_codes(d->access, useExtendedResultCodes);
        sqlite3_key(d->access, pass.toUtf8().constData(), pass.length());
        if (sqlite3_exec(d->access, "SELECT count(*) FROM sqlite_master;", NULL, NULL, NULL) == SQLITE_OK)
        {
            setOpen(true);
            setOpenError(false);
#if QT_CONFIG(regularexpression)
            if (defineRegexp)
            {
                auto cache = new QCache<QString, QRegularExpression>(regexpCacheSize);
                sqlite3_create_function_v2(d->access, "regexp", 2, SQLITE_UTF8, cache, &_q_regexp, nullptr, nullptr, &_q_regexp_cleanup);
            }
#endif
            return true;
        }
        else
        {
            setLastError(qMakeError(d->access, tr("Incorrect Password"), QSqlError::ConnectionError, res));
            setOpenError(true);
            if (d->access)
            {
                sqlite3_close(d->access);
                d->access = 0;
            }
            return false;
        }
    }
    else
    {
        setLastError(qMakeError(d->access, tr("Error opening database"), QSqlError::ConnectionError, res));
        setOpenError(true);

        if (d->access)
        {
            sqlite3_close(d->access);
            d->access = 0;
        }

        return false;
    }
}

void QSQLCipherDriver::close()
{
    Q_D(QSQLCipherDriver);
    if (isOpen())
    {
        for (QSQLCipherResult *result : qAsConst(d->results))
            result->d_func()->finalize();

        if (d->access && (d->notificationid.count() > 0))
        {
            d->notificationid.clear();
            sqlite3_update_hook(d->access, nullptr, nullptr);
        }

        const int res = sqlite3_close(d->access);

        if (res != SQLITE_OK)
            setLastError(qMakeError(d->access, tr("Error closing database"), QSqlError::ConnectionError, res));
        d->access = 0;
        setOpen(false);
        setOpenError(false);
    }
}

QSqlResult *QSQLCipherDriver::createResult() const
{
    return new QSQLCipherResult(this);
}

bool QSQLCipherDriver::beginTransaction()
{
    if (!isOpen() || isOpenError())
        return false;

    QSqlQuery q(createResult());
    if (!q.exec(QStringLiteral("BEGIN")))
    {
        setLastError(QSqlError(tr("Unable to begin transaction"), q.lastError().databaseText(), QSqlError::TransactionError));
        return false;
    }

    return true;
}

bool QSQLCipherDriver::commitTransaction()
{
    if (!isOpen() || isOpenError())
        return false;

    QSqlQuery q(createResult());
    if (!q.exec(QStringLiteral("COMMIT")))
    {
        setLastError(QSqlError(tr("Unable to commit transaction"), q.lastError().databaseText(), QSqlError::TransactionError));
        return false;
    }

    return true;
}

bool QSQLCipherDriver::rollbackTransaction()
{
    if (!isOpen() || isOpenError())
        return false;

    QSqlQuery q(createResult());
    if (!q.exec(QStringLiteral("ROLLBACK")))
    {
        setLastError(QSqlError(tr("Unable to rollback transaction"), q.lastError().databaseText(), QSqlError::TransactionError));
        return false;
    }

    return true;
}

QStringList QSQLCipherDriver::tables(QSql::TableType type) const
{
    QStringList res;
    if (!isOpen())
        return res;

    QSqlQuery q(createResult());
    q.setForwardOnly(true);

    QString sql = QStringLiteral("SELECT name FROM sqlite_master WHERE %1 "
                                 "UNION ALL SELECT name FROM sqlite_temp_master WHERE %1");
    if ((type & QSql::Tables) && (type & QSql::Views))
        sql = sql.arg(QStringLiteral("type='table' OR type='view'"));
    else if (type & QSql::Tables)
        sql = sql.arg(QStringLiteral("type='table'"));
    else if (type & QSql::Views)
        sql = sql.arg(QStringLiteral("type='view'"));
    else
        sql.clear();

    if (!sql.isEmpty() && q.exec(sql))
    {
        while (q.next())
            res.append(q.value(0).toString());
    }

    if (type & QSql::SystemTables)
    {
        // there are no internal tables beside this one:
        res.append(QStringLiteral("sqlite_master"));
    }

    return res;
}

static QSqlIndex qGetTableInfo(QSqlQuery &q, const QString &tableName, bool onlyPIndex = false)
{
    QString schema;
    QString table(tableName);
    const int indexOfSeparator = tableName.indexOf(u'.');
    if (indexOfSeparator > -1)
    {
        const int indexOfCloseBracket = tableName.indexOf(u']');
        if (indexOfCloseBracket != tableName.size() - 1)
        {
            // Handles a case like databaseName.tableName
            schema = tableName.left(indexOfSeparator + 1);
            table = tableName.mid(indexOfSeparator + 1);
        }
        else
        {
            const int indexOfOpenBracket = tableName.lastIndexOf(u'[', indexOfCloseBracket);
            if (indexOfOpenBracket > 0)
            {
                // Handles a case like databaseName.[tableName]
                schema = tableName.left(indexOfOpenBracket);
                table = tableName.mid(indexOfOpenBracket);
            }
        }
    }
    q.exec(QStringLiteral("PRAGMA ") + schema + QStringLiteral("table_info (") + _q_escapeIdentifier(table, QSqlDriver::TableName) + u')');
    QSqlIndex ind;
    while (q.next())
    {
        bool isPk = q.value(5).toInt();
        if (onlyPIndex && !isPk)
            continue;
        QString typeName = q.value(2).toString().toLower();
        QString defVal = q.value(4).toString();
        if (!defVal.isEmpty() && defVal.at(0) == u'\'')
        {
            const int end = defVal.lastIndexOf(u'\'');
            if (end > 0)
                defVal = defVal.mid(1, end - 1);
        }

        QSqlField fld(q.value(1).toString(), QMetaType(qGetColumnType(typeName)), tableName);
        if (isPk && (typeName == QStringLiteral("integer")))
            // INTEGER PRIMARY KEY fields are auto-generated in sqlite
            // INT PRIMARY KEY is not the same as INTEGER PRIMARY KEY!
            fld.setAutoValue(true);
        fld.setRequired(q.value(3).toInt() != 0);
        fld.setDefaultValue(defVal);
        ind.append(fld);
    }
    return ind;
}

QSqlIndex QSQLCipherDriver::primaryIndex(const QString &tblname) const
{
    if (!isOpen())
        return QSqlIndex();

    QString table = tblname;
    if (isIdentifierEscaped(table, QSqlDriver::TableName))
        table = stripDelimiters(table, QSqlDriver::TableName);

    QSqlQuery q(createResult());
    q.setForwardOnly(true);
    return qGetTableInfo(q, table, true);
}

QSqlRecord QSQLCipherDriver::record(const QString &tbl) const
{
    if (!isOpen())
        return QSqlRecord();

    QString table = tbl;
    if (isIdentifierEscaped(table, QSqlDriver::TableName))
        table = stripDelimiters(table, QSqlDriver::TableName);

    QSqlQuery q(createResult());
    q.setForwardOnly(true);
    return qGetTableInfo(q, table);
}

QVariant QSQLCipherDriver::handle() const
{
    Q_D(const QSQLCipherDriver);
    return QVariant::fromValue(d->access);
}

QString QSQLCipherDriver::escapeIdentifier(const QString &identifier, IdentifierType type) const
{
    return _q_escapeIdentifier(identifier, type);
}

static void handle_sqlite_callback(void *qobj, int aoperation, char const *adbname, char const *atablename, sqlite3_int64 arowid)
{
    Q_UNUSED(aoperation);
    Q_UNUSED(adbname);
    QSQLCipherDriver *driver = static_cast<QSQLCipherDriver *>(qobj);
    if (driver)
    {
        QMetaObject::invokeMethod(driver, "handleNotification", Qt::QueuedConnection, Q_ARG(QString, QString::fromUtf8(atablename)), Q_ARG(qint64, arowid));
    }
}

bool QSQLCipherDriver::subscribeToNotification(const QString &name)
{
    Q_D(QSQLCipherDriver);
    if (!isOpen())
    {
        qWarning("Database not open.");
        return false;
    }

    if (d->notificationid.contains(name))
    {
        qWarning("Already subscribing to '%s'.", qPrintable(name));
        return false;
    }

    // sqlite supports only one notification callback, so only the first is registered
    d->notificationid << name;
    if (d->notificationid.count() == 1)
        sqlite3_update_hook(d->access, &handle_sqlite_callback, reinterpret_cast<void *>(this));

    return true;
}

bool QSQLCipherDriver::unsubscribeFromNotification(const QString &name)
{
    Q_D(QSQLCipherDriver);
    if (!isOpen())
    {
        qWarning("Database not open.");
        return false;
    }

    if (!d->notificationid.contains(name))
    {
        qWarning("Not subscribed to '%s'.", qPrintable(name));
        return false;
    }

    d->notificationid.removeAll(name);
    if (d->notificationid.isEmpty())
        sqlite3_update_hook(d->access, nullptr, nullptr);

    return true;
}

QStringList QSQLCipherDriver::subscribedToNotifications() const
{
    Q_D(const QSQLCipherDriver);
    return d->notificationid;
}

void QSQLCipherDriver::handleNotification(const QString &tableName, qint64 rowid)
{
    Q_D(const QSQLCipherDriver);
    if (d->notificationid.contains(tableName))
        emit notification(tableName, QSqlDriver::UnknownSource, QVariant(rowid));
}

/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Copyright (C) 2016 Intel Corporation.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
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

#include "qplatformdefs.h"
#include "private/qdatetime_p.h"
#if QT_CONFIG(datetimeparser)
#include "private/qdatetimeparser_p.h"
#endif

#include "qdatastream.h"
#include "qset.h"
#include "qlocale.h"
#include "qdatetime.h"
#if QT_CONFIG(timezone)
#include "qtimezoneprivate_p.h"
#endif
#include "qregexp.h"
#include "qdebug.h"
#ifndef Q_OS_WIN
#include <locale.h>
#endif

#include <cmath>
#ifdef Q_CC_MINGW
#  include <unistd.h> // Define _POSIX_THREAD_SAFE_FUNCTIONS to obtain localtime_r()
#endif
#include <time.h>
#ifdef Q_OS_WIN
#  include <qt_windows.h>
#  ifdef Q_OS_WINRT
#    include "qfunctions_winrt.h"
#  endif
#endif

#if defined(Q_OS_MAC)
#include <private/qcore_mac_p.h>
#endif

#include "qcalendar.h"
#include "qgregoriancalendar_p.h"

QT_BEGIN_NAMESPACE

/*****************************************************************************
  Date/Time Constants
 *****************************************************************************/

enum {
    SECS_PER_DAY = 86400,
    MSECS_PER_DAY = 86400000,
    SECS_PER_HOUR = 3600,
    MSECS_PER_HOUR = 3600000,
    SECS_PER_MIN = 60,
    MSECS_PER_MIN = 60000,
    TIME_T_MAX = 2145916799,  // int maximum 2037-12-31T23:59:59 UTC
    JULIAN_DAY_FOR_EPOCH = 2440588 // result of julianDayFromDate(1970, 1, 1)
};

/*****************************************************************************
  QDate static helper functions
 *****************************************************************************/

static inline QDate fixedDate(QCalendar::YearMonthDay &&parts, QCalendar cal)
{
    if ((parts.year < 0 && !cal.isProleptic()) || (parts.year == 0 && !cal.hasYearZero()))
        return QDate();

    parts.day = qMin(parts.day, cal.daysInMonth(parts.month, parts.year));
    return cal.dateFromParts(parts);
}

static inline QDate fixedDate(QCalendar::YearMonthDay &&parts)
{
    if (parts.year) {
        parts.day = qMin(parts.day, QGregorianCalendar::monthLength(parts.month, parts.year));
        qint64 jd;
        if (QGregorianCalendar::julianFromParts(parts.year, parts.month, parts.day, &jd))
            return QDate::fromJulianDay(jd);
    }
    return QDate();
}

/*****************************************************************************
  Date/Time formatting helper functions
 *****************************************************************************/

#if QT_CONFIG(textdate)
static const char qt_shortMonthNames[][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int qt_monthNumberFromShortName(QStringView shortName)
{
    for (unsigned int i = 0; i < sizeof(qt_shortMonthNames) / sizeof(qt_shortMonthNames[0]); ++i) {
        if (shortName == QLatin1String(qt_shortMonthNames[i], 3))
            return i + 1;
    }
    return -1;
}
static int qt_monthNumberFromShortName(const QString &shortName)
{ return qt_monthNumberFromShortName(QStringView(shortName)); }

static int fromShortMonthName(QStringView monthName, int year)
{
    // Assume that English monthnames are the default
    int month = qt_monthNumberFromShortName(monthName);
    if (month != -1)
        return month;
    // If English names can't be found, search the localized ones
    for (int i = 1; i <= 12; ++i) {
        if (monthName == QCalendar().monthName(QLocale::system(), i, year, QLocale::ShortFormat))
            return i;
    }
    return -1;
}
#endif // textdate

#if QT_CONFIG(datestring)
struct ParsedRfcDateTime {
    QDate date;
    QTime time;
    int utcOffset;
};

static ParsedRfcDateTime rfcDateImpl(const QString &s)
{
    ParsedRfcDateTime result;

    // Matches "[ddd,] dd MMM yyyy[ hh:mm[:ss]] [±hhmm]" - correct RFC 822, 2822, 5322 format
    QRegExp rex(QStringLiteral("^[ \\t]*(?:[A-Z][a-z]+,)?[ \\t]*(\\d{1,2})[ \\t]+([A-Z][a-z]+)[ \\t]+(\\d\\d\\d\\d)(?:[ \\t]+(\\d\\d):(\\d\\d)(?::(\\d\\d))?)?[ \\t]*(?:([+-])(\\d\\d)(\\d\\d))?"));
    if (s.indexOf(rex) == 0) {
        const QStringList cap = rex.capturedTexts();
        result.date = QDate(cap[3].toInt(), qt_monthNumberFromShortName(cap[2]), cap[1].toInt());
        if (!cap[4].isEmpty())
            result.time = QTime(cap[4].toInt(), cap[5].toInt(), cap[6].toInt());
        const bool positiveOffset = (cap[7] == QLatin1String("+"));
        const int hourOffset = cap[8].toInt();
        const int minOffset = cap[9].toInt();
        result.utcOffset = ((hourOffset * 60 + minOffset) * (positiveOffset ? 60 : -60));
    } else {
        // Matches "ddd MMM dd[ hh:mm:ss] yyyy [±hhmm]" - permissive RFC 850, 1036 (read only)
        QRegExp rex(QStringLiteral("^[ \\t]*[A-Z][a-z]+[ \\t]+([A-Z][a-z]+)[ \\t]+(\\d\\d)(?:[ \\t]+(\\d\\d):(\\d\\d):(\\d\\d))?[ \\t]+(\\d\\d\\d\\d)[ \\t]*(?:([+-])(\\d\\d)(\\d\\d))?"));
        if (s.indexOf(rex) == 0) {
            const QStringList cap = rex.capturedTexts();
            result.date = QDate(cap[6].toInt(), qt_monthNumberFromShortName(cap[1]), cap[2].toInt());
            if (!cap[3].isEmpty())
                result.time = QTime(cap[3].toInt(), cap[4].toInt(), cap[5].toInt());
            const bool positiveOffset = (cap[7] == QLatin1String("+"));
            const int hourOffset = cap[8].toInt();
            const int minOffset = cap[9].toInt();
            result.utcOffset = ((hourOffset * 60 + minOffset) * (positiveOffset ? 60 : -60));
        }
    }

    return result;
}
#endif // datestring

// Return offset in [+-]HH:mm format
static QString toOffsetString(Qt::DateFormat format, int offset)
{
    return QString::asprintf("%c%02d%s%02d",
                             offset >= 0 ? '+' : '-',
                             qAbs(offset) / SECS_PER_HOUR,
                             // Qt::ISODate puts : between the hours and minutes, but Qt:TextDate does not:
                             format == Qt::TextDate ? "" : ":",
                             (qAbs(offset) / 60) % 60);
}

#if QT_CONFIG(datestring)
// Parse offset in [+-]HH[[:]mm] format
static int fromOffsetString(QStringView offsetString, bool *valid) noexcept
{
    *valid = false;

    const int size = offsetString.size();
    if (size < 2 || size > 6)
        return 0;

    // sign will be +1 for a positive and -1 for a negative offset
    int sign;

    // First char must be + or -
    const QChar signChar = offsetString.at(0);
    if (signChar == QLatin1Char('+'))
        sign = 1;
    else if (signChar == QLatin1Char('-'))
        sign = -1;
    else
        return 0;

    // Split the hour and minute parts
    const QStringView time = offsetString.mid(1);
    qsizetype hhLen = time.indexOf(QLatin1Char(':'));
    qsizetype mmIndex;
    if (hhLen == -1)
        mmIndex = hhLen = 2; // [+-]HHmm or [+-]HH format
    else
        mmIndex = hhLen + 1;

    const QLocale C = QLocale::c();
    const QStringView hhRef = time.left(qMin(hhLen, time.size()));
    bool ok = false;
    const int hour = C.toInt(hhRef, &ok);
    if (!ok)
        return 0;

    const QStringView mmRef = time.mid(qMin(mmIndex, time.size()));
    const int minute = mmRef.isEmpty() ? 0 : C.toInt(mmRef, &ok);
    if (!ok || minute < 0 || minute > 59)
        return 0;

    *valid = true;
    return sign * ((hour * 60) + minute) * 60;
}
#endif // datestring

/*****************************************************************************
  QDate member functions
 *****************************************************************************/

/*!
    \since 4.5

    \enum QDate::MonthNameType

    This enum describes the types of the string representation used
    for the month name.

    \value DateFormat This type of name can be used for date-to-string formatting.
    \value StandaloneFormat This type is used when you need to enumerate months or weekdays.
           Usually standalone names are represented in singular forms with
           capitalized first letter.
*/

/*!
    \class QDate
    \inmodule QtCore
    \reentrant
    \brief The QDate class provides date functions.


    A QDate object represents a particular date. This can be expressed as a
    calendar date, i.e. year, month, and day numbers, in the proleptic Gregorian
    calendar.

    A QDate object is typically created by giving the year, month, and day
    numbers explicitly. Note that QDate interprets year numbers less than 100 as
    presented, i.e., as years 1 through 99, without adding any offset. The
    static function currentDate() creates a QDate object containing the date
    read from the system clock. An explicit date can also be set using
    setDate(). The fromString() function returns a QDate given a string and a
    date format which is used to interpret the date within the string.

    The year(), month(), and day() functions provide access to the year, month,
    and day numbers. Also, dayOfWeek() and dayOfYear() functions are
    provided. The same information is provided in textual format by
    toString(). The day and month numbers can be mapped to names using QLocale.

    QDate provides a full set of operators to compare two QDate
    objects where smaller means earlier, and larger means later.

    You can increment (or decrement) a date by a given number of days
    using addDays(). Similarly you can use addMonths() and addYears().
    The daysTo() function returns the number of days between two
    dates.

    The daysInMonth() and daysInYear() functions return how many days
    there are in this date's month and year, respectively. The
    isLeapYear() function indicates whether a date is in a leap year.

    \section1 Remarks

    \section2 No Year 0

    There is no year 0. Dates in that year are considered invalid. The year -1
    is the year "1 before Christ" or "1 before current era." The day before 1
    January 1 CE, QDate(1, 1, 1), is 31 December 1 BCE, QDate(-1, 12, 31).

    \section2 Range of Valid Dates

    Dates are stored internally as a Julian Day number, an integer count of
    every day in a contiguous range, with 24 November 4714 BCE in the Gregorian
    calendar being Julian Day 0 (1 January 4713 BCE in the Julian calendar).
    As well as being an efficient and accurate way of storing an absolute date,
    it is suitable for converting a date into other calendar systems such as
    Hebrew, Islamic or Chinese. The Julian Day number can be obtained using
    QDate::toJulianDay() and can be set using QDate::fromJulianDay().

    The range of dates able to be stored by QDate as a Julian Day number is
    for technical reasons limited to between -784350574879 and 784354017364,
    which means from before 2 billion BCE to after 2 billion CE.

    \sa QTime, QDateTime, QCalendar, QDateTime::YearRange, QDateEdit, QDateTimeEdit, QCalendarWidget
*/

/*!
    \fn QDate::QDate()

    Constructs a null date. Null dates are invalid.

    \sa isNull(), isValid()
*/

/*!
    Constructs a date with year \a y, month \a m and day \a d.

    The date is understood in terms of the Gregorian calendar. If the specified
    date is invalid, the date is not set and isValid() returns \c false.

    \warning Years 1 to 99 are interpreted as is. Year 0 is invalid.

    \sa isValid(), QCalendar::dateFromParts()
*/

QDate::QDate(int y, int m, int d)
{
    if (!QGregorianCalendar::julianFromParts(y, m, d, &jd))
        jd = nullJd();
}

QDate::QDate(int y, int m, int d, QCalendar cal)
{
    *this = cal.dateFromParts(y, m, d);
}

/*!
    \fn bool QDate::isNull() const

    Returns \c true if the date is null; otherwise returns \c false. A null
    date is invalid.

    \note The behavior of this function is equivalent to isValid().

    \sa isValid()
*/

/*!
    \fn bool QDate::isValid() const

    Returns \c true if this date is valid; otherwise returns \c false.

    \sa isNull(), QCalendar::isDateValid()
*/

/*!
    Returns the year of this date.

    Uses \a cal as calendar, if supplied, else the Gregorian calendar.

    Returns 0 if the date is invalid. For some calendars, dates before their
    first year may all be invalid.

    If using a calendar which has a year 0, check using isValid() if the return
    is 0. Such calendars use negative year numbers in the obvious way, with
    year 1 preceded by year 0, in turn preceded by year -1 and so on.

    Some calendars, despite having no year 0, have a conventional numbering of
    the years before their first year, counting backwards from 1. For example,
    in the proleptic Gregorian calendar, successive years before 1 CE (the first
    year) are identified as 1 BCE, 2 BCE, 3 BCE and so on. For such calendars,
    negative year numbers are used to indicate these years before year 1, with
    -1 indicating the year before 1.

    \sa month(), day(), QCalendar::hasYearZero(), QCalendar::isProleptic()
*/

int QDate::year(QCalendar cal) const
{
    if (isValid()) {
        const auto parts = cal.partsFromDate(*this);
        if (parts.isValid())
            return parts.year;
    }
    return 0;
}

/*!
  \overload
 */

int QDate::year() const
{
    if (isValid()) {
        const auto parts = QGregorianCalendar::partsFromJulian(jd);
        if (parts.isValid())
            return parts.year;
    }
    return 0;
}

/*!
    Returns the month-number for the date.

    Numbers the months of the year starting with 1 for the first. Uses \a cal
    as calendar if supplied, else the Gregorian calendar, for which the month
    numbering is as follows:

    \list
    \li 1 = "January"
    \li 2 = "February"
    \li 3 = "March"
    \li 4 = "April"
    \li 5 = "May"
    \li 6 = "June"
    \li 7 = "July"
    \li 8 = "August"
    \li 9 = "September"
    \li 10 = "October"
    \li 11 = "November"
    \li 12 = "December"
    \endlist

    Returns 0 if the date is invalid. Note that some calendars may have more
    than 12 months in some years.

    \sa year(), day()
*/

int QDate::month(QCalendar cal) const
{
    if (isValid()) {
        const auto parts = cal.partsFromDate(*this);
        if (parts.isValid())
            return parts.month;
    }
    return 0;
}

/*!
  \overload
 */

int QDate::month() const
{
    if (isValid()) {
        const auto parts = QGregorianCalendar::partsFromJulian(jd);
        if (parts.isValid())
            return parts.month;
    }
    return 0;
}

/*!
    Returns the day of the month for this date.

    Uses \a cal as calendar if supplied, else the Gregorian calendar (for which
    the return ranges from 1 to 31). Returns 0 if the date is invalid.

    \sa year(), month(), dayOfWeek()
*/

int QDate::day(QCalendar cal) const
{
    if (isValid()) {
        const auto parts = cal.partsFromDate(*this);
        if (parts.isValid())
            return parts.day;
    }
    return 0;
}

/*!
  \overload
 */

int QDate::day() const
{
    if (isValid()) {
        const auto parts = QGregorianCalendar::partsFromJulian(jd);
        if (parts.isValid())
            return parts.day;
    }
    return 0;
}

/*!
    Returns the weekday (1 = Monday to 7 = Sunday) for this date.

    Uses \a cal as calendar if supplied, else the Gregorian calendar. Returns 0
    if the date is invalid. Some calendars may give special meaning
    (e.g. intercallary days) to values greater than 7.

    \sa day(), dayOfYear(), Qt::DayOfWeek
*/

int QDate::dayOfWeek(QCalendar cal) const
{
    if (isNull())
        return 0;

    return cal.dayOfWeek(*this);
}

/*!
  \overload
 */

int QDate::dayOfWeek() const
{
    return isValid() ? QGregorianCalendar::weekDayOfJulian(jd) : 0;
}

/*!
    Returns the day of the year (1 for the first day) for this date.

    Uses \a cal as calendar if supplied, else the Gregorian calendar.
    Returns 0 if either the date or the first day of its year is invalid.

    \sa day(), dayOfWeek()
*/

int QDate::dayOfYear(QCalendar cal) const
{
    if (isValid()) {
        QDate firstDay = cal.dateFromParts(year(cal), 1, 1);
        if (firstDay.isValid())
            return firstDay.daysTo(*this) + 1;
    }
    return 0;
}

/*!
  \overload
 */

int QDate::dayOfYear() const
{
    if (isValid()) {
        qint64 first;
        if (QGregorianCalendar::julianFromParts(year(), 1, 1, &first))
            return jd - first + 1;
    }
    return 0;
}

/*!
    Returns the number of days in the month for this date.

    Uses \a cal as calendar if supplied, else the Gregorian calendar (for which
    the result ranges from 28 to 31). Returns 0 if the date is invalid.

    \sa day(), daysInYear()
*/

int QDate::daysInMonth(QCalendar cal) const
{
    if (isValid()) {
        const auto parts = cal.partsFromDate(*this);
        if (parts.isValid())
            return cal.daysInMonth(parts.month, parts.year);
    }
    return 0;
}

/*!
  \overload
 */

int QDate::daysInMonth() const
{
    if (isValid()) {
        const auto parts = QGregorianCalendar::partsFromJulian(jd);
        if (parts.isValid())
            return QGregorianCalendar::monthLength(parts.month, parts.year);
    }
    return 0;
}

/*!
    Returns the number of days in the year for this date.

    Uses \a cal as calendar if supplied, else the Gregorian calendar (for which
    the result is 365 or 366). Returns 0 if the date is invalid.

    \sa day(), daysInMonth()
*/

int QDate::daysInYear(QCalendar cal) const
{
    if (isNull())
        return 0;

    return cal.daysInYear(year(cal));
}

/*!
  \overload
 */

int QDate::daysInYear() const
{
    return isValid() ? QGregorianCalendar::leapTest(year()) ? 366 : 365 : 0;
}

/*!
    Returns the ISO 8601 week number (1 to 53).

    Returns 0 if the date is invalid. Otherwise, returns the week number for the
    date. If \a yearNumber is not \nullptr (its default), stores the year as
    *\a{yearNumber}.

    In accordance with ISO 8601, each week falls in the year to which most of
    its days belong, in the Gregorian calendar. As ISO 8601's week starts on
    Monday, this is the year in which the week's Thursday falls. Most years have
    52 weeks, but some have 53.

    \note *\a{yearNumber} is not always the same as year(). For example, 1
    January 2000 has week number 52 in the year 1999, and 31 December
    2002 has week number 1 in the year 2003.

    \sa isValid()
*/

int QDate::weekNumber(int *yearNumber) const
{
    if (!isValid())
        return 0;

    // This could be replaced by use of QIso8601Calendar, once we implement it.
    // The Thursday of the same week determines our answer:
    QDate thursday(addDays(4 - dayOfWeek()));
    int year = thursday.year();
    // Week n's Thurs's DOY has 1 <= DOY - 7*(n-1) < 8, so 0 <= DOY + 6 - 7*n < 7:
    int week = (thursday.dayOfYear() + 6) / 7;

    if (yearNumber)
        *yearNumber = year;
    return week;
}

static bool inDateTimeRange(qint64 jd, bool start)
{
    using Bounds = std::numeric_limits<qint64>;
    if (jd < Bounds::min() + JULIAN_DAY_FOR_EPOCH)
        return false;
    jd -= JULIAN_DAY_FOR_EPOCH;
    const qint64 maxDay = Bounds::max() / MSECS_PER_DAY;
    const qint64 minDay = Bounds::min() / MSECS_PER_DAY - 1;
    // (Divisions rounded towards zero, as MSECS_PER_DAY has factors other than two.)
    // Range includes start of last day and end of first:
    if (start)
        return jd > minDay && jd <= maxDay;
    return jd >= minDay && jd < maxDay;
}

static QDateTime toEarliest(QDate day, const QDateTime &form)
{
    const Qt::TimeSpec spec = form.timeSpec();
    const int offset = (spec == Qt::OffsetFromUTC) ? form.offsetFromUtc() : 0;
#if QT_CONFIG(timezone)
    QTimeZone zone;
    if (spec == Qt::TimeZone)
        zone = form.timeZone();
#endif
    auto moment = [=](QTime time) {
        switch (spec) {
        case Qt::OffsetFromUTC: return QDateTime(day, time, spec, offset);
#if QT_CONFIG(timezone)
        case Qt::TimeZone: return QDateTime(day, time, zone);
#endif
        default: return QDateTime(day, time, spec);
        }
    };
    // Longest routine time-zone transition is 2 hours:
    QDateTime when = moment(QTime(2, 0));
    if (!when.isValid()) {
        // Noon should be safe ...
        when = moment(QTime(12, 0));
        if (!when.isValid()) {
            // ... unless it's a 24-hour jump (moving the date-line)
            when = moment(QTime(23, 59, 59, 999));
            if (!when.isValid())
                return QDateTime();
        }
    }
    int high = when.time().msecsSinceStartOfDay() / 60000;
    int low = 0;
    // Binary chop to the right minute
    while (high > low + 1) {
        int mid = (high + low) / 2;
        QDateTime probe = moment(QTime(mid / 60, mid % 60));
        if (probe.isValid() && probe.date() == day) {
            high = mid;
            when = probe;
        } else {
            low = mid;
        }
    }
    return when;
}

/*!
    \since 5.14
    \fn QDateTime QDate::startOfDay(Qt::TimeSpec spec, int offsetSeconds) const
    \fn QDateTime QDate::startOfDay(const QTimeZone &zone) const

    Returns the start-moment of the day.  Usually, this shall be midnight at the
    start of the day: however, if a time-zone transition causes the given date
    to skip over that midnight (e.g. a DST spring-forward skipping from the end
    of the previous day to 01:00 of the new day), the actual earliest time in
    the day is returned.  This can only arise when the start-moment is specified
    in terms of a time-zone (by passing its QTimeZone as \a zone) or in terms of
    local time (by passing Qt::LocalTime as \a spec; this is its default).

    The \a offsetSeconds is ignored unless \a spec is Qt::OffsetFromUTC, when it
    gives the implied zone's offset from UTC.  As UTC and such zones have no
    transitions, the start of the day is QTime(0, 0) in these cases.

    In the rare case of a date that was entirely skipped (this happens when a
    zone east of the international date-line switches to being west of it), the
    return shall be invalid.  Passing Qt::TimeZone as \a spec (instead of
    passing a QTimeZone) or passing an invalid time-zone as \a zone will also
    produce an invalid result, as shall dates that start outside the range
    representable by QDateTime.

    \sa endOfDay()
*/
QDateTime QDate::startOfDay(Qt::TimeSpec spec, int offsetSeconds) const
{
    if (!inDateTimeRange(jd, true))
        return QDateTime();

    switch (spec) {
    case Qt::TimeZone: // should pass a QTimeZone instead of Qt::TimeZone
        qWarning() << "Called QDate::startOfDay(Qt::TimeZone) on" << *this;
        return QDateTime();
    case Qt::OffsetFromUTC:
    case Qt::UTC:
        return QDateTime(*this, QTime(0, 0), spec, offsetSeconds);

    case Qt::LocalTime:
        if (offsetSeconds)
            qWarning("Ignoring offset (%d seconds) passed with Qt::LocalTime", offsetSeconds);
        break;
    }
    QDateTime when(*this, QTime(0, 0), spec);
    if (!when.isValid())
        when = toEarliest(*this, when);

    return when.isValid() ? when : QDateTime();
}

#if QT_CONFIG(timezone)
/*!
  \overload
  \since 5.14
*/
QDateTime QDate::startOfDay(const QTimeZone &zone) const
{
    if (!inDateTimeRange(jd, true) || !zone.isValid())
        return QDateTime();

    QDateTime when(*this, QTime(0, 0), zone);
    if (when.isValid())
        return when;

    // The start of the day must have fallen in a spring-forward's gap; find the spring-forward:
    if (zone.hasTransitions()) {
        QTimeZone::OffsetData tran = zone.previousTransition(QDateTime(*this, QTime(23, 59, 59, 999), zone));
        const QDateTime &at = tran.atUtc.toTimeZone(zone);
        if (at.isValid() && at.date() == *this)
            return at;
    }

    when = toEarliest(*this, when);
    return when.isValid() ? when : QDateTime();
}
#endif // timezone

static QDateTime toLatest(QDate day, const QDateTime &form)
{
    const Qt::TimeSpec spec = form.timeSpec();
    const int offset = (spec == Qt::OffsetFromUTC) ? form.offsetFromUtc() : 0;
#if QT_CONFIG(timezone)
    QTimeZone zone;
    if (spec == Qt::TimeZone)
        zone = form.timeZone();
#endif
    auto moment = [=](QTime time) {
        switch (spec) {
        case Qt::OffsetFromUTC: return QDateTime(day, time, spec, offset);
#if QT_CONFIG(timezone)
        case Qt::TimeZone: return QDateTime(day, time, zone);
#endif
        default: return QDateTime(day, time, spec);
        }
    };
    // Longest routine time-zone transition is 2 hours:
    QDateTime when = moment(QTime(21, 59, 59, 999));
    if (!when.isValid()) {
        // Noon should be safe ...
        when = moment(QTime(12, 0));
        if (!when.isValid()) {
            // ... unless it's a 24-hour jump (moving the date-line)
            when = moment(QTime(0, 0));
            if (!when.isValid())
                return QDateTime();
        }
    }
    int high = 24 * 60;
    int low = when.time().msecsSinceStartOfDay() / 60000;
    // Binary chop to the right minute
    while (high > low + 1) {
        int mid = (high + low) / 2;
        QDateTime probe = moment(QTime(mid / 60, mid % 60, 59, 999));
        if (probe.isValid() && probe.date() == day) {
            low = mid;
            when = probe;
        } else {
            high = mid;
        }
    }
    return when;
}

/*!
    \since 5.14
    \fn QDateTime QDate::endOfDay(Qt::TimeSpec spec, int offsetSeconds) const
    \fn QDateTime QDate::endOfDay(const QTimeZone &zone) const

    Returns the end-moment of the day.  Usually, this is one millisecond before
    the midnight at the end of the day: however, if a time-zone transition
    causes the given date to skip over that midnight (e.g. a DST spring-forward
    skipping from just before 23:00 to the start of the next day), the actual
    latest time in the day is returned.  This can only arise when the
    start-moment is specified in terms of a time-zone (by passing its QTimeZone
    as \a zone) or in terms of local time (by passing Qt::LocalTime as \a spec;
    this is its default).

    The \a offsetSeconds is ignored unless \a spec is Qt::OffsetFromUTC, when it
    gives the implied zone's offset from UTC.  As UTC and such zones have no
    transitions, the end of the day is QTime(23, 59, 59, 999) in these cases.

    In the rare case of a date that was entirely skipped (this happens when a
    zone east of the international date-line switches to being west of it), the
    return shall be invalid.  Passing Qt::TimeZone as \a spec (instead of
    passing a QTimeZone) will also produce an invalid result, as shall dates
    that end outside the range representable by QDateTime.

    \sa startOfDay()
*/
QDateTime QDate::endOfDay(Qt::TimeSpec spec, int offsetSeconds) const
{
    if (!inDateTimeRange(jd, false))
        return QDateTime();

    switch (spec) {
    case Qt::TimeZone: // should pass a QTimeZone instead of Qt::TimeZone
        qWarning() << "Called QDate::endOfDay(Qt::TimeZone) on" << *this;
        return QDateTime();
    case Qt::UTC:
    case Qt::OffsetFromUTC:
        return QDateTime(*this, QTime(23, 59, 59, 999), spec, offsetSeconds);

    case Qt::LocalTime:
        if (offsetSeconds)
            qWarning("Ignoring offset (%d seconds) passed with Qt::LocalTime", offsetSeconds);
        break;
    }
    QDateTime when(*this, QTime(23, 59, 59, 999), spec);
    if (!when.isValid())
        when = toLatest(*this, when);
    return when.isValid() ? when : QDateTime();
}

#if QT_CONFIG(timezone)
/*!
  \overload
  \since 5.14
*/
QDateTime QDate::endOfDay(const QTimeZone &zone) const
{
    if (!inDateTimeRange(jd, false) || !zone.isValid())
        return QDateTime();

    QDateTime when(*this, QTime(23, 59, 59, 999), zone);
    if (when.isValid())
        return when;

    // The end of the day must have fallen in a spring-forward's gap; find the spring-forward:
    if (zone.hasTransitions()) {
        QTimeZone::OffsetData tran = zone.nextTransition(QDateTime(*this, QTime(0, 0), zone));
        const QDateTime &at = tran.atUtc.toTimeZone(zone);
        if (at.isValid() && at.date() == *this)
            return at;
    }

    when = toLatest(*this, when);
    return when.isValid() ? when : QDateTime();
}
#endif // timezone

#if QT_DEPRECATED_SINCE(5, 11) && QT_CONFIG(textdate)

/*!
    \since 4.5
    \deprecated

    Returns the short name of the \a month for the representation specified
    by \a type.

    The months are enumerated using the following convention:

    \list
    \li 1 = "Jan"
    \li 2 = "Feb"
    \li 3 = "Mar"
    \li 4 = "Apr"
    \li 5 = "May"
    \li 6 = "Jun"
    \li 7 = "Jul"
    \li 8 = "Aug"
    \li 9 = "Sep"
    \li 10 = "Oct"
    \li 11 = "Nov"
    \li 12 = "Dec"
    \endlist

    The month names will be localized according to the system's
    locale settings, i.e. using QLocale::system().

    Returns an empty string if the date is invalid.

    \sa toString(), longMonthName(), shortDayName(), longDayName()
*/

QString QDate::shortMonthName(int month, QDate::MonthNameType type)
{
    switch (type) {
    case QDate::DateFormat:
        return QCalendar().monthName(QLocale::system(), month,
                                     QCalendar::Unspecified, QLocale::ShortFormat);
    case QDate::StandaloneFormat:
        return QCalendar().standaloneMonthName(QLocale::system(), month,
                                               QCalendar::Unspecified, QLocale::ShortFormat);
    }
    return QString();
}

/*!
    \since 4.5
    \deprecated

    Returns the long name of the \a month for the representation specified
    by \a type.

    The months are enumerated using the following convention:

    \list
    \li 1 = "January"
    \li 2 = "February"
    \li 3 = "March"
    \li 4 = "April"
    \li 5 = "May"
    \li 6 = "June"
    \li 7 = "July"
    \li 8 = "August"
    \li 9 = "September"
    \li 10 = "October"
    \li 11 = "November"
    \li 12 = "December"
    \endlist

    The month names will be localized according to the system's
    locale settings, i.e. using QLocale::system().

    Returns an empty string if the date is invalid.

    \sa toString(), shortMonthName(), shortDayName(), longDayName()
*/

QString QDate::longMonthName(int month, MonthNameType type)
{
    switch (type) {
    case QDate::DateFormat:
        return QCalendar().monthName(QLocale::system(), month,
                                     QCalendar::Unspecified, QLocale::LongFormat);
    case QDate::StandaloneFormat:
        return QCalendar().standaloneMonthName(QLocale::system(), month,
                                               QCalendar::Unspecified, QLocale::LongFormat);
    }
    return QString();
}

/*!
    \since 4.5
    \deprecated

    Returns the short name of the \a weekday for the representation specified
    by \a type.

    The days are enumerated using the following convention:

    \list
    \li 1 = "Mon"
    \li 2 = "Tue"
    \li 3 = "Wed"
    \li 4 = "Thu"
    \li 5 = "Fri"
    \li 6 = "Sat"
    \li 7 = "Sun"
    \endlist

    The day names will be localized according to the system's
    locale settings, i.e. using QLocale::system().

    Returns an empty string if the date is invalid.

    \sa toString(), shortMonthName(), longMonthName(), longDayName()
*/

QString QDate::shortDayName(int weekday, MonthNameType type)
{
    switch (type) {
    case QDate::DateFormat:
        return QLocale::system().dayName(weekday, QLocale::ShortFormat);
    case QDate::StandaloneFormat:
        return QLocale::system().standaloneDayName(weekday, QLocale::ShortFormat);
    }
    return QString();
}

/*!
    \since 4.5
    \deprecated

    Returns the long name of the \a weekday for the representation specified
    by \a type.

    The days are enumerated using the following convention:

    \list
    \li 1 = "Monday"
    \li 2 = "Tuesday"
    \li 3 = "Wednesday"
    \li 4 = "Thursday"
    \li 5 = "Friday"
    \li 6 = "Saturday"
    \li 7 = "Sunday"
    \endlist

    The day names will be localized according to the system's
    locale settings, i.e. using QLocale::system().

    Returns an empty string if the date is invalid.

    \sa toString(), shortDayName(), shortMonthName(), longMonthName()
*/

QString QDate::longDayName(int weekday, MonthNameType type)
{
    switch (type) {
    case QDate::DateFormat:
        return QLocale::system().dayName(weekday, QLocale::LongFormat);
    case QDate::StandaloneFormat:
        return QLocale::system().standaloneDayName(weekday, QLocale::LongFormat);
    }
    return QString();
}
#endif // textdate && deprecated

#if QT_CONFIG(datestring) // depends on, so implies, textdate

static QString toStringTextDate(QDate date, QCalendar cal)
{
    if (date.isValid()) {
        const auto parts = cal.partsFromDate(date);
        if (parts.isValid()) {
            const QLatin1Char sp(' ');
            return QLocale::system().dayName(cal.dayOfWeek(date), QLocale::ShortFormat) + sp
                + cal.monthName(QLocale::system(), parts.month, parts.year, QLocale::ShortFormat)
                + sp + QString::number(parts.day) + sp + QString::number(parts.year);
        }
    }
    return QString();
}

static QString toStringTextDate(QDate date)
{
    return toStringTextDate(date, QCalendar());
}

static QString toStringIsoDate(QDate date)
{
    const auto parts = QCalendar().partsFromDate(date);
    if (parts.isValid() && parts.year >= 0 && parts.year <= 9999)
        return QString::asprintf("%04d-%02d-%02d", parts.year, parts.month, parts.day);
    return QString();
}

/*!
    \fn QString QDate::toString(Qt::DateFormat format) const

    \overload

    Returns the date as a string. The \a format parameter determines
    the format of the string.

    If the \a format is Qt::TextDate, the string is formatted in the default
    way. The day and month names will be localized names using the system
    locale, i.e. QLocale::system(). An example of this formatting
    is "Sat May 20 1995".

    If the \a format is Qt::ISODate, the string format corresponds
    to the ISO 8601 extended specification for representations of
    dates and times, taking the form yyyy-MM-dd, where yyyy is the
    year, MM is the month of the year (between 01 and 12), and dd is
    the day of the month between 01 and 31.

    If the \a format is Qt::SystemLocaleShortDate or
    Qt::SystemLocaleLongDate, the string format depends on the locale
    settings of the system. Identical to calling
    QLocale::system().toString(date, QLocale::ShortFormat) or
    QLocale::system().toString(date, QLocale::LongFormat).

    If the \a format is Qt::DefaultLocaleShortDate or
    Qt::DefaultLocaleLongDate, the string format depends on the
    default application locale. This is the locale set with
    QLocale::setDefault(), or the system locale if no default locale
    has been set. Identical to calling
    \l {QLocale::toString()}{QLocale().toString(date, QLocale::ShortFormat) } or
    \l {QLocale::toString()}{QLocale().toString(date, QLocale::LongFormat)}.

    If the \a format is Qt::RFC2822Date, the string is formatted in
    an \l{RFC 2822} compatible way. An example of this formatting is
    "20 May 1995".

    If the date is invalid, an empty string will be returned.

    \warning The Qt::ISODate format is only valid for years in the
    range 0 to 9999. This restriction may apply to locale-aware
    formats as well, depending on the locale settings.

    \sa fromString(), QLocale::toString()
*/
QString QDate::toString(Qt::DateFormat format) const
{
    if (!isValid())
        return QString();

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toString(*this, QLocale::ShortFormat);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toString(*this, QLocale::LongFormat);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toString(*this, QLocale::ShortFormat);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toString(*this, QLocale::LongFormat);
    case Qt::RFC2822Date:
        return QLocale::c().toString(*this, u"dd MMM yyyy");
    default:
    case Qt::TextDate:
        return toStringTextDate(*this);
    case Qt::ISODate:
    case Qt::ISODateWithMs:
        return toStringIsoDate(*this);
    }
}

/*!
    \fn QString QDate::toString(const QString &format) const
    \fn QString QDate::toString(QStringView format) const

    Returns the date as a string. The \a format parameter determines
    the format of the result string.

    These expressions may be used:

    \table
    \header \li Expression \li Output
    \row \li d \li The day as a number without a leading zero (1 to 31)
    \row \li dd \li The day as a number with a leading zero (01 to 31)
    \row \li ddd
         \li The abbreviated localized day name (e.g. 'Mon' to 'Sun').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li dddd
         \li The long localized day name (e.g. 'Monday' to 'Sunday').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li M \li The month as a number without a leading zero (1 to 12)
    \row \li MM \li The month as a number with a leading zero (01 to 12)
    \row \li MMM
         \li The abbreviated localized month name (e.g. 'Jan' to 'Dec').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li MMMM
         \li The long localized month name (e.g. 'January' to 'December').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li yy \li The year as a two digit number (00 to 99)
    \row \li yyyy \li The year as a four digit number. If the year is negative,
            a minus sign is prepended, making five characters.
    \endtable

    Any sequence of characters enclosed in single quotes will be included
    verbatim in the output string (stripped of the quotes), even if it contains
    formatting characters. Two consecutive single quotes ("''") are replaced by
    a single quote in the output. All other characters in the format string are
    included verbatim in the output string.

    Formats without separators (e.g. "ddMM") are supported but must be used with
    care, as the resulting strings aren't always reliably readable (e.g. if "dM"
    produces "212" it could mean either the 2nd of December or the 21st of
    February).

    Example format strings (assuming that the QDate is the 20 July
    1969):

    \table
    \header \li Format            \li Result
    \row    \li dd.MM.yyyy        \li 20.07.1969
    \row    \li ddd MMMM d yy     \li Sun July 20 69
    \row    \li 'The day is' dddd \li The day is Sunday
    \endtable

    If the datetime is invalid, an empty string will be returned.

    \sa fromString(), QDateTime::toString(), QTime::toString(), QLocale::toString()

*/
QString QDate::toString(QStringView format) const
{
    return QLocale::system().toString(*this, format); // QLocale::c() ### Qt6
}

#if QT_STRINGVIEW_LEVEL < 2
QString QDate::toString(const QString &format) const
{
    return toString(qToStringViewIgnoringNull(format));
}
#endif

QString QDate::toString(Qt::DateFormat format, QCalendar cal) const
{
    if (!isValid())
        return QString();

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toString(*this, QLocale::ShortFormat, cal);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toString(*this, QLocale::LongFormat, cal);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toString(*this, QLocale::ShortFormat, cal);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toString(*this, QLocale::LongFormat, cal);
    case Qt::RFC2822Date:
        return QLocale::c().toString(*this, QStringView(u"dd MMM yyyy"), cal);
    default:
    case Qt::TextDate:
        return toStringTextDate(*this, cal);
    case Qt::ISODate:
    case Qt::ISODateWithMs:
        return toStringIsoDate(*this);
    }
}

QString QDate::toString(QStringView format, QCalendar cal) const
{
    return QLocale::system().toString(*this, format, cal); // QLocale::c() ### Qt6
}

#if QT_STRINGVIEW_LEVEL < 2
QString QDate::toString(const QString &format, QCalendar cal) const
{
    return toString(qToStringViewIgnoringNull(format), cal);
}
#endif

#endif // datestring

/*!
    \fn bool QDate::setYMD(int y, int m, int d)

    \deprecated in 5.0, use setDate() instead.

    Sets the date's year \a y, month \a m, and day \a d.

    If \a y is in the range 0 to 99, it is interpreted as 1900 to
    1999.
    Returns \c false if the date is invalid.

    Use setDate() instead.
*/

/*!
    \since 4.2

    Sets this to represent the date, in the Gregorian calendar, with the given
    \a year, \a month and \a day numbers. Returns true if the resulting date is
    valid, otherwise it sets this to represent an invalid date and returns
    false.

    \sa isValid(), QCalendar::dateFromParts()
*/
bool QDate::setDate(int year, int month, int day)
{
    if (QGregorianCalendar::julianFromParts(year, month, day, &jd))
        return true;

    jd = nullJd();
    return false;
}

/*!
    \since 5.14

    Sets this to represent the date, in the given calendar \a cal, with the
    given \a year, \a month and \a day numbers. Returns true if the resulting
    date is valid, otherwise it sets this to represent an invalid date and
    returns false.

    \sa isValid(), QCalendar::dateFromParts()
*/

bool QDate::setDate(int year, int month, int day, QCalendar cal)
{
    *this = QDate(year, month, day, cal);
    return isValid();
}

/*!
    \since 4.5

    Extracts the date's year, month, and day, and assigns them to
    *\a year, *\a month, and *\a day. The pointers may be null.

    Returns 0 if the date is invalid.

    \note In Qt versions prior to 5.7, this function is marked as non-\c{const}.

    \sa year(), month(), day(), isValid(), QCalendar::partsFromDate()
*/
void QDate::getDate(int *year, int *month, int *day) const
{
    QCalendar::YearMonthDay parts; // invalid by default
    if (isValid())
        parts = QGregorianCalendar::partsFromJulian(jd);

    const bool ok = parts.isValid();
    if (year)
        *year = ok ? parts.year : 0;
    if (month)
        *month = ok ? parts.month : 0;
    if (day)
        *day = ok ? parts.day : 0;
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
/*!
    \overload
    \internal
*/
void QDate::getDate(int *year, int *month, int *day)
{
    qAsConst(*this).getDate(year, month, day);
}
#endif // < Qt 6

/*!
    Returns a QDate object containing a date \a ndays later than the
    date of this object (or earlier if \a ndays is negative).

    Returns a null date if the current date is invalid or the new date is
    out of range.

    \sa addMonths(), addYears(), daysTo()
*/

QDate QDate::addDays(qint64 ndays) const
{
    if (isNull())
        return QDate();

    // Due to limits on minJd() and maxJd() we know that any overflow
    // will be invalid and caught by fromJulianDay().
    return fromJulianDay(jd + ndays);
}

/*!
    Returns a QDate object containing a date \a nmonths later than the
    date of this object (or earlier if \a nmonths is negative).

    Uses \a cal as calendar, if supplied, else the Gregorian calendar.

    \note If the ending day/month combination does not exist in the resulting
    month/year, this function will return a date that is the latest valid date
    in the selected month.

    \sa addDays(), addYears()
*/

QDate QDate::addMonths(int nmonths, QCalendar cal) const
{
    if (!isValid())
        return QDate();

    if (nmonths == 0)
        return *this;

    auto parts = cal.partsFromDate(*this);

    if (!parts.isValid())
        return QDate();
    Q_ASSERT(parts.year || cal.hasYearZero());

    parts.month += nmonths;
    while (parts.month <= 0) {
        if (--parts.year || cal.hasYearZero())
            parts.month += cal.monthsInYear(parts.year);
    }
    int count = cal.monthsInYear(parts.year);
    while (parts.month > count) {
        parts.month -= count;
        count = (++parts.year || cal.hasYearZero()) ? cal.monthsInYear(parts.year) : 0;
    }

    return fixedDate(std::move(parts), cal);
}

/*!
  \overload
*/

QDate QDate::addMonths(int nmonths) const
{
    if (isNull())
        return QDate();

    if (nmonths == 0)
        return *this;

    auto parts = QGregorianCalendar::partsFromJulian(jd);

    if (!parts.isValid())
        return QDate();
    Q_ASSERT(parts.year);

    parts.month += nmonths;
    while (parts.month <= 0) {
        if (--parts.year) // skip over year 0
            parts.month += 12;
    }
    while (parts.month > 12) {
        parts.month -= 12;
        if (!++parts.year) // skip over year 0
            ++parts.year;
    }

    return fixedDate(std::move(parts));
}

/*!
    Returns a QDate object containing a date \a nyears later than the
    date of this object (or earlier if \a nyears is negative).

    Uses \a cal as calendar, if supplied, else the Gregorian calendar.

    \note If the ending day/month combination does not exist in the resulting
    year (e.g., for the Gregorian calendar, if the date was Feb 29 and the final
    year is not a leap year), this function will return a date that is the
    latest valid date in the given month (in the example, Feb 28).

    \sa addDays(), addMonths()
*/

QDate QDate::addYears(int nyears, QCalendar cal) const
{
    if (!isValid())
        return QDate();

    auto parts = cal.partsFromDate(*this);
    if (!parts.isValid())
        return QDate();

    int old_y = parts.year;
    parts.year += nyears;

    // If we just crossed (or hit) a missing year zero, adjust year by +/- 1:
    if (!cal.hasYearZero() && ((old_y > 0) != (parts.year > 0) || !parts.year))
        parts.year += nyears > 0 ? +1 : -1;

    return fixedDate(std::move(parts), cal);
}

/*!
    \overload
*/

QDate QDate::addYears(int nyears) const
{
    if (isNull())
        return QDate();

    auto parts = QGregorianCalendar::partsFromJulian(jd);
    if (!parts.isValid())
        return QDate();

    int old_y = parts.year;
    parts.year += nyears;

    // If we just crossed (or hit) a missing year zero, adjust year by +/- 1:
    if ((old_y > 0) != (parts.year > 0) || !parts.year)
        parts.year += nyears > 0 ? +1 : -1;

    return fixedDate(std::move(parts));
}

/*!
    Returns the number of days from this date to \a d (which is
    negative if \a d is earlier than this date).

    Returns 0 if either date is invalid.

    Example:
    \snippet code/src_corelib_tools_qdatetime.cpp 0

    \sa addDays()
*/

qint64 QDate::daysTo(const QDate &d) const
{
    if (isNull() || d.isNull())
        return 0;

    // Due to limits on minJd() and maxJd() we know this will never overflow
    return d.jd - jd;
}


/*!
    \fn bool QDate::operator==(const QDate &d) const

    Returns \c true if this date is equal to \a d; otherwise returns
    false.

*/

/*!
    \fn bool QDate::operator!=(const QDate &d) const

    Returns \c true if this date is different from \a d; otherwise
    returns \c false.
*/

/*!
    \fn bool QDate::operator<(const QDate &d) const

    Returns \c true if this date is earlier than \a d; otherwise returns
    false.
*/

/*!
    \fn bool QDate::operator<=(const QDate &d) const

    Returns \c true if this date is earlier than or equal to \a d;
    otherwise returns \c false.
*/

/*!
    \fn bool QDate::operator>(const QDate &d) const

    Returns \c true if this date is later than \a d; otherwise returns
    false.
*/

/*!
    \fn bool QDate::operator>=(const QDate &d) const

    Returns \c true if this date is later than or equal to \a d;
    otherwise returns \c false.
*/

/*!
    \fn QDate::currentDate()
    Returns the current date, as reported by the system clock.

    \sa QTime::currentTime(), QDateTime::currentDateTime()
*/

#if QT_CONFIG(datestring) // depends on, so implies, textdate
namespace {

struct ParsedInt { int value = 0; bool ok = false; };

/*
    /internal

    Read an int that must be the whole text.  QStringRef::toInt() will ignore
    spaces happily; but ISO date format should not.
*/
ParsedInt readInt(QStringView text)
{
    ParsedInt result;
    for (const auto &ch : text) {
        if (ch.isSpace())
            return result;
    }
    result.value = QLocale::c().toInt(text, &result.ok);
    return result;
}

}

/*!
    Returns the QDate represented by the \a string, using the
    \a format given, or an invalid date if the string cannot be
    parsed.

    Note for Qt::TextDate: It is recommended that you use the
    English short month names (e.g. "Jan"). Although localized month
    names can also be used, they depend on the user's locale settings.

    \sa toString(), QLocale::toDate()
*/

QDate QDate::fromString(const QString &string, Qt::DateFormat format)
{
    if (string.isEmpty())
        return QDate();

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toDate(string, QLocale::ShortFormat);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toDate(string, QLocale::LongFormat);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toDate(string, QLocale::ShortFormat);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toDate(string, QLocale::LongFormat);
    case Qt::RFC2822Date:
        return rfcDateImpl(string).date;
    default:
    case Qt::TextDate: {
        QVector<QStringRef> parts = string.splitRef(QLatin1Char(' '), QString::SkipEmptyParts);

        if (parts.count() != 4)
            return QDate();

        bool ok = false;
        int year = parts.at(3).toInt(&ok);
        int day = ok ? parts.at(2).toInt(&ok) : 0;
        if (!ok || !day)
            return QDate();

        const int month = fromShortMonthName(parts.at(1), year);
        if (month == -1) // Month name matches no English or localised name.
            return QDate();

        return QDate(year, month, day);
        }
    case Qt::ISODate:
        // Semi-strict parsing, must be long enough and have punctuators as separators
        if (string.size() >= 10 && string.at(4).isPunct() && string.at(7).isPunct()
                && (string.size() == 10 || !string.at(10).isDigit())) {
            QStringView view(string);
            const ParsedInt year = readInt(view.mid(0, 4));
            const ParsedInt month = readInt(view.mid(5, 2));
            const ParsedInt day = readInt(view.mid(8, 2));
            if (year.ok && year.value > 0 && year.value <= 9999 && month.ok && day.ok)
                return QDate(year.value, month.value, day.value);
        }
        break;
    }
    return QDate();
}

/*!
    Returns the QDate represented by the \a string, using the \a
    format given, or an invalid date if the string cannot be parsed.

    Uses \a cal as calendar if supplied, else the Gregorian calendar. Ranges of
    values in the format descriptions below are for the latter; they may be
    different for other calendars.

    These expressions may be used for the format:

    \table
    \header \li Expression \li Output
    \row \li d \li The day as a number without a leading zero (1 to 31)
    \row \li dd \li The day as a number with a leading zero (01 to 31)
    \row \li ddd
         \li The abbreviated localized day name (e.g. 'Mon' to 'Sun').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li dddd
         \li The long localized day name (e.g. 'Monday' to 'Sunday').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li M \li The month as a number without a leading zero (1 to 12)
    \row \li MM \li The month as a number with a leading zero (01 to 12)
    \row \li MMM
         \li The abbreviated localized month name (e.g. 'Jan' to 'Dec').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li MMMM
         \li The long localized month name (e.g. 'January' to 'December').
             Uses the system locale to localize the name, i.e. QLocale::system().
    \row \li yy \li The year as a two digit number (00 to 99)
    \row \li yyyy \li The year as a four digit number, possibly plus a leading
             minus sign for negative years.
    \endtable

    \note Unlike the other version of this function, day and month names must
    be given in the user's local language. It is only possible to use the English
    names if the user's language is English.

    All other input characters will be treated as text. Any sequence
    of characters that are enclosed in single quotes will also be
    treated as text and will not be used as an expression. For example:

    \snippet code/src_corelib_tools_qdatetime.cpp 1

    If the format is not satisfied, an invalid QDate is returned. The
    expressions that don't expect leading zeroes (d, M) will be
    greedy. This means that they will use two digits even if this
    will put them outside the accepted range of values and leaves too
    few digits for other sections. For example, the following format
    string could have meant January 30 but the M will grab two
    digits, resulting in an invalid date:

    \snippet code/src_corelib_tools_qdatetime.cpp 2

    For any field that is not represented in the format the following
    defaults are used:

    \table
    \header \li Field  \li Default value
    \row    \li Year   \li 1900
    \row    \li Month  \li 1
    \row    \li Day    \li 1
    \endtable

    The following examples demonstrate the default values:

    \snippet code/src_corelib_tools_qdatetime.cpp 3

    \sa toString(), QDateTime::fromString(), QTime::fromString(),
        QLocale::toDate()
*/

QDate QDate::fromString(const QString &string, const QString &format, QCalendar cal)
{
    QDate date;
#if QT_CONFIG(datetimeparser)
    QDateTimeParser dt(QVariant::Date, QDateTimeParser::FromString, cal);
    // dt.setDefaultLocale(QLocale::c()); ### Qt 6
    if (dt.parseFormat(format))
        dt.fromString(string, &date, 0);
#else
    Q_UNUSED(string);
    Q_UNUSED(format);
    Q_UNUSED(cal);
#endif
    return date;
}

/*!
  \overload
*/

QDate QDate::fromString(const QString &string, const QString &format)
{
    return fromString(string, format, QCalendar());
}
#endif // datestring

/*!
    \overload

    Returns \c true if the specified date (\a year, \a month, and \a day) is
    valid in the Gregorian calendar; otherwise returns \c false.

    Example:
    \snippet code/src_corelib_tools_qdatetime.cpp 4

    \sa isNull(), setDate(), QCalendar::isDateValid()
*/

bool QDate::isValid(int year, int month, int day)
{
    return QGregorianCalendar::validParts(year, month, day);
}

/*!
    \fn bool QDate::isLeapYear(int year)

    Returns \c true if the specified \a year is a leap year in the Gregorian
    calendar; otherwise returns \c false.

    \sa QCalendar::isLeapYear()
*/

bool QDate::isLeapYear(int y)
{
    return QGregorianCalendar::leapTest(y);
}

/*! \fn static QDate QDate::fromJulianDay(qint64 jd)

    Converts the Julian day \a jd to a QDate.

    \sa toJulianDay()
*/

/*! \fn int QDate::toJulianDay() const

    Converts the date to a Julian day.

    \sa fromJulianDay()
*/

/*****************************************************************************
  QTime member functions
 *****************************************************************************/

/*!
    \class QTime
    \inmodule QtCore
    \reentrant

    \brief The QTime class provides clock time functions.


    A QTime object contains a clock time, which it can express as the numbers of
    hours, minutes, seconds, and milliseconds since midnight. It provides
    functions for comparing times and for manipulating a time by adding a number
    of milliseconds.

    QTime uses the 24-hour clock format; it has no concept of AM/PM.
    Unlike QDateTime, QTime knows nothing about time zones or
    daylight-saving time (DST).

    A QTime object is typically created either by giving the number of hours,
    minutes, seconds, and milliseconds explicitly, or by using the static
    function currentTime(), which creates a QTime object that represents the
    system's local time.

    The hour(), minute(), second(), and msec() functions provide
    access to the number of hours, minutes, seconds, and milliseconds
    of the time. The same information is provided in textual format by
    the toString() function.

    The addSecs() and addMSecs() functions provide the time a given
    number of seconds or milliseconds later than a given time.
    Correspondingly, the number of seconds or milliseconds
    between two times can be found using secsTo() or msecsTo().

    QTime provides a full set of operators to compare two QTime
    objects; an earlier time is considered smaller than a later one;
    if A.msecsTo(B) is positive, then A < B.

    \sa QDate, QDateTime
*/

/*!
    \fn QTime::QTime()

    Constructs a null time object. For a null time, isNull() returns \c true and
    isValid() returns \c false. If you need a zero time, use QTime(0, 0).  For
    the start of a day, see QDate::startOfDay().

    \sa isNull(), isValid()
*/

/*!
    Constructs a time with hour \a h, minute \a m, seconds \a s and
    milliseconds \a ms.

    \a h must be in the range 0 to 23, \a m and \a s must be in the
    range 0 to 59, and \a ms must be in the range 0 to 999.

    \sa isValid()
*/

QTime::QTime(int h, int m, int s, int ms)
{
    setHMS(h, m, s, ms);
}


/*!
    \fn bool QTime::isNull() const

    Returns \c true if the time is null (i.e., the QTime object was
    constructed using the default constructor); otherwise returns
    false. A null time is also an invalid time.

    \sa isValid()
*/

/*!
    Returns \c true if the time is valid; otherwise returns \c false. For example,
    the time 23:30:55.746 is valid, but 24:12:30 is invalid.

    \sa isNull()
*/

bool QTime::isValid() const
{
    return mds > NullTime && mds < MSECS_PER_DAY;
}


/*!
    Returns the hour part (0 to 23) of the time.

    Returns -1 if the time is invalid.

    \sa minute(), second(), msec()
*/

int QTime::hour() const
{
    if (!isValid())
        return -1;

    return ds() / MSECS_PER_HOUR;
}

/*!
    Returns the minute part (0 to 59) of the time.

    Returns -1 if the time is invalid.

    \sa hour(), second(), msec()
*/

int QTime::minute() const
{
    if (!isValid())
        return -1;

    return (ds() % MSECS_PER_HOUR) / MSECS_PER_MIN;
}

/*!
    Returns the second part (0 to 59) of the time.

    Returns -1 if the time is invalid.

    \sa hour(), minute(), msec()
*/

int QTime::second() const
{
    if (!isValid())
        return -1;

    return (ds() / 1000)%SECS_PER_MIN;
}

/*!
    Returns the millisecond part (0 to 999) of the time.

    Returns -1 if the time is invalid.

    \sa hour(), minute(), second()
*/

int QTime::msec() const
{
    if (!isValid())
        return -1;

    return ds() % 1000;
}

#if QT_CONFIG(datestring) // depends on, so implies, textdate
/*!
    \overload

    Returns the time as a string. The \a format parameter determines
    the format of the string.

    If \a format is Qt::TextDate, the string format is HH:mm:ss;
    e.g. 1 second before midnight would be "23:59:59".

    If \a format is Qt::ISODate, the string format corresponds to the
    ISO 8601 extended specification for representations of dates,
    represented by HH:mm:ss. To include milliseconds in the ISO 8601
    date, use the \a format Qt::ISODateWithMs, which corresponds to
    HH:mm:ss.zzz.

    If the \a format is Qt::SystemLocaleShortDate or
    Qt::SystemLocaleLongDate, the string format depends on the locale
    settings of the system. Identical to calling
    QLocale::system().toString(time, QLocale::ShortFormat) or
    QLocale::system().toString(time, QLocale::LongFormat).

    If the \a format is Qt::DefaultLocaleShortDate or
    Qt::DefaultLocaleLongDate, the string format depends on the
    default application locale. This is the locale set with
    QLocale::setDefault(), or the system locale if no default locale
    has been set. Identical to calling

    \l {QLocale::toString()}{QLocale().toString(time, QLocale::ShortFormat)} or
    \l {QLocale::toString()}{QLocale().toString(time, QLocale::LongFormat)}.

    If the \a format is Qt::RFC2822Date, the string is formatted in
    an \l{RFC 2822} compatible way. An example of this formatting is
    "23:59:20".

    If the time is invalid, an empty string will be returned.

    \sa fromString(), QDate::toString(), QDateTime::toString(), QLocale::toString()
*/

QString QTime::toString(Qt::DateFormat format) const
{
    if (!isValid())
        return QString();

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toString(*this, QLocale::ShortFormat);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toString(*this, QLocale::LongFormat);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toString(*this, QLocale::ShortFormat);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toString(*this, QLocale::LongFormat);
    case Qt::ISODateWithMs:
        return QString::asprintf("%02d:%02d:%02d.%03d", hour(), minute(), second(), msec());
    case Qt::RFC2822Date:
    case Qt::ISODate:
    case Qt::TextDate:
    default:
        return QString::asprintf("%02d:%02d:%02d", hour(), minute(), second());
    }
}

/*!
    \fn QString QTime::toString(const QString &format) const
    \fn QString QTime::toString(QStringView format) const

    Returns the time as a string. The \a format parameter determines
    the format of the result string.

    These expressions may be used:

    \table
    \header \li Expression \li Output
    \row \li h
         \li The hour without a leading zero (0 to 23 or 1 to 12 if AM/PM display)
    \row \li hh
         \li The hour with a leading zero (00 to 23 or 01 to 12 if AM/PM display)
    \row \li H
         \li The hour without a leading zero (0 to 23, even with AM/PM display)
    \row \li HH
         \li The hour with a leading zero (00 to 23, even with AM/PM display)
    \row \li m \li The minute without a leading zero (0 to 59)
    \row \li mm \li The minute with a leading zero (00 to 59)
    \row \li s \li The whole second, without any leading zero (0 to 59)
    \row \li ss \li The whole second, with a leading zero where applicable (00 to 59)
    \row \li z \li The fractional part of the second, to go after a decimal
                point, without trailing zeroes (0 to 999).  Thus "\c{s.z}"
                reports the seconds to full available (millisecond) precision
                without trailing zeroes.
    \row \li zzz \li The fractional part of the second, to millisecond
                precision, including trailing zeroes where applicable (000 to 999).
    \row \li AP or A
         \li Use AM/PM display. \e A/AP will be replaced by an upper-case
             version of either QLocale::amText() or QLocale::pmText().
    \row \li ap or a
         \li Use am/pm display. \e a/ap will be replaced by a lower-case version
             of either QLocale::amText() or QLocale::pmText().
    \row \li t \li The timezone (for example "CEST")
    \endtable

    Any sequence of characters enclosed in single quotes will be included
    verbatim in the output string (stripped of the quotes), even if it contains
    formatting characters. Two consecutive single quotes ("''") are replaced by
    a single quote in the output. All other characters in the format string are
    included verbatim in the output string.

    Formats without separators (e.g. "ddMM") are supported but must be used with
    care, as the resulting strings aren't always reliably readable (e.g. if "dM"
    produces "212" it could mean either the 2nd of December or the 21st of
    February).

    Example format strings (assuming that the QTime is 14:13:09.042 and the system
    locale is \c{en_US})

    \table
    \header \li Format \li Result
    \row \li hh:mm:ss.zzz \li 14:13:09.042
    \row \li h:m:s ap     \li 2:13:9 pm
    \row \li H:m:s a      \li 14:13:9 pm
    \endtable

    If the time is invalid, an empty string will be returned.
    If \a format is empty, the default format "hh:mm:ss" is used.

    \sa fromString(), QDate::toString(), QDateTime::toString(), QLocale::toString()
*/
QString QTime::toString(QStringView format) const
{
    return QLocale::system().toString(*this, format); // QLocale::c() ### Qt6
}

#if QT_STRINGVIEW_VERSION < 2
QString QTime::toString(const QString &format) const
{
    return toString(qToStringViewIgnoringNull(format));
}
#endif

#endif // datestring

/*!
    Sets the time to hour \a h, minute \a m, seconds \a s and
    milliseconds \a ms.

    \a h must be in the range 0 to 23, \a m and \a s must be in the
    range 0 to 59, and \a ms must be in the range 0 to 999.
    Returns \c true if the set time is valid; otherwise returns \c false.

    \sa isValid()
*/

bool QTime::setHMS(int h, int m, int s, int ms)
{
    if (!isValid(h,m,s,ms)) {
        mds = NullTime;                // make this invalid
        return false;
    }
    mds = (h*SECS_PER_HOUR + m*SECS_PER_MIN + s)*1000 + ms;
    return true;
}

/*!
    Returns a QTime object containing a time \a s seconds later
    than the time of this object (or earlier if \a s is negative).

    Note that the time will wrap if it passes midnight.

    Returns a null time if this time is invalid.

    Example:

    \snippet code/src_corelib_tools_qdatetime.cpp 5

    \sa addMSecs(), secsTo(), QDateTime::addSecs()
*/

QTime QTime::addSecs(int s) const
{
    s %= SECS_PER_DAY;
    return addMSecs(s * 1000);
}

/*!
    Returns the number of seconds from this time to \a t.
    If \a t is earlier than this time, the number of seconds returned
    is negative.

    Because QTime measures time within a day and there are 86400
    seconds in a day, the result is always between -86400 and 86400.

    secsTo() does not take into account any milliseconds.

    Returns 0 if either time is invalid.

    \sa addSecs(), QDateTime::secsTo()
*/

int QTime::secsTo(const QTime &t) const
{
    if (!isValid() || !t.isValid())
        return 0;

    // Truncate milliseconds as we do not want to consider them.
    int ourSeconds = ds() / 1000;
    int theirSeconds = t.ds() / 1000;
    return theirSeconds - ourSeconds;
}

/*!
    Returns a QTime object containing a time \a ms milliseconds later
    than the time of this object (or earlier if \a ms is negative).

    Note that the time will wrap if it passes midnight. See addSecs()
    for an example.

    Returns a null time if this time is invalid.

    \sa addSecs(), msecsTo(), QDateTime::addMSecs()
*/

QTime QTime::addMSecs(int ms) const
{
    QTime t;
    if (isValid()) {
        if (ms < 0) {
            // %,/ not well-defined for -ve, so always work with +ve.
            int negdays = (MSECS_PER_DAY - ms) / MSECS_PER_DAY;
            t.mds = (ds() + ms + negdays * MSECS_PER_DAY) % MSECS_PER_DAY;
        } else {
            t.mds = (ds() + ms) % MSECS_PER_DAY;
        }
    }
    return t;
}

/*!
    Returns the number of milliseconds from this time to \a t.
    If \a t is earlier than this time, the number of milliseconds returned
    is negative.

    Because QTime measures time within a day and there are 86400
    seconds in a day, the result is always between -86400000 and
    86400000 ms.

    Returns 0 if either time is invalid.

    \sa secsTo(), addMSecs(), QDateTime::msecsTo()
*/

int QTime::msecsTo(const QTime &t) const
{
    if (!isValid() || !t.isValid())
        return 0;
    return t.ds() - ds();
}


/*!
    \fn bool QTime::operator==(const QTime &t) const

    Returns \c true if this time is equal to \a t; otherwise returns \c false.
*/

/*!
    \fn bool QTime::operator!=(const QTime &t) const

    Returns \c true if this time is different from \a t; otherwise returns \c false.
*/

/*!
    \fn bool QTime::operator<(const QTime &t) const

    Returns \c true if this time is earlier than \a t; otherwise returns \c false.
*/

/*!
    \fn bool QTime::operator<=(const QTime &t) const

    Returns \c true if this time is earlier than or equal to \a t;
    otherwise returns \c false.
*/

/*!
    \fn bool QTime::operator>(const QTime &t) const

    Returns \c true if this time is later than \a t; otherwise returns \c false.
*/

/*!
    \fn bool QTime::operator>=(const QTime &t) const

    Returns \c true if this time is later than or equal to \a t;
    otherwise returns \c false.
*/

/*!
    \fn QTime QTime::fromMSecsSinceStartOfDay(int msecs)

    Returns a new QTime instance with the time set to the number of \a msecs
    since the start of the day, i.e. since 00:00:00.

    If \a msecs falls outside the valid range an invalid QTime will be returned.

    \sa msecsSinceStartOfDay()
*/

/*!
    \fn int QTime::msecsSinceStartOfDay() const

    Returns the number of msecs since the start of the day, i.e. since 00:00:00.

    \sa fromMSecsSinceStartOfDay()
*/

/*!
    \fn QTime::currentTime()

    Returns the current time as reported by the system clock.

    Note that the accuracy depends on the accuracy of the underlying
    operating system; not all systems provide 1-millisecond accuracy.

    Furthermore, currentTime() only increases within each day; it shall drop by
    24 hours each time midnight passes; and, beside this, changes in it may not
    correspond to elapsed time, if a daylight-saving transition intervenes.

    \sa QDateTime::currentDateTime(), QDateTime::currentDateTimeUtc()
*/

#if QT_CONFIG(datestring) // depends on, so implies, textdate

static QTime fromIsoTimeString(QStringView string, Qt::DateFormat format, bool *isMidnight24)
{
    if (isMidnight24)
        *isMidnight24 = false;

    const int size = string.size();
    if (size < 5 || string.at(2) != QLatin1Char(':'))
        return QTime();

    ParsedInt hour = readInt(string.mid(0, 2));
    ParsedInt minute = readInt(string.mid(3, 2));
    if (!hour.ok || !minute.ok)
        return QTime();
    // FIXME: ISO 8601 allows [,.]\d+ after hour, just as it does after minute

    int second = 0;
    int msec = 0;

    if (size == 5) {
        // HH:mm format
        second = 0;
        msec = 0;
    } else if (string.at(5) == QLatin1Char(',') || string.at(5) == QLatin1Char('.')) {
        if (format == Qt::TextDate)
            return QTime();
        // ISODate HH:mm.ssssss format
        // We only want 5 digits worth of fraction of minute. This follows the existing
        // behavior that determines how milliseconds are read; 4 millisecond digits are
        // read and then rounded to 3. If we read at most 5 digits for fraction of minute,
        // the maximum amount of millisecond digits it will expand to once converted to
        // seconds is 4. E.g. 12:34,99999 will expand to 12:34:59.9994. The milliseconds
        // will then be rounded up AND clamped to 999.

        const QStringView minuteFractionStr = string.mid(6, qMin(qsizetype(5), string.size() - 6));
        const ParsedInt parsed = readInt(minuteFractionStr);
        if (!parsed.ok)
            return QTime();
        const float secondWithMs
            = double(parsed.value) * 60 / (std::pow(double(10), minuteFractionStr.size()));

        second = std::floor(secondWithMs);
        const float secondFraction = secondWithMs - second;
        msec = qMin(qRound(secondFraction * 1000.0), 999);
    } else if (string.at(5) == QLatin1Char(':')) {
        // HH:mm:ss or HH:mm:ss.zzz
        const ParsedInt parsed = readInt(string.mid(6, qMin(qsizetype(2), string.size() - 6)));
        if (!parsed.ok)
            return QTime();
        second = parsed.value;
        if (size <= 8) {
            // No fractional part to read
        } else if (string.at(8) == QLatin1Char(',') || string.at(8) == QLatin1Char('.')) {
            QStringView msecStr(string.mid(9, qMin(qsizetype(4), string.size() - 9)));
            bool ok = true;
            // Can't use readInt() here, as we *do* allow trailing space - but not leading:
            if (!msecStr.isEmpty() && !msecStr.at(0).isDigit())
                return QTime();
            msecStr = msecStr.trimmed();
            int msecInt = msecStr.isEmpty() ? 0 : QLocale::c().toInt(msecStr, &ok);
            if (!ok)
                return QTime();
            const double secondFraction(msecInt / (std::pow(double(10), msecStr.size())));
            msec = qMin(qRound(secondFraction * 1000.0), 999);
        } else {
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0) // behavior change
            // Stray cruft after date-time: tolerate trailing space, but nothing else.
            for (const auto &ch : string.mid(8)) {
                if (!ch.isSpace())
                    return QTime();
            }
#endif
        }
    } else {
        return QTime();
    }

    const bool isISODate = format == Qt::ISODate || format == Qt::ISODateWithMs;
    if (isISODate && hour.value == 24 && minute.value == 0 && second == 0 && msec == 0) {
        if (isMidnight24)
            *isMidnight24 = true;
        hour.value = 0;
    }

    return QTime(hour.value, minute.value, second, msec);
}

/*!
    Returns the time represented in the \a string as a QTime using the
    \a format given, or an invalid time if this is not possible.

    Note that fromString() uses a "C" locale encoded string to convert
    milliseconds to a float value. If the default locale is not "C",
    this may result in two conversion attempts (if the conversion
    fails for the default locale). This should be considered an
    implementation detail.

    \sa toString(), QLocale::toTime()
*/
QTime QTime::fromString(const QString &string, Qt::DateFormat format)
{
    if (string.isEmpty())
        return QTime();

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toTime(string, QLocale::ShortFormat);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toTime(string, QLocale::LongFormat);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toTime(string, QLocale::ShortFormat);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toTime(string, QLocale::LongFormat);
    case Qt::RFC2822Date:
        return rfcDateImpl(string).time;
    case Qt::ISODate:
    case Qt::ISODateWithMs:
    case Qt::TextDate:
    default:
        return fromIsoTimeString(QStringView(string), format, nullptr);
    }
}

/*!
    Returns the QTime represented by the \a string, using the \a
    format given, or an invalid time if the string cannot be parsed.

    These expressions may be used for the format:

    \table
    \header \li Expression \li Output
    \row \li h
         \li The hour without a leading zero (0 to 23 or 1 to 12 if AM/PM display)
    \row \li hh
         \li The hour with a leading zero (00 to 23 or 01 to 12 if AM/PM display)
    \row \li H
         \li The hour without a leading zero (0 to 23, even with AM/PM display)
    \row \li HH
         \li The hour with a leading zero (00 to 23, even with AM/PM display)
    \row \li m \li The minute without a leading zero (0 to 59)
    \row \li mm \li The minute with a leading zero (00 to 59)
    \row \li s \li The whole second, without any leading zero (0 to 59)
    \row \li ss \li The whole second, with a leading zero where applicable (00 to 59)
    \row \li z \li The fractional part of the second, to go after a decimal
                point, without trailing zeroes (0 to 999).  Thus "\c{s.z}"
                reports the seconds to full available (millisecond) precision
                without trailing zeroes.
    \row \li zzz \li The fractional part of the second, to millisecond
                precision, including trailing zeroes where applicable (000 to 999).
    \row \li AP or A
         \li Interpret as an AM/PM time. \e A/AP will match an upper-case
             version of either QLocale::amText() or QLocale::pmText().
    \row \li ap or a
         \li Interpret as an am/pm time. \e a/ap will match a lower-case version
             of either QLocale::amText() or QLocale::pmText().
    \row \li t \li the timezone (for example "CEST")
    \endtable

    All other input characters will be treated as text. Any sequence
    of characters that are enclosed in single quotes will also be
    treated as text and not be used as an expression.

    \snippet code/src_corelib_tools_qdatetime.cpp 6

    If the format is not satisfied, an invalid QTime is returned.
    Expressions that do not expect leading zeroes to be given (h, m, s
    and z) are greedy. This means that they will use two digits even if
    this puts them outside the range of accepted values and leaves too
    few digits for other sections. For example, the following string
    could have meant 00:07:10, but the m will grab two digits, resulting
    in an invalid time:

    \snippet code/src_corelib_tools_qdatetime.cpp 7

    Any field that is not represented in the format will be set to zero.
    For example:

    \snippet code/src_corelib_tools_qdatetime.cpp 8

    \sa toString(), QDateTime::fromString(), QDate::fromString(),
    QLocale::toTime()
*/

QTime QTime::fromString(const QString &string, const QString &format)
{
    QTime time;
#if QT_CONFIG(datetimeparser)
    QDateTimeParser dt(QVariant::Time, QDateTimeParser::FromString, QCalendar());
    // dt.setDefaultLocale(QLocale::c()); ### Qt 6
    if (dt.parseFormat(format))
        dt.fromString(string, 0, &time);
#else
    Q_UNUSED(string);
    Q_UNUSED(format);
#endif
    return time;
}

#endif // datestring


/*!
    \overload

    Returns \c true if the specified time is valid; otherwise returns
    false.

    The time is valid if \a h is in the range 0 to 23, \a m and
    \a s are in the range 0 to 59, and \a ms is in the range 0 to 999.

    Example:

    \snippet code/src_corelib_tools_qdatetime.cpp 9
*/

bool QTime::isValid(int h, int m, int s, int ms)
{
    return (uint)h < 24 && (uint)m < 60 && (uint)s < 60 && (uint)ms < 1000;
}

#if QT_DEPRECATED_SINCE(5, 14) // ### Qt 6: remove
/*!
    Sets this time to the current time. This is practical for timing:

    \snippet code/src_corelib_tools_qdatetime.cpp 10

    \sa restart(), elapsed(), currentTime()
*/

void QTime::start()
{
    *this = currentTime();
}

/*!
    Sets this time to the current time and returns the number of
    milliseconds that have elapsed since the last time start() or
    restart() was called.

    This function is guaranteed to be atomic and is thus very handy
    for repeated measurements. Call start() to start the first
    measurement, and restart() for each later measurement.

    Note that the counter wraps to zero 24 hours after the last call
    to start() or restart().

    \warning If the system's clock setting has been changed since the
    last time start() or restart() was called, the result is
    undefined. This can happen when daylight-saving time is turned on
    or off.

    \sa start(), elapsed(), currentTime()
*/

int QTime::restart()
{
    QTime t = currentTime();
    int n = msecsTo(t);
    if (n < 0)                                // passed midnight
        n += 86400*1000;
    *this = t;
    return n;
}

/*!
    Returns the number of milliseconds that have elapsed since the
    last time start() or restart() was called.

    Note that the counter wraps to zero 24 hours after the last call
    to start() or restart.

    Note that the accuracy depends on the accuracy of the underlying
    operating system; not all systems provide 1-millisecond accuracy.

    \warning If the system's clock setting has been changed since the
    last time start() or restart() was called, the result is
    undefined. This can happen when daylight-saving time is turned on
    or off.

    \sa start(), restart()
*/

int QTime::elapsed() const
{
    int n = msecsTo(currentTime());
    if (n < 0)                                // passed midnight
        n += 86400 * 1000;
    return n;
}
#endif // Use QElapsedTimer instead !

/*****************************************************************************
  QDateTime static helper functions
 *****************************************************************************/

// get the types from QDateTime (through QDateTimePrivate)
typedef QDateTimePrivate::QDateTimeShortData ShortData;
typedef QDateTimePrivate::QDateTimeData QDateTimeData;

// Returns the platform variant of timezone, i.e. the standard time offset
// The timezone external variable is documented as always holding the
// Standard Time offset as seconds west of Greenwich, i.e. UTC+01:00 is -3600
// Note this may not be historicaly accurate.
// Relies on tzset, mktime, or localtime having been called to populate timezone
static int qt_timezone()
{
#if defined(_MSC_VER)
        long offset;
        _get_timezone(&offset);
        return offset;
#elif defined(Q_OS_BSD4) && !defined(Q_OS_DARWIN)
        time_t clock = time(NULL);
        struct tm t;
        localtime_r(&clock, &t);
        // QTBUG-36080 Workaround for systems without the POSIX timezone
        // variable. This solution is not very efficient but fixing it is up to
        // the libc implementations.
        //
        // tm_gmtoff has some important differences compared to the timezone
        // variable:
        // - It returns the number of seconds east of UTC, and we want the
        //   number of seconds west of UTC.
        // - It also takes DST into account, so we need to adjust it to always
        //   get the Standard Time offset.
        return -t.tm_gmtoff + (t.tm_isdst ? (long)SECS_PER_HOUR : 0L);
#elif defined(Q_OS_INTEGRITY) || defined(Q_OS_RTEMS)
        return 0;
#else
        return timezone;
#endif // Q_OS_WIN
}

// Returns the tzname, assume tzset has been called already
static QString qt_tzname(QDateTimePrivate::DaylightStatus daylightStatus)
{
    int isDst = (daylightStatus == QDateTimePrivate::DaylightTime) ? 1 : 0;
#if defined(Q_CC_MSVC)
    size_t s = 0;
    char name[512];
    if (_get_tzname(&s, name, 512, isDst))
        return QString();
    return QString::fromLocal8Bit(name);
#else
    return QString::fromLocal8Bit(tzname[isDst]);
#endif // Q_OS_WIN
}

#if QT_CONFIG(datetimeparser) && QT_CONFIG(timezone)
/*
  \internal
  Implemented here to share qt_tzname()
*/
int QDateTimeParser::startsWithLocalTimeZone(const QStringRef name)
{
    QDateTimePrivate::DaylightStatus zones[2] = {
        QDateTimePrivate::StandardTime,
        QDateTimePrivate::DaylightTime
    };
    for (const auto z : zones) {
        QString zone(qt_tzname(z));
        if (name.startsWith(zone))
            return zone.size();
    }
    return 0;
}
#endif // datetimeparser && timezone

// Calls the platform variant of mktime for the given date, time and daylightStatus,
// and updates the date, time, daylightStatus and abbreviation with the returned values
// If the date falls outside the 1970 to 2037 range supported by mktime / time_t
// then null date/time will be returned, you should adjust the date first if
// you need a guaranteed result.
static qint64 qt_mktime(QDate *date, QTime *time, QDateTimePrivate::DaylightStatus *daylightStatus,
                        QString *abbreviation, bool *ok = nullptr)
{
    const qint64 msec = time->msec();
    int yy, mm, dd;
    date->getDate(&yy, &mm, &dd);

    // All other platforms provide standard C library time functions
    tm local;
    memset(&local, 0, sizeof(local)); // tm_[wy]day plus any non-standard fields
    local.tm_sec = time->second();
    local.tm_min = time->minute();
    local.tm_hour = time->hour();
    local.tm_mday = dd;
    local.tm_mon = mm - 1;
    local.tm_year = yy - 1900;
    if (daylightStatus)
        local.tm_isdst = int(*daylightStatus);
    else
        local.tm_isdst = -1;

#if defined(Q_OS_WIN)
    int hh = local.tm_hour;
#endif // Q_OS_WIN
    time_t secsSinceEpoch = qMkTime(&local);
    if (secsSinceEpoch != time_t(-1)) {
        *date = QDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        *time = QTime(local.tm_hour, local.tm_min, local.tm_sec, msec);
#if defined(Q_OS_WIN)
        // Windows mktime for the missing hour subtracts 1 hour from the time
        // instead of adding 1 hour.  If time differs and is standard time then
        // this has happened, so add 2 hours to the time and 1 hour to the msecs
        if (local.tm_isdst == 0 && local.tm_hour != hh) {
            if (time->hour() >= 22)
                *date = date->addDays(1);
            *time = time->addSecs(2 * SECS_PER_HOUR);
            secsSinceEpoch += SECS_PER_HOUR;
            local.tm_isdst = 1;
        }
#endif // Q_OS_WIN
        if (local.tm_isdst >= 1) {
            if (daylightStatus)
                *daylightStatus = QDateTimePrivate::DaylightTime;
            if (abbreviation)
                *abbreviation = qt_tzname(QDateTimePrivate::DaylightTime);
        } else if (local.tm_isdst == 0) {
            if (daylightStatus)
                *daylightStatus = QDateTimePrivate::StandardTime;
            if (abbreviation)
                *abbreviation = qt_tzname(QDateTimePrivate::StandardTime);
        } else {
            if (daylightStatus)
                *daylightStatus = QDateTimePrivate::UnknownDaylightTime;
            if (abbreviation)
                *abbreviation = qt_tzname(QDateTimePrivate::StandardTime);
        }
        if (ok)
            *ok = true;
    } else {
        *date = QDate();
        *time = QTime();
        if (daylightStatus)
            *daylightStatus = QDateTimePrivate::UnknownDaylightTime;
        if (abbreviation)
            *abbreviation = QString();
        if (ok)
            *ok = false;
    }

    return ((qint64)secsSinceEpoch * 1000) + msec;
}

// Calls the platform variant of localtime for the given msecs, and updates
// the date, time, and DST status with the returned values.
static bool qt_localtime(qint64 msecsSinceEpoch, QDate *localDate, QTime *localTime,
                         QDateTimePrivate::DaylightStatus *daylightStatus)
{
    const time_t secsSinceEpoch = msecsSinceEpoch / 1000;
    const int msec = msecsSinceEpoch % 1000;

    tm local;
    bool valid = false;

    // localtime() is specified to work as if it called tzset().
    // localtime_r() does not have this constraint, so make an explicit call.
    // The explicit call should also request the timezone info be re-parsed.
    qTzSet();
#if QT_CONFIG(thread) && defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    // Use the reentrant version of localtime() where available
    // as is thread-safe and doesn't use a shared static data area
    tm *res = nullptr;
    res = localtime_r(&secsSinceEpoch, &local);
    if (res)
        valid = true;
#elif defined(Q_CC_MSVC)
    if (!_localtime64_s(&local, &secsSinceEpoch))
        valid = true;
#else
    // Returns shared static data which may be overwritten at any time
    // So copy the result asap
    tm *res = nullptr;
    res = localtime(&secsSinceEpoch);
    if (res) {
        local = *res;
        valid = true;
    }
#endif
    if (valid) {
        *localDate = QDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        *localTime = QTime(local.tm_hour, local.tm_min, local.tm_sec, msec);
        if (daylightStatus) {
            if (local.tm_isdst > 0)
                *daylightStatus = QDateTimePrivate::DaylightTime;
            else if (local.tm_isdst < 0)
                *daylightStatus = QDateTimePrivate::UnknownDaylightTime;
            else
                *daylightStatus = QDateTimePrivate::StandardTime;
        }
        return true;
    } else {
        *localDate = QDate();
        *localTime = QTime();
        if (daylightStatus)
            *daylightStatus = QDateTimePrivate::UnknownDaylightTime;
        return false;
    }
}

// Converts an msecs value into a date and time
static void msecsToTime(qint64 msecs, QDate *date, QTime *time)
{
    qint64 jd = JULIAN_DAY_FOR_EPOCH;
    qint64 ds = 0;

    if (msecs >= MSECS_PER_DAY || msecs <= -MSECS_PER_DAY) {
        jd += msecs / MSECS_PER_DAY;
        msecs %= MSECS_PER_DAY;
    }

    if (msecs < 0) {
        ds = MSECS_PER_DAY - msecs - 1;
        jd -= ds / MSECS_PER_DAY;
        ds = ds % MSECS_PER_DAY;
        ds = MSECS_PER_DAY - ds - 1;
    } else {
        ds = msecs;
    }

    if (date)
        *date = QDate::fromJulianDay(jd);
    if (time)
        *time = QTime::fromMSecsSinceStartOfDay(ds);
}

// Converts a date/time value into msecs
static qint64 timeToMSecs(QDate date, QTime time)
{
    return ((date.toJulianDay() - JULIAN_DAY_FOR_EPOCH) * MSECS_PER_DAY)
           + time.msecsSinceStartOfDay();
}

// Convert an MSecs Since Epoch into Local Time
static bool epochMSecsToLocalTime(qint64 msecs, QDate *localDate, QTime *localTime,
                                  QDateTimePrivate::DaylightStatus *daylightStatus = nullptr)
{
    if (msecs < 0) {
        // Docs state any LocalTime before 1970-01-01 will *not* have any Daylight Time applied
        // Instead just use the standard offset from UTC to convert to UTC time
        qTzSet();
        msecsToTime(msecs - qt_timezone() * 1000, localDate, localTime);
        if (daylightStatus)
            *daylightStatus = QDateTimePrivate::StandardTime;
        return true;
    } else if (msecs > (qint64(TIME_T_MAX) * 1000)) {
        // Docs state any LocalTime after 2037-12-31 *will* have any DST applied
        // but this may fall outside the supported time_t range, so need to fake it.
        // Use existing method to fake the conversion, but this is deeply flawed as it may
        // apply the conversion from the wrong day number, e.g. if rule is last Sunday of month
        // TODO Use QTimeZone when available to apply the future rule correctly
        QDate utcDate;
        QTime utcTime;
        msecsToTime(msecs, &utcDate, &utcTime);
        int year, month, day;
        utcDate.getDate(&year, &month, &day);
        // 2037 is not a leap year, so make sure date isn't Feb 29
        if (month == 2 && day == 29)
            --day;
        QDate fakeDate(2037, month, day);
        qint64 fakeMsecs = QDateTime(fakeDate, utcTime, Qt::UTC).toMSecsSinceEpoch();
        bool res = qt_localtime(fakeMsecs, localDate, localTime, daylightStatus);
        *localDate = localDate->addDays(fakeDate.daysTo(utcDate));
        return res;
    } else {
        // Falls inside time_t suported range so can use localtime
        return qt_localtime(msecs, localDate, localTime, daylightStatus);
    }
}

// Convert a LocalTime expressed in local msecs encoding and the corresponding
// DST status into a UTC epoch msecs. Optionally populate the returned
// values from mktime for the adjusted local date and time.
static qint64 localMSecsToEpochMSecs(qint64 localMsecs,
                                     QDateTimePrivate::DaylightStatus *daylightStatus,
                                     QDate *localDate = nullptr, QTime *localTime = nullptr,
                                     QString *abbreviation = nullptr)
{
    QDate dt;
    QTime tm;
    msecsToTime(localMsecs, &dt, &tm);

    const qint64 msecsMax = qint64(TIME_T_MAX) * 1000;

    if (localMsecs <= qint64(MSECS_PER_DAY)) {

        // Docs state any LocalTime before 1970-01-01 will *not* have any DST applied

        // First, if localMsecs is within +/- 1 day of minimum time_t try mktime in case it does
        // fall after minimum and needs proper DST conversion
        if (localMsecs >= -qint64(MSECS_PER_DAY)) {
            bool valid;
            qint64 utcMsecs = qt_mktime(&dt, &tm, daylightStatus, abbreviation, &valid);
            if (valid && utcMsecs >= 0) {
                // mktime worked and falls in valid range, so use it
                if (localDate)
                    *localDate = dt;
                if (localTime)
                    *localTime = tm;
                return utcMsecs;
            }
        } else {
            // If we don't call mktime then need to call tzset to get offset
            qTzSet();
        }
        // Time is clearly before 1970-01-01 so just use standard offset to convert
        qint64 utcMsecs = localMsecs + qt_timezone() * 1000;
        if (localDate || localTime)
            msecsToTime(localMsecs, localDate, localTime);
        if (daylightStatus)
            *daylightStatus = QDateTimePrivate::StandardTime;
        if (abbreviation)
            *abbreviation = qt_tzname(QDateTimePrivate::StandardTime);
        return utcMsecs;

    } else if (localMsecs >= msecsMax - MSECS_PER_DAY) {

        // Docs state any LocalTime after 2037-12-31 *will* have any DST applied
        // but this may fall outside the supported time_t range, so need to fake it.

        // First, if localMsecs is within +/- 1 day of maximum time_t try mktime in case it does
        // fall before maximum and can use proper DST conversion
        if (localMsecs <= msecsMax + MSECS_PER_DAY) {
            bool valid;
            qint64 utcMsecs = qt_mktime(&dt, &tm, daylightStatus, abbreviation, &valid);
            if (valid && utcMsecs <= msecsMax) {
                // mktime worked and falls in valid range, so use it
                if (localDate)
                    *localDate = dt;
                if (localTime)
                    *localTime = tm;
                return utcMsecs;
            }
        }
        // Use existing method to fake the conversion, but this is deeply flawed as it may
        // apply the conversion from the wrong day number, e.g. if rule is last Sunday of month
        // TODO Use QTimeZone when available to apply the future rule correctly
        int year, month, day;
        dt.getDate(&year, &month, &day);
        // 2037 is not a leap year, so make sure date isn't Feb 29
        if (month == 2 && day == 29)
            --day;
        QDate fakeDate(2037, month, day);
        qint64 fakeDiff = fakeDate.daysTo(dt);
        qint64 utcMsecs = qt_mktime(&fakeDate, &tm, daylightStatus, abbreviation);
        if (localDate)
            *localDate = fakeDate.addDays(fakeDiff);
        if (localTime)
            *localTime = tm;
        QDate utcDate;
        QTime utcTime;
        msecsToTime(utcMsecs, &utcDate, &utcTime);
        utcDate = utcDate.addDays(fakeDiff);
        utcMsecs = timeToMSecs(utcDate, utcTime);
        return utcMsecs;

    } else {

        // Clearly falls inside 1970-2037 suported range so can use mktime
        qint64 utcMsecs = qt_mktime(&dt, &tm, daylightStatus, abbreviation);
        if (localDate)
            *localDate = dt;
        if (localTime)
            *localTime = tm;
        return utcMsecs;

    }
}

static inline bool specCanBeSmall(Qt::TimeSpec spec)
{
    return spec == Qt::LocalTime || spec == Qt::UTC;
}

static inline bool msecsCanBeSmall(qint64 msecs)
{
    if (!QDateTimeData::CanBeSmall)
        return false;

    ShortData sd;
    sd.msecs = qintptr(msecs);
    return sd.msecs == msecs;
}

static Q_DECL_CONSTEXPR inline
QDateTimePrivate::StatusFlags mergeSpec(QDateTimePrivate::StatusFlags status, Qt::TimeSpec spec)
{
    return QDateTimePrivate::StatusFlags((status & ~QDateTimePrivate::TimeSpecMask) |
                                         (int(spec) << QDateTimePrivate::TimeSpecShift));
}

static Q_DECL_CONSTEXPR inline Qt::TimeSpec extractSpec(QDateTimePrivate::StatusFlags status)
{
    return Qt::TimeSpec((status & QDateTimePrivate::TimeSpecMask) >> QDateTimePrivate::TimeSpecShift);
}

// Set the Daylight Status if LocalTime set via msecs
static Q_DECL_RELAXED_CONSTEXPR inline QDateTimePrivate::StatusFlags
mergeDaylightStatus(QDateTimePrivate::StatusFlags sf, QDateTimePrivate::DaylightStatus status)
{
    sf &= ~QDateTimePrivate::DaylightMask;
    if (status == QDateTimePrivate::DaylightTime) {
        sf |= QDateTimePrivate::SetToDaylightTime;
    } else if (status == QDateTimePrivate::StandardTime) {
        sf |= QDateTimePrivate::SetToStandardTime;
    }
    return sf;
}

// Get the DST Status if LocalTime set via msecs
static Q_DECL_RELAXED_CONSTEXPR inline
QDateTimePrivate::DaylightStatus extractDaylightStatus(QDateTimePrivate::StatusFlags status)
{
    if (status & QDateTimePrivate::SetToDaylightTime)
        return QDateTimePrivate::DaylightTime;
    if (status & QDateTimePrivate::SetToStandardTime)
        return QDateTimePrivate::StandardTime;
    return QDateTimePrivate::UnknownDaylightTime;
}

static inline qint64 getMSecs(const QDateTimeData &d)
{
    if (d.isShort()) {
        // same as, but producing better code
        //return d.data.msecs;
        return qintptr(d.d) >> 8;
    }
    return d->m_msecs;
}

static inline QDateTimePrivate::StatusFlags getStatus(const QDateTimeData &d)
{
    if (d.isShort()) {
        // same as, but producing better code
        //return StatusFlag(d.data.status);
        return QDateTimePrivate::StatusFlag(qintptr(d.d) & 0xFF);
    }
    return d->m_status;
}

static inline Qt::TimeSpec getSpec(const QDateTimeData &d)
{
    return extractSpec(getStatus(d));
}

#if QT_CONFIG(timezone)
void QDateTimePrivate::setUtcOffsetByTZ(qint64 atMSecsSinceEpoch)
{
    m_offsetFromUtc = m_timeZone.d->offsetFromUtc(atMSecsSinceEpoch);
}
#endif

// Refresh the LocalTime validity and offset
static void refreshDateTime(QDateTimeData &d)
{
    auto status = getStatus(d);
    const auto spec = extractSpec(status);
    const qint64 msecs = getMSecs(d);
    qint64 epochMSecs = 0;
    int offsetFromUtc = 0;
    QDate testDate;
    QTime testTime;
    Q_ASSERT(spec == Qt::TimeZone || spec == Qt::LocalTime);

#if QT_CONFIG(timezone)
    // If not valid time zone then is invalid
    if (spec == Qt::TimeZone) {
        if (!d->m_timeZone.isValid()) {
            status &= ~QDateTimePrivate::ValidDateTime;
        } else {
            epochMSecs = QDateTimePrivate::zoneMSecsToEpochMSecs(msecs, d->m_timeZone, extractDaylightStatus(status), &testDate, &testTime);
            d->setUtcOffsetByTZ(epochMSecs);
        }
    }
#endif // timezone

    // If not valid date and time then is invalid
    if (!(status & QDateTimePrivate::ValidDate) || !(status & QDateTimePrivate::ValidTime)) {
        status &= ~QDateTimePrivate::ValidDateTime;
        if (status & QDateTimePrivate::ShortData) {
            d.data.status = status;
        } else {
            d->m_status = status;
            d->m_offsetFromUtc = 0;
        }
        return;
    }

    // We have a valid date and time and a Qt::LocalTime or Qt::TimeZone that needs calculating
    // LocalTime and TimeZone might fall into a "missing" DST transition hour
    // Calling toEpochMSecs will adjust the returned date/time if it does
    if (spec == Qt::LocalTime) {
        auto dstStatus = extractDaylightStatus(status);
        epochMSecs = localMSecsToEpochMSecs(msecs, &dstStatus, &testDate, &testTime);
    }
    if (timeToMSecs(testDate, testTime) == msecs) {
        status |= QDateTimePrivate::ValidDateTime;
        // Cache the offset to use in offsetFromUtc()
        offsetFromUtc = (msecs - epochMSecs) / 1000;
    } else {
        status &= ~QDateTimePrivate::ValidDateTime;
    }

    if (status & QDateTimePrivate::ShortData) {
        d.data.status = status;
    } else {
        d->m_status = status;
        d->m_offsetFromUtc = offsetFromUtc;
    }
}

// Check the UTC / offsetFromUTC validity
static void checkValidDateTime(QDateTimeData &d)
{
    auto status = getStatus(d);
    auto spec = extractSpec(status);
    switch (spec) {
    case Qt::OffsetFromUTC:
    case Qt::UTC:
        // for these, a valid date and a valid time imply a valid QDateTime
        if ((status & QDateTimePrivate::ValidDate) && (status & QDateTimePrivate::ValidTime))
            status |= QDateTimePrivate::ValidDateTime;
        else
            status &= ~QDateTimePrivate::ValidDateTime;
        if (status & QDateTimePrivate::ShortData)
            d.data.status = status;
        else
            d->m_status = status;
        break;
    case Qt::TimeZone:
    case Qt::LocalTime:
        // for these, we need to check whether the timezone is valid and whether
        // the time is valid in that timezone. Expensive, but no other option.
        refreshDateTime(d);
        break;
    }
}

static void setTimeSpec(QDateTimeData &d, Qt::TimeSpec spec, int offsetSeconds)
{
    auto status = getStatus(d);
    status &= ~(QDateTimePrivate::ValidDateTime | QDateTimePrivate::DaylightMask |
                QDateTimePrivate::TimeSpecMask);

    switch (spec) {
    case Qt::OffsetFromUTC:
        if (offsetSeconds == 0)
            spec = Qt::UTC;
        break;
    case Qt::TimeZone:
        // Use system time zone instead
        spec = Qt::LocalTime;
        Q_FALLTHROUGH();
    case Qt::UTC:
    case Qt::LocalTime:
        offsetSeconds = 0;
        break;
    }

    status = mergeSpec(status, spec);
    if (d.isShort() && offsetSeconds == 0) {
        d.data.status = status;
    } else {
        d.detach();
        d->m_status = status & ~QDateTimePrivate::ShortData;
        d->m_offsetFromUtc = offsetSeconds;
#if QT_CONFIG(timezone)
        d->m_timeZone = QTimeZone();
#endif // timezone
    }
}

static void setDateTime(QDateTimeData &d, QDate date, QTime time)
{
    // If the date is valid and the time is not we set time to 00:00:00
    QTime useTime = time;
    if (!useTime.isValid() && date.isValid())
        useTime = QTime::fromMSecsSinceStartOfDay(0);

    QDateTimePrivate::StatusFlags newStatus = { };

    // Set date value and status
    qint64 days = 0;
    if (date.isValid()) {
        days = date.toJulianDay() - JULIAN_DAY_FOR_EPOCH;
        newStatus = QDateTimePrivate::ValidDate;
    }

    // Set time value and status
    int ds = 0;
    if (useTime.isValid()) {
        ds = useTime.msecsSinceStartOfDay();
        newStatus |= QDateTimePrivate::ValidTime;
    }

    // Set msecs serial value
    qint64 msecs = (days * MSECS_PER_DAY) + ds;
    if (d.isShort()) {
        // let's see if we can keep this short
        if (msecsCanBeSmall(msecs)) {
            // yes, we can
            d.data.msecs = qintptr(msecs);
            d.data.status &= ~(QDateTimePrivate::ValidityMask | QDateTimePrivate::DaylightMask);
            d.data.status |= newStatus;
        } else {
            // nope...
            d.detach();
        }
    }
    if (!d.isShort()) {
        d.detach();
        d->m_msecs = msecs;
        d->m_status &= ~(QDateTimePrivate::ValidityMask | QDateTimePrivate::DaylightMask);
        d->m_status |= newStatus;
    }

    // Set if date and time are valid
    checkValidDateTime(d);
}

static QPair<QDate, QTime> getDateTime(const QDateTimeData &d)
{
    QPair<QDate, QTime> result;
    qint64 msecs = getMSecs(d);
    auto status = getStatus(d);
    msecsToTime(msecs, &result.first, &result.second);

    if (!status.testFlag(QDateTimePrivate::ValidDate))
        result.first = QDate();

    if (!status.testFlag(QDateTimePrivate::ValidTime))
        result.second = QTime();

    return result;
}

/*****************************************************************************
  QDateTime::Data member functions
 *****************************************************************************/

inline QDateTime::Data::Data()
{
    // default-constructed data has a special exception:
    // it can be small even if CanBeSmall == false
    // (optimization so we don't allocate memory in the default constructor)
    quintptr value = quintptr(mergeSpec(QDateTimePrivate::ShortData, Qt::LocalTime));
    d = reinterpret_cast<QDateTimePrivate *>(value);
}

inline QDateTime::Data::Data(Qt::TimeSpec spec)
{
    if (CanBeSmall && Q_LIKELY(specCanBeSmall(spec))) {
        d = reinterpret_cast<QDateTimePrivate *>(quintptr(mergeSpec(QDateTimePrivate::ShortData, spec)));
    } else {
        // the structure is too small, we need to detach
        d = new QDateTimePrivate;
        d->ref.ref();
        d->m_status = mergeSpec(nullptr, spec);
    }
}

inline QDateTime::Data::Data(const Data &other)
    : d(other.d)
{
    if (!isShort()) {
        // check if we could shrink
        if (specCanBeSmall(extractSpec(d->m_status)) && msecsCanBeSmall(d->m_msecs)) {
            ShortData sd;
            sd.msecs = qintptr(d->m_msecs);
            sd.status = d->m_status | QDateTimePrivate::ShortData;
            data = sd;
        } else {
            // no, have to keep it big
            d->ref.ref();
        }
    }
}

inline QDateTime::Data::Data(Data &&other)
    : d(other.d)
{
    // reset the other to a short state
    Data dummy;
    Q_ASSERT(dummy.isShort());
    other.d = dummy.d;
}

inline QDateTime::Data &QDateTime::Data::operator=(const Data &other)
{
    if (d == other.d)
        return *this;

    auto x = d;
    d = other.d;
    if (!other.isShort()) {
        // check if we could shrink
        if (specCanBeSmall(extractSpec(other.d->m_status)) && msecsCanBeSmall(other.d->m_msecs)) {
            ShortData sd;
            sd.msecs = qintptr(other.d->m_msecs);
            sd.status = other.d->m_status | QDateTimePrivate::ShortData;
            data = sd;
        } else {
            // no, have to keep it big
            other.d->ref.ref();
        }
    }

    if (!(quintptr(x) & QDateTimePrivate::ShortData) && !x->ref.deref())
        delete x;
    return *this;
}

inline QDateTime::Data::~Data()
{
    if (!isShort() && !d->ref.deref())
        delete d;
}

inline bool QDateTime::Data::isShort() const
{
    bool b = quintptr(d) & QDateTimePrivate::ShortData;

    // sanity check:
    Q_ASSERT(b || (d->m_status & QDateTimePrivate::ShortData) == 0);

    // even if CanBeSmall = false, we have short data for a default-constructed
    // QDateTime object. But it's unlikely.
    if (CanBeSmall)
        return Q_LIKELY(b);
    return Q_UNLIKELY(b);
}

inline void QDateTime::Data::detach()
{
    QDateTimePrivate *x;
    bool wasShort = isShort();
    if (wasShort) {
        // force enlarging
        x = new QDateTimePrivate;
        x->m_status = QDateTimePrivate::StatusFlag(data.status & ~QDateTimePrivate::ShortData);
        x->m_msecs = data.msecs;
    } else {
        if (d->ref.loadRelaxed() == 1)
            return;

        x = new QDateTimePrivate(*d);
    }

    x->ref.storeRelaxed(1);
    if (!wasShort && !d->ref.deref())
        delete d;
    d = x;
}

inline const QDateTimePrivate *QDateTime::Data::operator->() const
{
    Q_ASSERT(!isShort());
    return d;
}

inline QDateTimePrivate *QDateTime::Data::operator->()
{
    // should we attempt to detach here?
    Q_ASSERT(!isShort());
    Q_ASSERT(d->ref.loadRelaxed() == 1);
    return d;
}

/*****************************************************************************
  QDateTimePrivate member functions
 *****************************************************************************/

Q_NEVER_INLINE
QDateTime::Data QDateTimePrivate::create(const QDate &toDate, const QTime &toTime, Qt::TimeSpec toSpec,
                                         int offsetSeconds)
{
    QDateTime::Data result(toSpec);
    setTimeSpec(result, toSpec, offsetSeconds);
    setDateTime(result, toDate, toTime);
    return result;
}

#if QT_CONFIG(timezone)
inline QDateTime::Data QDateTimePrivate::create(const QDate &toDate, const QTime &toTime,
                                                const QTimeZone &toTimeZone)
{
    QDateTime::Data result(Qt::TimeZone);
    Q_ASSERT(!result.isShort());

    result.d->m_status = mergeSpec(result.d->m_status, Qt::TimeZone);
    result.d->m_timeZone = toTimeZone;
    setDateTime(result, toDate, toTime);
    return result;
}

// Convert a TimeZone time expressed in zone msecs encoding into a UTC epoch msecs
// DST transitions are disambiguated by hint.
inline qint64 QDateTimePrivate::zoneMSecsToEpochMSecs(qint64 zoneMSecs, const QTimeZone &zone,
                                                      DaylightStatus hint,
                                                      QDate *zoneDate, QTime *zoneTime)
{
    Q_ASSERT(zone.isValid());
    // Get the effective data from QTimeZone
    QTimeZonePrivate::Data data = zone.d->dataForLocalTime(zoneMSecs, int(hint));
    // Docs state any time before 1970-01-01 will *not* have any DST applied
    // but all affected times afterwards will have DST applied.
    if (data.atMSecsSinceEpoch < 0) {
        msecsToTime(zoneMSecs, zoneDate, zoneTime);
        return zoneMSecs - data.standardTimeOffset * 1000;
    } else {
        msecsToTime(data.atMSecsSinceEpoch + data.offsetFromUtc * 1000, zoneDate, zoneTime);
        return data.atMSecsSinceEpoch;
    }
}
#endif // timezone

/*****************************************************************************
  QDateTime member functions
 *****************************************************************************/

/*!
    \class QDateTime
    \inmodule QtCore
    \ingroup shared
    \reentrant
    \brief The QDateTime class provides date and time functions.


    A QDateTime object encodes a calendar date and a clock time (a
    "datetime"). It combines features of the QDate and QTime classes.
    It can read the current datetime from the system clock. It
    provides functions for comparing datetimes and for manipulating a
    datetime by adding a number of seconds, days, months, or years.

    QDateTime can describe datetimes with respect to \l{Qt::LocalTime}{local
    time}, to \l{Qt::UTC}{UTC}, to a specified \l{Qt::OffsetFromUTC}{offset from
    UTC} or to a specified \l{Qt::TimeZone}{time zone}, in conjunction with the
    QTimeZone class. For example, a time zone of "Europe/Berlin" will apply the
    daylight-saving rules as used in Germany since 1970. In contrast, an offset
    from UTC of +3600 seconds is one hour ahead of UTC (usually written in ISO
    standard notation as "UTC+01:00"), with no daylight-saving offset or
    changes. When using either local time or a specified time zone, time-zone
    transitions such as the starts and ends of daylight-saving time (DST; but
    see below) are taken into account. The choice of system used to represent a
    datetime is described as its "timespec".

    A QDateTime object is typically created either by giving a date and time
    explicitly in the constructor, or by using a static function such as
    currentDateTime() or fromMSecsSinceEpoch(). The date and time can be changed
    with setDate() and setTime(). A datetime can also be set using the
    setMSecsSinceEpoch() function that takes the time, in milliseconds, since
    00:00:00 on January 1, 1970. The fromString() function returns a QDateTime,
    given a string and a date format used to interpret the date within the
    string.

    QDateTime::currentDateTime() returns a QDateTime that expresses the current
    time with respect to local time. QDateTime::currentDateTimeUtc() returns a
    QDateTime that expresses the current time with respect to UTC.

    The date() and time() functions provide access to the date and
    time parts of the datetime. The same information is provided in
    textual format by the toString() function.

    QDateTime provides a full set of operators to compare two
    QDateTime objects, where smaller means earlier and larger means
    later.

    You can increment (or decrement) a datetime by a given number of
    milliseconds using addMSecs(), seconds using addSecs(), or days using
    addDays(). Similarly, you can use addMonths() and addYears(). The daysTo()
    function returns the number of days between two datetimes, secsTo() returns
    the number of seconds between two datetimes, and msecsTo() returns the
    number of milliseconds between two datetimes. These operations are aware of
    daylight-saving time (DST) and other time-zone transitions, where
    applicable.

    Use toTimeSpec() to express a datetime in local time or UTC,
    toOffsetFromUtc() to express in terms of an offset from UTC, or toTimeZone()
    to express it with respect to a general time zone. You can use timeSpec() to
    find out what time-spec a QDateTime object stores its time relative to. When
    that is Qt::TimeZone, you can use timeZone() to find out which zone it is
    using.

    \note QDateTime does not account for leap seconds.

    \section1 Remarks

    \section2 No Year 0

    There is no year 0. Dates in that year are considered invalid. The
    year -1 is the year "1 before Christ" or "1 before current era."
    The day before 1 January 1 CE is 31 December 1 BCE.

    \section2 Range of Valid Dates

    The range of values that QDateTime can represent is dependent on the
    internal storage implementation. QDateTime is currently stored in a qint64
    as a serial msecs value encoding the date and time. This restricts the date
    range to about +/- 292 million years, compared to the QDate range of +/- 2
    billion years. Care must be taken when creating a QDateTime with extreme
    values that you do not overflow the storage. The exact range of supported
    values varies depending on the Qt::TimeSpec and time zone.

    \section2 Use of Timezones

    QDateTime uses the system's time zone information to determine the current
    local time zone and its offset from UTC. If the system is not configured
    correctly or not up-to-date, QDateTime will give wrong results.

    QDateTime likewise uses system-provided information to determine the offsets
    of other timezones from UTC. If this information is incomplete or out of
    date, QDateTime will give wrong results. See the QTimeZone documentation for
    more details.

    On modern Unix systems, this means QDateTime usually has accurate
    information about historical transitions (including DST, see below) whenever
    possible. On Windows, where the system doesn't support historical timezone
    data, historical accuracy is not maintained with respect to timezone
    transitions, notably including DST.

    \section2 Daylight-Saving Time (DST)

    QDateTime takes into account transitions between Standard Time and
    Daylight-Saving Time. For example, if the transition is at 2am and the clock
    goes forward to 3am, then there is a "missing" hour from 02:00:00 to
    02:59:59.999 which QDateTime considers to be invalid. Any date arithmetic
    performed will take this missing hour into account and return a valid
    result. For example, adding one minute to 01:59:59 will get 03:00:00.

    The range of valid dates taking DST into account is 1970-01-01 to the
    present, and rules are in place for handling DST correctly until 2037-12-31,
    but these could change. For dates after 2037, QDateTime makes a \e{best
    guess} using the rules for year 2037, but we can't guarantee accuracy;
    indeed, for \e{any} future date, the time-zone may change its rules before
    that date comes around. For dates before 1970, QDateTime doesn't take DST
    changes into account, even if the system's time zone database provides that
    information, although it does take into account changes to the time-zone's
    standard offset, where this information is available.

    \section2 Offsets From UTC

    There is no explicit size restriction on an offset from UTC, but there is an
    implicit limit imposed when using the toString() and fromString() methods
    which use a [+|-]hh:mm format, effectively limiting the range to +/- 99
    hours and 59 minutes and whole minutes only. Note that currently no time
    zone lies outside the range of +/- 14 hours.

    \sa QDate, QTime, QDateTimeEdit, QTimeZone
*/

/*!
    \since 5.14
    \enum QDateTime::YearRange

    This enumerated type describes the range of years (in the Gregorian
    calendar) representable by QDateTime:

    \value First The later parts of this year are representable
    \value Last The earlier parts of this year are representable

    All dates strictly between these two years are also representable.
    Note, however, that the Gregorian Calendar has no year zero.

    \note QDate can describe dates in a wider range of years.  For most
    purposes, this makes little difference, as the range of years that QDateTime
    can support reaches 292 million years either side of 1970.

    \sa isValid(), QDate
*/

/*!
    Constructs a null datetime (i.e. null date and null time). A null
    datetime is invalid, since the date is invalid.

    \sa isValid()
*/
QDateTime::QDateTime() noexcept(Data::CanBeSmall)
{
}


/*!
    Constructs a datetime with the given \a date, a valid
    time(00:00:00.000), and sets the timeSpec() to Qt::LocalTime.
*/

QDateTime::QDateTime(const QDate &date)
    : d(QDateTimePrivate::create(date, QTime(0, 0), Qt::LocalTime, 0))
{
}

/*!
    Constructs a datetime with the given \a date and \a time, using
    the time specification defined by \a spec.

    If \a date is valid and \a time is not, the time will be set to midnight.

    If \a spec is Qt::OffsetFromUTC then it will be set to Qt::UTC, i.e. an
    offset of 0 seconds. To create a Qt::OffsetFromUTC datetime use the
    correct constructor.

    If \a spec is Qt::TimeZone then the spec will be set to Qt::LocalTime,
    i.e. the current system time zone.  To create a Qt::TimeZone datetime
    use the correct constructor.
*/

QDateTime::QDateTime(const QDate &date, const QTime &time, Qt::TimeSpec spec)
    : d(QDateTimePrivate::create(date, time, spec, 0))
{
}

/*!
    \since 5.2

    Constructs a datetime with the given \a date and \a time, using
    the time specification defined by \a spec and \a offsetSeconds seconds.

    If \a date is valid and \a time is not, the time will be set to midnight.

    If the \a spec is not Qt::OffsetFromUTC then \a offsetSeconds will be ignored.

    If the \a spec is Qt::OffsetFromUTC and \a offsetSeconds is 0 then the
    timeSpec() will be set to Qt::UTC, i.e. an offset of 0 seconds.

    If \a spec is Qt::TimeZone then the spec will be set to Qt::LocalTime,
    i.e. the current system time zone.  To create a Qt::TimeZone datetime
    use the correct constructor.
*/

QDateTime::QDateTime(const QDate &date, const QTime &time, Qt::TimeSpec spec, int offsetSeconds)
         : d(QDateTimePrivate::create(date, time, spec, offsetSeconds))
{
}

#if QT_CONFIG(timezone)
/*!
    \since 5.2

    Constructs a datetime with the given \a date and \a time, using
    the Time Zone specified by \a timeZone.

    If \a date is valid and \a time is not, the time will be set to 00:00:00.

    If \a timeZone is invalid then the datetime will be invalid.
*/

QDateTime::QDateTime(const QDate &date, const QTime &time, const QTimeZone &timeZone)
    : d(QDateTimePrivate::create(date, time, timeZone))
{
}
#endif // timezone

/*!
    Constructs a copy of the \a other datetime.
*/
QDateTime::QDateTime(const QDateTime &other) noexcept
    : d(other.d)
{
}

/*!
    \since 5.8
    Moves the content of the temporary \a other datetime to this object and
    leaves \a other in an unspecified (but proper) state.
*/
QDateTime::QDateTime(QDateTime &&other) noexcept
    : d(std::move(other.d))
{
}

/*!
    Destroys the datetime.
*/
QDateTime::~QDateTime()
{
}

/*!
    Makes a copy of the \a other datetime and returns a reference to the
    copy.
*/

QDateTime &QDateTime::operator=(const QDateTime &other) noexcept
{
    d = other.d;
    return *this;
}
/*!
    \fn void QDateTime::swap(QDateTime &other)
    \since 5.0

    Swaps this datetime with \a other. This operation is very fast
    and never fails.
*/

/*!
    Returns \c true if both the date and the time are null; otherwise
    returns \c false. A null datetime is invalid.

    \sa QDate::isNull(), QTime::isNull(), isValid()
*/

bool QDateTime::isNull() const
{
    auto status = getStatus(d);
    return !status.testFlag(QDateTimePrivate::ValidDate) &&
            !status.testFlag(QDateTimePrivate::ValidTime);
}

/*!
    Returns \c true if both the date and the time are valid and they are valid in
    the current Qt::TimeSpec, otherwise returns \c false.

    If the timeSpec() is Qt::LocalTime or Qt::TimeZone then the date and time are
    checked to see if they fall in the Standard Time to Daylight-Saving Time transition
    hour, i.e. if the transition is at 2am and the clock goes forward to 3am
    then the time from 02:00:00 to 02:59:59.999 is considered to be invalid.

    \sa QDateTime::YearRange, QDate::isValid(), QTime::isValid()
*/

bool QDateTime::isValid() const
{
    auto status = getStatus(d);
    return status & QDateTimePrivate::ValidDateTime;
}

/*!
    Returns the date part of the datetime.

    \sa setDate(), time(), timeSpec()
*/

QDate QDateTime::date() const
{
    auto status = getStatus(d);
    if (!status.testFlag(QDateTimePrivate::ValidDate))
        return QDate();
    QDate dt;
    msecsToTime(getMSecs(d), &dt, nullptr);
    return dt;
}

/*!
    Returns the time part of the datetime.

    \sa setTime(), date(), timeSpec()
*/

QTime QDateTime::time() const
{
    auto status = getStatus(d);
    if (!status.testFlag(QDateTimePrivate::ValidTime))
        return QTime();
    QTime tm;
    msecsToTime(getMSecs(d), nullptr, &tm);
    return tm;
}

/*!
    Returns the time specification of the datetime.

    \sa setTimeSpec(), date(), time(), Qt::TimeSpec
*/

Qt::TimeSpec QDateTime::timeSpec() const
{
    return getSpec(d);
}

#if QT_CONFIG(timezone)
/*!
    \since 5.2

    Returns the time zone of the datetime.

    If the timeSpec() is Qt::LocalTime then an instance of the current system
    time zone will be returned. Note however that if you copy this time zone
    the instance will not remain in sync if the system time zone changes.

    \sa setTimeZone(), Qt::TimeSpec
*/

QTimeZone QDateTime::timeZone() const
{
    switch (getSpec(d)) {
    case Qt::UTC:
        return QTimeZone::utc();
    case Qt::OffsetFromUTC:
        return QTimeZone(d->m_offsetFromUtc);
    case Qt::TimeZone:
        if (d->m_timeZone.isValid())
            return d->m_timeZone;
        break;
    case Qt::LocalTime:
        return QTimeZone::systemTimeZone();
    }
    return QTimeZone();
}
#endif // timezone

/*!
    \since 5.2

    Returns this date-time's Offset From UTC in seconds.

    The result depends on timeSpec():
    \list
    \li \c Qt::UTC The offset is 0.
    \li \c Qt::OffsetFromUTC The offset is the value originally set.
    \li \c Qt::LocalTime The local time's offset from UTC is returned.
    \li \c Qt::TimeZone The offset used by the time-zone is returned.
    \endlist

    For the last two, the offset at this date and time will be returned, taking
    account of Daylight-Saving Offset unless the date precedes the start of
    1970. The offset is the difference between the local time or time in the
    given time-zone and UTC time; it is positive in time-zones ahead of UTC
    (East of The Prime Meridian), negative for those behind UTC (West of The
    Prime Meridian).

    \sa setOffsetFromUtc()
*/

int QDateTime::offsetFromUtc() const
{
    if (!d.isShort())
        return d->m_offsetFromUtc;
    if (!isValid())
        return 0;

    auto spec = getSpec(d);
    if (spec == Qt::LocalTime) {
        // we didn't cache the value, so we need to calculate it now...
        qint64 msecs = getMSecs(d);
        return (msecs - toMSecsSinceEpoch()) / 1000;
    }

    Q_ASSERT(spec == Qt::UTC);
    return 0;
}

/*!
    \since 5.2

    Returns the Time Zone Abbreviation for the datetime.

    If the timeSpec() is Qt::UTC this will be "UTC".

    If the timeSpec() is Qt::OffsetFromUTC this will be in the format
    "UTC[+-]00:00".

    If the timeSpec() is Qt::LocalTime then the host system is queried for the
    correct abbreviation.

    Note that abbreviations may or may not be localized.

    Note too that the abbreviation is not guaranteed to be a unique value,
    i.e. different time zones may have the same abbreviation.

    \sa timeSpec()
*/

QString QDateTime::timeZoneAbbreviation() const
{
    if (!isValid())
        return QString();

    switch (getSpec(d)) {
    case Qt::UTC:
        return QLatin1String("UTC");
    case Qt::OffsetFromUTC:
        return QLatin1String("UTC") + toOffsetString(Qt::ISODate, d->m_offsetFromUtc);
    case Qt::TimeZone:
#if !QT_CONFIG(timezone)
        break;
#else
        Q_ASSERT(d->m_timeZone.isValid());
        return d->m_timeZone.d->abbreviation(toMSecsSinceEpoch());
#endif // timezone
    case Qt::LocalTime:  {
        QString abbrev;
        auto status = extractDaylightStatus(getStatus(d));
        localMSecsToEpochMSecs(getMSecs(d), &status, nullptr, nullptr, &abbrev);
        return abbrev;
        }
    }
    return QString();
}

/*!
    \since 5.2

    Returns if this datetime falls in Daylight-Saving Time.

    If the Qt::TimeSpec is not Qt::LocalTime or Qt::TimeZone then will always
    return false.

    \sa timeSpec()
*/

bool QDateTime::isDaylightTime() const
{
    if (!isValid())
        return false;

    switch (getSpec(d)) {
    case Qt::UTC:
    case Qt::OffsetFromUTC:
        return false;
    case Qt::TimeZone:
#if !QT_CONFIG(timezone)
        break;
#else
        Q_ASSERT(d->m_timeZone.isValid());
        return d->m_timeZone.d->isDaylightTime(toMSecsSinceEpoch());
#endif // timezone
    case Qt::LocalTime: {
        auto status = extractDaylightStatus(getStatus(d));
        if (status == QDateTimePrivate::UnknownDaylightTime)
            localMSecsToEpochMSecs(getMSecs(d), &status);
        return (status == QDateTimePrivate::DaylightTime);
        }
    }
    return false;
}

/*!
    Sets the date part of this datetime to \a date. If no time is set yet, it
    is set to midnight. If \a date is invalid, this QDateTime becomes invalid.

    \sa date(), setTime(), setTimeSpec()
*/

void QDateTime::setDate(const QDate &date)
{
    setDateTime(d, date, time());
}

/*!
    Sets the time part of this datetime to \a time. If \a time is not valid,
    this function sets it to midnight. Therefore, it's possible to clear any
    set time in a QDateTime by setting it to a default QTime:

    \code
        QDateTime dt = QDateTime::currentDateTime();
        dt.setTime(QTime());
    \endcode

    \sa time(), setDate(), setTimeSpec()
*/

void QDateTime::setTime(const QTime &time)
{
    setDateTime(d, date(), time);
}

/*!
    Sets the time specification used in this datetime to \a spec.
    The datetime will refer to a different point in time.

    If \a spec is Qt::OffsetFromUTC then the timeSpec() will be set
    to Qt::UTC, i.e. an effective offset of 0.

    If \a spec is Qt::TimeZone then the spec will be set to Qt::LocalTime,
    i.e. the current system time zone.

    Example:
    \snippet code/src_corelib_tools_qdatetime.cpp 19

    \sa timeSpec(), setDate(), setTime(), setTimeZone(), Qt::TimeSpec
*/

void QDateTime::setTimeSpec(Qt::TimeSpec spec)
{
    QT_PREPEND_NAMESPACE(setTimeSpec(d, spec, 0));
    checkValidDateTime(d);
}

/*!
    \since 5.2

    Sets the timeSpec() to Qt::OffsetFromUTC and the offset to \a offsetSeconds.
    The datetime will refer to a different point in time.

    The maximum and minimum offset is 14 positive or negative hours.  If
    \a offsetSeconds is larger or smaller than that, then the result is
    undefined.

    If \a offsetSeconds is 0 then the timeSpec() will be set to Qt::UTC.

    \sa isValid(), offsetFromUtc()
*/

void QDateTime::setOffsetFromUtc(int offsetSeconds)
{
    QT_PREPEND_NAMESPACE(setTimeSpec(d, Qt::OffsetFromUTC, offsetSeconds));
    checkValidDateTime(d);
}

#if QT_CONFIG(timezone)
/*!
    \since 5.2

    Sets the time zone used in this datetime to \a toZone.
    The datetime will refer to a different point in time.

    If \a toZone is invalid then the datetime will be invalid.

    \sa timeZone(), Qt::TimeSpec
*/

void QDateTime::setTimeZone(const QTimeZone &toZone)
{
    d.detach();         // always detach
    d->m_status = mergeSpec(d->m_status, Qt::TimeZone);
    d->m_offsetFromUtc = 0;
    d->m_timeZone = toZone;
    refreshDateTime(d);
}
#endif // timezone

/*!
    \since 4.7

    Returns the datetime as the number of milliseconds that have passed
    since 1970-01-01T00:00:00.000, Coordinated Universal Time (Qt::UTC).

    On systems that do not support time zones, this function will
    behave as if local time were Qt::UTC.

    The behavior for this function is undefined if the datetime stored in
    this object is not valid. However, for all valid dates, this function
    returns a unique value.

    \sa toSecsSinceEpoch(), setMSecsSinceEpoch()
*/
qint64 QDateTime::toMSecsSinceEpoch() const
{
    // Note: QDateTimeParser relies on this producing a useful result, even when
    // !isValid(), at least when the invalidity is a time in a fall-back (that
    // we'll have adjusted to lie outside it, but marked invalid because it's
    // not what was asked for). Other things may be doing similar.
    switch (getSpec(d)) {
    case Qt::UTC:
        return getMSecs(d);

    case Qt::OffsetFromUTC:
        return d->m_msecs - (d->m_offsetFromUtc * 1000);

    case Qt::LocalTime: {
        // recalculate the local timezone
        auto status = extractDaylightStatus(getStatus(d));
        return localMSecsToEpochMSecs(getMSecs(d), &status);
    }

    case Qt::TimeZone:
#if QT_CONFIG(timezone)
        if (d->m_timeZone.isValid()) {
            return QDateTimePrivate::zoneMSecsToEpochMSecs(d->m_msecs, d->m_timeZone,
                                                           extractDaylightStatus(getStatus(d)));
        }
#endif
        return 0;
    }
    Q_UNREACHABLE();
    return 0;
}

/*!
    \since 5.8

    Returns the datetime as the number of seconds that have passed since
    1970-01-01T00:00:00.000, Coordinated Universal Time (Qt::UTC).

    On systems that do not support time zones, this function will
    behave as if local time were Qt::UTC.

    The behavior for this function is undefined if the datetime stored in
    this object is not valid. However, for all valid dates, this function
    returns a unique value.

    \sa toMSecsSinceEpoch(), setSecsSinceEpoch()
*/
qint64 QDateTime::toSecsSinceEpoch() const
{
    return toMSecsSinceEpoch() / 1000;
}

#if QT_DEPRECATED_SINCE(5, 8)
/*!
    \deprecated

    Returns the datetime as the number of seconds that have passed
    since 1970-01-01T00:00:00, Coordinated Universal Time (Qt::UTC).

    On systems that do not support time zones, this function will
    behave as if local time were Qt::UTC.

    \note This function returns a 32-bit unsigned integer and is deprecated.

    If the date is outside the range 1970-01-01T00:00:00 to
    2106-02-07T06:28:14, this function returns -1 cast to an unsigned integer
    (i.e., 0xFFFFFFFF).

    To get an extended range, use toMSecsSinceEpoch() or toSecsSinceEpoch().

    \sa toSecsSinceEpoch(), toMSecsSinceEpoch(), setTime_t()
*/

uint QDateTime::toTime_t() const
{
    if (!isValid())
        return uint(-1);
    qint64 retval = toMSecsSinceEpoch() / 1000;
    if (quint64(retval) >= Q_UINT64_C(0xFFFFFFFF))
        return uint(-1);
    return uint(retval);
}
#endif

/*!
    \since 4.7

    Sets the date and time given the number of milliseconds \a msecs that have
    passed since 1970-01-01T00:00:00.000, Coordinated Universal Time
    (Qt::UTC). On systems that do not support time zones this function
    will behave as if local time were Qt::UTC.

    Note that passing the minimum of \c qint64
    (\c{std::numeric_limits<qint64>::min()}) to \a msecs will result in
    undefined behavior.

    \sa toMSecsSinceEpoch(), setSecsSinceEpoch()
*/
void QDateTime::setMSecsSinceEpoch(qint64 msecs)
{
    const auto spec = getSpec(d);
    auto status = getStatus(d);

    status &= ~QDateTimePrivate::ValidityMask;
    switch (spec) {
    case Qt::UTC:
        status = status
                    | QDateTimePrivate::ValidDate
                    | QDateTimePrivate::ValidTime
                    | QDateTimePrivate::ValidDateTime;
        break;
    case Qt::OffsetFromUTC:
        msecs = msecs + (d->m_offsetFromUtc * 1000);
        status = status
                    | QDateTimePrivate::ValidDate
                    | QDateTimePrivate::ValidTime
                    | QDateTimePrivate::ValidDateTime;
        break;
    case Qt::TimeZone:
        Q_ASSERT(!d.isShort());
#if QT_CONFIG(timezone)
        d.detach();
        if (!d->m_timeZone.isValid())
            break;
        // Docs state any LocalTime before 1970-01-01 will *not* have any DST applied
        // but all affected times afterwards will have DST applied.
        if (msecs >= 0) {
            status = mergeDaylightStatus(status,
                                         d->m_timeZone.d->isDaylightTime(msecs)
                                         ? QDateTimePrivate::DaylightTime
                                         : QDateTimePrivate::StandardTime);
            d->m_offsetFromUtc = d->m_timeZone.d->offsetFromUtc(msecs);
        } else {
            status = mergeDaylightStatus(status, QDateTimePrivate::StandardTime);
            d->m_offsetFromUtc = d->m_timeZone.d->standardTimeOffset(msecs);
        }
        msecs = msecs + (d->m_offsetFromUtc * 1000);
        status = status
                    | QDateTimePrivate::ValidDate
                    | QDateTimePrivate::ValidTime
                    | QDateTimePrivate::ValidDateTime;
#endif // timezone
        break;
    case Qt::LocalTime: {
        QDate dt;
        QTime tm;
        QDateTimePrivate::DaylightStatus dstStatus;
        epochMSecsToLocalTime(msecs, &dt, &tm, &dstStatus);
        setDateTime(d, dt, tm);
        msecs = getMSecs(d);
        status = mergeDaylightStatus(getStatus(d), dstStatus);
        break;
        }
    }

    if (msecsCanBeSmall(msecs) && d.isShort()) {
        // we can keep short
        d.data.msecs = qintptr(msecs);
        d.data.status = status;
    } else {
        d.detach();
        d->m_status = status & ~QDateTimePrivate::ShortData;
        d->m_msecs = msecs;
    }

    if (spec == Qt::LocalTime || spec == Qt::TimeZone)
        refreshDateTime(d);
}

/*!
    \since 5.8

    Sets the date and time given the number of seconds \a secs that have
    passed since 1970-01-01T00:00:00.000, Coordinated Universal Time
    (Qt::UTC). On systems that do not support time zones this function
    will behave as if local time were Qt::UTC.

    \sa toSecsSinceEpoch(), setMSecsSinceEpoch()
*/
void QDateTime::setSecsSinceEpoch(qint64 secs)
{
    setMSecsSinceEpoch(secs * 1000);
}

#if QT_DEPRECATED_SINCE(5, 8)
/*!
    \fn void QDateTime::setTime_t(uint seconds)
    \deprecated

    Sets the date and time given the number of \a seconds that have
    passed since 1970-01-01T00:00:00, Coordinated Universal Time
    (Qt::UTC). On systems that do not support time zones this function
    will behave as if local time were Qt::UTC.

    \note This function is deprecated. For new code, use setSecsSinceEpoch().

    \sa toTime_t()
*/

void QDateTime::setTime_t(uint secsSince1Jan1970UTC)
{
    setMSecsSinceEpoch((qint64)secsSince1Jan1970UTC * 1000);
}
#endif

#if QT_CONFIG(datestring) // depends on, so implies, textdate
/*!
    \fn QString QDateTime::toString(Qt::DateFormat format) const

    \overload

    Returns the datetime as a string in the \a format given.

    If the \a format is Qt::TextDate, the string is formatted in the default
    way. The day and month names will be localized names using the system
    locale, i.e. QLocale::system(). An example of this formatting is "Wed May 20
    03:40:13 1998".

    If the \a format is Qt::ISODate, the string format corresponds
    to the ISO 8601 extended specification for representations of
    dates and times, taking the form yyyy-MM-ddTHH:mm:ss[Z|[+|-]HH:mm],
    depending on the timeSpec() of the QDateTime. If the timeSpec()
    is Qt::UTC, Z will be appended to the string; if the timeSpec() is
    Qt::OffsetFromUTC, the offset in hours and minutes from UTC will
    be appended to the string. To include milliseconds in the ISO 8601
    date, use the \a format Qt::ISODateWithMs, which corresponds to
    yyyy-MM-ddTHH:mm:ss.zzz[Z|[+|-]HH:mm].

    If the \a format is Qt::SystemLocaleShortDate or
    Qt::SystemLocaleLongDate, the string format depends on the locale
    settings of the system. Identical to calling
    QLocale::system().toString(datetime, QLocale::ShortFormat) or
    QLocale::system().toString(datetime, QLocale::LongFormat).

    If the \a format is Qt::DefaultLocaleShortDate or
    Qt::DefaultLocaleLongDate, the string format depends on the
    default application locale. This is the locale set with
    QLocale::setDefault(), or the system locale if no default locale
    has been set. Identical to calling QLocale().toString(datetime,
    QLocale::ShortFormat) or QLocale().toString(datetime,
    QLocale::LongFormat).

    If the \a format is Qt::RFC2822Date, the string is formatted
    following \l{RFC 2822}.

    If the datetime is invalid, an empty string will be returned.

    \warning The Qt::ISODate format is only valid for years in the
    range 0 to 9999. This restriction may apply to locale-aware
    formats as well, depending on the locale settings.

    \sa fromString(), QDate::toString(), QTime::toString(),
    QLocale::toString()
*/

QString QDateTime::toString(Qt::DateFormat format) const
{
    QString buf;
    if (!isValid())
        return buf;

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toString(*this, QLocale::ShortFormat);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toString(*this, QLocale::LongFormat);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toString(*this, QLocale::ShortFormat);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toString(*this, QLocale::LongFormat);
    case Qt::RFC2822Date: {
        buf = QLocale::c().toString(*this, u"dd MMM yyyy hh:mm:ss ");
        buf += toOffsetString(Qt::TextDate, offsetFromUtc());
        return buf;
    }
    default:
    case Qt::TextDate: {
        const QPair<QDate, QTime> p = getDateTime(d);
        buf = p.first.toString(Qt::TextDate);
        // Insert time between date's day and year:
        buf.insert(buf.lastIndexOf(QLatin1Char(' ')),
                   QLatin1Char(' ') + p.second.toString(Qt::TextDate));
        // Append zone/offset indicator, as appropriate:
        switch (timeSpec()) {
        case Qt::LocalTime:
            break;
#if QT_CONFIG(timezone)
        case Qt::TimeZone:
            buf += QLatin1Char(' ') + d->m_timeZone.abbreviation(*this);
            break;
#endif
        default:
            buf += QLatin1String(" GMT");
            if (getSpec(d) == Qt::OffsetFromUTC)
                buf += toOffsetString(Qt::TextDate, offsetFromUtc());
        }
        return buf;
    }
    case Qt::ISODate:
    case Qt::ISODateWithMs: {
        const QPair<QDate, QTime> p = getDateTime(d);
        const QDate &dt = p.first;
        const QTime &tm = p.second;
        buf = dt.toString(Qt::ISODate);
        if (buf.isEmpty())
            return QString();   // failed to convert
        buf += QLatin1Char('T');
        buf += tm.toString(format);
        switch (getSpec(d)) {
        case Qt::UTC:
            buf += QLatin1Char('Z');
            break;
        case Qt::OffsetFromUTC:
#if QT_CONFIG(timezone)
        case Qt::TimeZone:
#endif
            buf += toOffsetString(Qt::ISODate, offsetFromUtc());
            break;
        default:
            break;
        }
        return buf;
    }
    }
}

/*!
    \fn QString QDateTime::toString(const QString &format) const
    \fn QString QDateTime::toString(QStringView format) const

    Returns the datetime as a string. The \a format parameter determines the
    format of the result string. See QTime::toString() and QDate::toString() for
    the supported specifiers for time and date, respectively.

    Any sequence of characters enclosed in single quotes will be included
    verbatim in the output string (stripped of the quotes), even if it contains
    formatting characters. Two consecutive single quotes ("''") are replaced by
    a single quote in the output. All other characters in the format string are
    included verbatim in the output string.

    Formats without separators (e.g. "ddMM") are supported but must be used with
    care, as the resulting strings aren't always reliably readable (e.g. if "dM"
    produces "212" it could mean either the 2nd of December or the 21st of
    February).

    Example format strings (assumed that the QDateTime is 21 May 2001
    14:13:09.120):

    \table
    \header \li Format       \li Result
    \row \li dd.MM.yyyy      \li 21.05.2001
    \row \li ddd MMMM d yy   \li Tue May 21 01
    \row \li hh:mm:ss.zzz    \li 14:13:09.120
    \row \li hh:mm:ss.z      \li 14:13:09.12
    \row \li h:m:s ap        \li 2:13:9 pm
    \endtable

    If the datetime is invalid, an empty string will be returned.

    \sa fromString(), QDate::toString(), QTime::toString(), QLocale::toString()
*/
QString QDateTime::toString(QStringView format) const
{
    return QLocale::system().toString(*this, format); // QLocale::c() ### Qt6
}

#if QT_STRINGVIEW_LEVEL < 2
QString QDateTime::toString(const QString &format) const
{
    return toString(qToStringViewIgnoringNull(format));
}
#endif

#endif // datestring

static inline void massageAdjustedDateTime(const QDateTimeData &d, QDate *date, QTime *time)
{
    /*
      If we have just adjusted to a day with a DST transition, our given time
      may lie in the transition hour (either missing or duplicated).  For any
      other time, telling mktime (deep in the bowels of localMSecsToEpochMSecs)
      we don't know its DST-ness will produce no adjustment (just a decision as
      to its DST-ness); but for a time in spring's missing hour it'll adjust the
      time while picking a DST-ness.  (Handling of autumn is trickier, as either
      DST-ness is valid, without adjusting the time.  We might want to propagate
      the daylight status in that case, but it's hard to do so without breaking
      (far more common) other cases; and it makes little difference, as the two
      answers do then differ only in DST-ness.)
    */
    auto spec = getSpec(d);
    if (spec == Qt::LocalTime) {
        QDateTimePrivate::DaylightStatus status = QDateTimePrivate::UnknownDaylightTime;
        localMSecsToEpochMSecs(timeToMSecs(*date, *time), &status, date, time);
#if QT_CONFIG(timezone)
    } else if (spec == Qt::TimeZone && d->m_timeZone.isValid()) {
        QDateTimePrivate::zoneMSecsToEpochMSecs(timeToMSecs(*date, *time),
                                                d->m_timeZone,
                                                QDateTimePrivate::UnknownDaylightTime,
                                                date, time);
#endif // timezone
    }
}

/*!
    Returns a QDateTime object containing a datetime \a ndays days
    later than the datetime of this object (or earlier if \a ndays is
    negative).

    If the timeSpec() is Qt::LocalTime and the resulting
    date and time fall in the Standard Time to Daylight-Saving Time transition
    hour then the result will be adjusted accordingly, i.e. if the transition
    is at 2am and the clock goes forward to 3am and the result falls between
    2am and 3am then the result will be adjusted to fall after 3am.

    \sa daysTo(), addMonths(), addYears(), addSecs()
*/

QDateTime QDateTime::addDays(qint64 ndays) const
{
    QDateTime dt(*this);
    QPair<QDate, QTime> p = getDateTime(d);
    QDate &date = p.first;
    QTime &time = p.second;
    date = date.addDays(ndays);
    massageAdjustedDateTime(dt.d, &date, &time);
    setDateTime(dt.d, date, time);
    return dt;
}

/*!
    Returns a QDateTime object containing a datetime \a nmonths months
    later than the datetime of this object (or earlier if \a nmonths
    is negative).

    If the timeSpec() is Qt::LocalTime and the resulting
    date and time fall in the Standard Time to Daylight-Saving Time transition
    hour then the result will be adjusted accordingly, i.e. if the transition
    is at 2am and the clock goes forward to 3am and the result falls between
    2am and 3am then the result will be adjusted to fall after 3am.

    \sa daysTo(), addDays(), addYears(), addSecs()
*/

QDateTime QDateTime::addMonths(int nmonths) const
{
    QDateTime dt(*this);
    QPair<QDate, QTime> p = getDateTime(d);
    QDate &date = p.first;
    QTime &time = p.second;
    date = date.addMonths(nmonths);
    massageAdjustedDateTime(dt.d, &date, &time);
    setDateTime(dt.d, date, time);
    return dt;
}

/*!
    Returns a QDateTime object containing a datetime \a nyears years
    later than the datetime of this object (or earlier if \a nyears is
    negative).

    If the timeSpec() is Qt::LocalTime and the resulting
    date and time fall in the Standard Time to Daylight-Saving Time transition
    hour then the result will be adjusted accordingly, i.e. if the transition
    is at 2am and the clock goes forward to 3am and the result falls between
    2am and 3am then the result will be adjusted to fall after 3am.

    \sa daysTo(), addDays(), addMonths(), addSecs()
*/

QDateTime QDateTime::addYears(int nyears) const
{
    QDateTime dt(*this);
    QPair<QDate, QTime> p = getDateTime(d);
    QDate &date = p.first;
    QTime &time = p.second;
    date = date.addYears(nyears);
    massageAdjustedDateTime(dt.d, &date, &time);
    setDateTime(dt.d, date, time);
    return dt;
}

/*!
    Returns a QDateTime object containing a datetime \a s seconds
    later than the datetime of this object (or earlier if \a s is
    negative).

    If this datetime is invalid, an invalid datetime will be returned.

    \sa addMSecs(), secsTo(), addDays(), addMonths(), addYears()
*/

QDateTime QDateTime::addSecs(qint64 s) const
{
    return addMSecs(s * 1000);
}

/*!
    Returns a QDateTime object containing a datetime \a msecs miliseconds
    later than the datetime of this object (or earlier if \a msecs is
    negative).

    If this datetime is invalid, an invalid datetime will be returned.

    \sa addSecs(), msecsTo(), addDays(), addMonths(), addYears()
*/
QDateTime QDateTime::addMSecs(qint64 msecs) const
{
    if (!isValid())
        return QDateTime();

    QDateTime dt(*this);
    auto spec = getSpec(d);
    if (spec == Qt::LocalTime || spec == Qt::TimeZone) {
        // Convert to real UTC first in case crosses DST transition
        dt.setMSecsSinceEpoch(toMSecsSinceEpoch() + msecs);
    } else {
        // No need to convert, just add on
        if (d.isShort()) {
            // need to check if we need to enlarge first
            msecs += dt.d.data.msecs;
            if (msecsCanBeSmall(msecs)) {
                dt.d.data.msecs = qintptr(msecs);
            } else {
                dt.d.detach();
                dt.d->m_msecs = msecs;
            }
        } else {
            dt.d.detach();
            dt.d->m_msecs += msecs;
        }
    }
    return dt;
}

/*!
    Returns the number of days from this datetime to the \a other
    datetime. The number of days is counted as the number of times
    midnight is reached between this datetime to the \a other
    datetime. This means that a 10 minute difference from 23:55 to
    0:05 the next day counts as one day.

    If the \a other datetime is earlier than this datetime,
    the value returned is negative.

    Example:
    \snippet code/src_corelib_tools_qdatetime.cpp 15

    \sa addDays(), secsTo(), msecsTo()
*/

qint64 QDateTime::daysTo(const QDateTime &other) const
{
    return date().daysTo(other.date());
}

/*!
    Returns the number of seconds from this datetime to the \a other
    datetime. If the \a other datetime is earlier than this datetime,
    the value returned is negative.

    Before performing the comparison, the two datetimes are converted
    to Qt::UTC to ensure that the result is correct if daylight-saving
    (DST) applies to one of the two datetimes but not the other.

    Returns 0 if either datetime is invalid.

    Example:
    \snippet code/src_corelib_tools_qdatetime.cpp 11

    \sa addSecs(), daysTo(), QTime::secsTo()
*/

qint64 QDateTime::secsTo(const QDateTime &other) const
{
    return (msecsTo(other) / 1000);
}

/*!
    Returns the number of milliseconds from this datetime to the \a other
    datetime. If the \a other datetime is earlier than this datetime,
    the value returned is negative.

    Before performing the comparison, the two datetimes are converted
    to Qt::UTC to ensure that the result is correct if daylight-saving
    (DST) applies to one of the two datetimes and but not the other.

    Returns 0 if either datetime is invalid.

    \sa addMSecs(), daysTo(), QTime::msecsTo()
*/

qint64 QDateTime::msecsTo(const QDateTime &other) const
{
    if (!isValid() || !other.isValid())
        return 0;

    return other.toMSecsSinceEpoch() - toMSecsSinceEpoch();
}

/*!
    \fn QDateTime QDateTime::toTimeSpec(Qt::TimeSpec spec) const

    Returns a copy of this datetime converted to the given time
    \a spec.

    If \a spec is Qt::OffsetFromUTC then it is set to Qt::UTC.  To set to a
    spec of Qt::OffsetFromUTC use toOffsetFromUtc().

    If \a spec is Qt::TimeZone then it is set to Qt::LocalTime,
    i.e. the local Time Zone.

    Example:
    \snippet code/src_corelib_tools_qdatetime.cpp 16

    \sa timeSpec(), toTimeZone(), toOffsetFromUtc()
*/

QDateTime QDateTime::toTimeSpec(Qt::TimeSpec spec) const
{
    if (getSpec(d) == spec && (spec == Qt::UTC || spec == Qt::LocalTime))
        return *this;

    if (!isValid()) {
        QDateTime ret = *this;
        ret.setTimeSpec(spec);
        return ret;
    }

    return fromMSecsSinceEpoch(toMSecsSinceEpoch(), spec, 0);
}

/*!
    \since 5.2

    \fn QDateTime QDateTime::toOffsetFromUtc(int offsetSeconds) const

    Returns a copy of this datetime converted to a spec of Qt::OffsetFromUTC
    with the given \a offsetSeconds.

    If the \a offsetSeconds equals 0 then a UTC datetime will be returned

    \sa setOffsetFromUtc(), offsetFromUtc(), toTimeSpec()
*/

QDateTime QDateTime::toOffsetFromUtc(int offsetSeconds) const
{
    if (getSpec(d) == Qt::OffsetFromUTC
            && d->m_offsetFromUtc == offsetSeconds)
        return *this;

    if (!isValid()) {
        QDateTime ret = *this;
        ret.setOffsetFromUtc(offsetSeconds);
        return ret;
    }

    return fromMSecsSinceEpoch(toMSecsSinceEpoch(), Qt::OffsetFromUTC, offsetSeconds);
}

#if QT_CONFIG(timezone)
/*!
    \since 5.2

    Returns a copy of this datetime converted to the given \a timeZone

    \sa timeZone(), toTimeSpec()
*/

QDateTime QDateTime::toTimeZone(const QTimeZone &timeZone) const
{
    if (getSpec(d) == Qt::TimeZone && d->m_timeZone == timeZone)
        return *this;

    if (!isValid()) {
        QDateTime ret = *this;
        ret.setTimeZone(timeZone);
        return ret;
    }

    return fromMSecsSinceEpoch(toMSecsSinceEpoch(), timeZone);
}
#endif // timezone

/*!
    Returns \c true if this datetime is equal to the \a other datetime;
    otherwise returns \c false.

    Since 5.14, all invalid datetimes are equal to one another and differ from
    all other datetimes.

    \sa operator!=()
*/

bool QDateTime::operator==(const QDateTime &other) const
{
    if (!isValid())
        return !other.isValid();
    if (!other.isValid())
        return false;

    if (getSpec(d) == Qt::LocalTime && getStatus(d) == getStatus(other.d))
        return getMSecs(d) == getMSecs(other.d);

    // Convert to UTC and compare
    return toMSecsSinceEpoch() == other.toMSecsSinceEpoch();
}

/*!
    \fn bool QDateTime::operator!=(const QDateTime &other) const

    Returns \c true if this datetime is different from the \a other
    datetime; otherwise returns \c false.

    Two datetimes are different if either the date, the time, or the time zone
    components are different. Since 5.14, any invalid datetime is less than all
    valid datetimes.

    \sa operator==()
*/

/*!
    Returns \c true if this datetime is earlier than the \a other
    datetime; otherwise returns \c false.
*/

bool QDateTime::operator<(const QDateTime &other) const
{
    if (!isValid())
        return other.isValid();
    if (!other.isValid())
        return false;

    if (getSpec(d) == Qt::LocalTime && getStatus(d) == getStatus(other.d))
        return getMSecs(d) < getMSecs(other.d);

    // Convert to UTC and compare
    return toMSecsSinceEpoch() < other.toMSecsSinceEpoch();
}

/*!
    \fn bool QDateTime::operator<=(const QDateTime &other) const

    Returns \c true if this datetime is earlier than or equal to the
    \a other datetime; otherwise returns \c false.
*/

/*!
    \fn bool QDateTime::operator>(const QDateTime &other) const

    Returns \c true if this datetime is later than the \a other datetime;
    otherwise returns \c false.
*/

/*!
    \fn bool QDateTime::operator>=(const QDateTime &other) const

    Returns \c true if this datetime is later than or equal to the
    \a other datetime; otherwise returns \c false.
*/

/*!
    \fn QDateTime QDateTime::currentDateTime()
    Returns the current datetime, as reported by the system clock, in
    the local time zone.

    \sa currentDateTimeUtc(), QDate::currentDate(), QTime::currentTime(), toTimeSpec()
*/

/*!
    \fn QDateTime QDateTime::currentDateTimeUtc()
    \since 4.7
    Returns the current datetime, as reported by the system clock, in
    UTC.

    \sa currentDateTime(), QDate::currentDate(), QTime::currentTime(), toTimeSpec()
*/

/*!
    \fn qint64 QDateTime::currentMSecsSinceEpoch()
    \since 4.7

    Returns the number of milliseconds since 1970-01-01T00:00:00 Universal
    Coordinated Time. This number is like the POSIX time_t variable, but
    expressed in milliseconds instead.

    \sa currentDateTime(), currentDateTimeUtc(), toTime_t(), toTimeSpec()
*/

/*!
    \fn qint64 QDateTime::currentSecsSinceEpoch()
    \since 5.8

    Returns the number of seconds since 1970-01-01T00:00:00 Universal
    Coordinated Time.

    \sa currentMSecsSinceEpoch()
*/

#if defined(Q_OS_WIN)
static inline uint msecsFromDecomposed(int hour, int minute, int sec, int msec = 0)
{
    return MSECS_PER_HOUR * hour + MSECS_PER_MIN * minute + 1000 * sec + msec;
}

QDate QDate::currentDate()
{
    SYSTEMTIME st;
    memset(&st, 0, sizeof(SYSTEMTIME));
    GetLocalTime(&st);
    return QDate(st.wYear, st.wMonth, st.wDay);
}

QTime QTime::currentTime()
{
    QTime ct;
    SYSTEMTIME st;
    memset(&st, 0, sizeof(SYSTEMTIME));
    GetLocalTime(&st);
    ct.setHMS(st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return ct;
}

QDateTime QDateTime::currentDateTime()
{
    QTime t;
    SYSTEMTIME st;
    memset(&st, 0, sizeof(SYSTEMTIME));
    GetLocalTime(&st);
    QDate d(st.wYear, st.wMonth, st.wDay);
    t.mds = msecsFromDecomposed(st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return QDateTime(d, t);
}

QDateTime QDateTime::currentDateTimeUtc()
{
    QTime t;
    SYSTEMTIME st;
    memset(&st, 0, sizeof(SYSTEMTIME));
    GetSystemTime(&st);
    QDate d(st.wYear, st.wMonth, st.wDay);
    t.mds = msecsFromDecomposed(st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return QDateTime(d, t, Qt::UTC);
}

qint64 QDateTime::currentMSecsSinceEpoch() noexcept
{
    SYSTEMTIME st;
    memset(&st, 0, sizeof(SYSTEMTIME));
    GetSystemTime(&st);
    const qint64 daysAfterEpoch = QDate(1970, 1, 1).daysTo(QDate(st.wYear, st.wMonth, st.wDay));

    return msecsFromDecomposed(st.wHour, st.wMinute, st.wSecond, st.wMilliseconds) +
           daysAfterEpoch * Q_INT64_C(86400000);
}

qint64 QDateTime::currentSecsSinceEpoch() noexcept
{
    SYSTEMTIME st;
    memset(&st, 0, sizeof(SYSTEMTIME));
    GetSystemTime(&st);
    const qint64 daysAfterEpoch = QDate(1970, 1, 1).daysTo(QDate(st.wYear, st.wMonth, st.wDay));

    return st.wHour * SECS_PER_HOUR + st.wMinute * SECS_PER_MIN + st.wSecond +
           daysAfterEpoch * Q_INT64_C(86400);
}

#elif defined(Q_OS_UNIX)
QDate QDate::currentDate()
{
    return QDateTime::currentDateTime().date();
}

QTime QTime::currentTime()
{
    return QDateTime::currentDateTime().time();
}

QDateTime QDateTime::currentDateTime()
{
    return fromMSecsSinceEpoch(currentMSecsSinceEpoch(), Qt::LocalTime);
}

QDateTime QDateTime::currentDateTimeUtc()
{
    return fromMSecsSinceEpoch(currentMSecsSinceEpoch(), Qt::UTC);
}

qint64 QDateTime::currentMSecsSinceEpoch() noexcept
{
    // posix compliant system
    // we have milliseconds
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return qint64(tv.tv_sec) * Q_INT64_C(1000) + tv.tv_usec / 1000;
}

qint64 QDateTime::currentSecsSinceEpoch() noexcept
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return qint64(tv.tv_sec);
}
#else
#error "What system is this?"
#endif

#if QT_DEPRECATED_SINCE(5, 8)
/*!
  \since 4.2
  \deprecated

  Returns a datetime whose date and time are the number of \a seconds
  that have passed since 1970-01-01T00:00:00, Coordinated Universal
  Time (Qt::UTC) and converted to Qt::LocalTime.  On systems that do not
  support time zones, the time will be set as if local time were Qt::UTC.

  \note This function is deprecated. Please use fromSecsSinceEpoch() in new
  code.

  \sa toTime_t(), setTime_t()
*/
QDateTime QDateTime::fromTime_t(uint seconds)
{
    return fromMSecsSinceEpoch((qint64)seconds * 1000, Qt::LocalTime);
}

/*!
  \since 5.2
  \deprecated

  Returns a datetime whose date and time are the number of \a seconds
  that have passed since 1970-01-01T00:00:00, Coordinated Universal
  Time (Qt::UTC) and converted to the given \a spec.

  If the \a spec is not Qt::OffsetFromUTC then the \a offsetSeconds will be
  ignored.  If the \a spec is Qt::OffsetFromUTC and the \a offsetSeconds is 0
  then the spec will be set to Qt::UTC, i.e. an offset of 0 seconds.

  \note This function is deprecated. Please use fromSecsSinceEpoch() in new
  code.

  \sa toTime_t(), setTime_t()
*/
QDateTime QDateTime::fromTime_t(uint seconds, Qt::TimeSpec spec, int offsetSeconds)
{
    return fromMSecsSinceEpoch((qint64)seconds * 1000, spec, offsetSeconds);
}

#if QT_CONFIG(timezone)
/*!
    \since 5.2
    \deprecated

    Returns a datetime whose date and time are the number of \a seconds
    that have passed since 1970-01-01T00:00:00, Coordinated Universal
    Time (Qt::UTC) and with the given \a timeZone.

    \note This function is deprecated. Please use fromSecsSinceEpoch() in new
    code.

    \sa toTime_t(), setTime_t()
*/
QDateTime QDateTime::fromTime_t(uint seconds, const QTimeZone &timeZone)
{
    return fromMSecsSinceEpoch((qint64)seconds * 1000, timeZone);
}
#endif
#endif // QT_DEPRECATED_SINCE(5, 8)

/*!
  \since 4.7

  Returns a datetime whose date and time are the number of milliseconds, \a msecs,
  that have passed since 1970-01-01T00:00:00.000, Coordinated Universal
  Time (Qt::UTC), and converted to Qt::LocalTime.  On systems that do not
  support time zones, the time will be set as if local time were Qt::UTC.

  Note that there are possible values for \a msecs that lie outside the valid
  range of QDateTime, both negative and positive. The behavior of this
  function is undefined for those values.

  \sa toMSecsSinceEpoch(), setMSecsSinceEpoch()
*/
QDateTime QDateTime::fromMSecsSinceEpoch(qint64 msecs)
{
    return fromMSecsSinceEpoch(msecs, Qt::LocalTime);
}

/*!
  \since 5.2

  Returns a datetime whose date and time are the number of milliseconds \a msecs
  that have passed since 1970-01-01T00:00:00.000, Coordinated Universal
  Time (Qt::UTC) and converted to the given \a spec.

  Note that there are possible values for \a msecs that lie outside the valid
  range of QDateTime, both negative and positive. The behavior of this
  function is undefined for those values.

  If the \a spec is not Qt::OffsetFromUTC then the \a offsetSeconds will be
  ignored.  If the \a spec is Qt::OffsetFromUTC and the \a offsetSeconds is 0
  then the spec will be set to Qt::UTC, i.e. an offset of 0 seconds.

  If \a spec is Qt::TimeZone then the spec will be set to Qt::LocalTime,
  i.e. the current system time zone.

  \sa toMSecsSinceEpoch(), setMSecsSinceEpoch()
*/
QDateTime QDateTime::fromMSecsSinceEpoch(qint64 msecs, Qt::TimeSpec spec, int offsetSeconds)
{
    QDateTime dt;
    QT_PREPEND_NAMESPACE(setTimeSpec(dt.d, spec, offsetSeconds));
    dt.setMSecsSinceEpoch(msecs);
    return dt;
}

/*!
  \since 5.8

  Returns a datetime whose date and time are the number of seconds \a secs
  that have passed since 1970-01-01T00:00:00.000, Coordinated Universal
  Time (Qt::UTC) and converted to the given \a spec.

  Note that there are possible values for \a secs that lie outside the valid
  range of QDateTime, both negative and positive. The behavior of this
  function is undefined for those values.

  If the \a spec is not Qt::OffsetFromUTC then the \a offsetSeconds will be
  ignored.  If the \a spec is Qt::OffsetFromUTC and the \a offsetSeconds is 0
  then the spec will be set to Qt::UTC, i.e. an offset of 0 seconds.

  If \a spec is Qt::TimeZone then the spec will be set to Qt::LocalTime,
  i.e. the current system time zone.

  \sa toSecsSinceEpoch(), setSecsSinceEpoch()
*/
QDateTime QDateTime::fromSecsSinceEpoch(qint64 secs, Qt::TimeSpec spec, int offsetSeconds)
{
    return fromMSecsSinceEpoch(secs * 1000, spec, offsetSeconds);
}

#if QT_CONFIG(timezone)
/*!
    \since 5.2

    Returns a datetime whose date and time are the number of milliseconds \a msecs
    that have passed since 1970-01-01T00:00:00.000, Coordinated Universal
    Time (Qt::UTC) and with the given \a timeZone.

    \sa fromSecsSinceEpoch()
*/
QDateTime QDateTime::fromMSecsSinceEpoch(qint64 msecs, const QTimeZone &timeZone)
{
    QDateTime dt;
    dt.setTimeZone(timeZone);
    if (timeZone.isValid())
        dt.setMSecsSinceEpoch(msecs);
    return dt;
}

/*!
    \since 5.8

    Returns a datetime whose date and time are the number of seconds \a secs
    that have passed since 1970-01-01T00:00:00.000, Coordinated Universal
    Time (Qt::UTC) and with the given \a timeZone.

    \sa fromMSecsSinceEpoch()
*/
QDateTime QDateTime::fromSecsSinceEpoch(qint64 secs, const QTimeZone &timeZone)
{
    return fromMSecsSinceEpoch(secs * 1000, timeZone);
}
#endif

#if QT_DEPRECATED_SINCE(5, 2)
/*!
    \since 4.4
    \internal
    \obsolete

    This method was added in 4.4 but never documented as public. It was replaced
    in 5.2 with public method setOffsetFromUtc() for consistency with QTimeZone.

    This method should never be made public.

    \sa setOffsetFromUtc()
 */
void QDateTime::setUtcOffset(int seconds)
{
    setOffsetFromUtc(seconds);
}

/*!
    \since 4.4
    \internal
    \obsolete

    This method was added in 4.4 but never documented as public. It was replaced
    in 5.1 with public method offsetFromUTC() for consistency with QTimeZone.

    This method should never be made public.

    \sa offsetFromUTC()
*/
int QDateTime::utcOffset() const
{
    return offsetFromUtc();
}
#endif // QT_DEPRECATED_SINCE

#if QT_CONFIG(datestring) // depends on, so implies, textdate

/*!
    Returns the QDateTime represented by the \a string, using the
    \a format given, or an invalid datetime if this is not possible.

    Note for Qt::TextDate: It is recommended that you use the
    English short month names (e.g. "Jan"). Although localized month
    names can also be used, they depend on the user's locale settings.

    \sa toString(), QLocale::toDateTime()
*/
QDateTime QDateTime::fromString(const QString &string, Qt::DateFormat format)
{
    if (string.isEmpty())
        return QDateTime();

    switch (format) {
    case Qt::SystemLocaleDate:
    case Qt::SystemLocaleShortDate:
        return QLocale::system().toDateTime(string, QLocale::ShortFormat);
    case Qt::SystemLocaleLongDate:
        return QLocale::system().toDateTime(string, QLocale::LongFormat);
    case Qt::LocaleDate:
    case Qt::DefaultLocaleShortDate:
        return QLocale().toDateTime(string, QLocale::ShortFormat);
    case Qt::DefaultLocaleLongDate:
        return QLocale().toDateTime(string, QLocale::LongFormat);
    case Qt::RFC2822Date: {
        const ParsedRfcDateTime rfc = rfcDateImpl(string);

        if (!rfc.date.isValid() || !rfc.time.isValid())
            return QDateTime();

        QDateTime dateTime(rfc.date, rfc.time, Qt::UTC);
        dateTime.setOffsetFromUtc(rfc.utcOffset);
        return dateTime;
    }
    case Qt::ISODate:
    case Qt::ISODateWithMs: {
        const int size = string.size();
        if (size < 10)
            return QDateTime();

        QDate date = QDate::fromString(string.left(10), Qt::ISODate);
        if (!date.isValid())
            return QDateTime();
        if (size == 10)
            return date.startOfDay();

        Qt::TimeSpec spec = Qt::LocalTime;
        QStringView isoString = QStringView(string).mid(10); // trim "yyyy-MM-dd"

        // Must be left with T (or space) and at least one digit for the hour:
        if (isoString.size() < 2
            || !(isoString.startsWith(QLatin1Char('T'), Qt::CaseInsensitive)
                 // RFC 3339 (section 5.6) allows a space here.  (It actually
                 // allows any separator one considers more readable, merely
                 // giving space as an example - but let's not go wild !)
                 || isoString.startsWith(QLatin1Char(' ')))) {
            return QDateTime();
        }
        isoString = isoString.mid(1); // trim 'T' (or space)

        int offset = 0;
        // Check end of string for Time Zone definition, either Z for UTC or [+-]HH:mm for Offset
        if (isoString.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive)) {
            spec = Qt::UTC;
            isoString.chop(1); // trim 'Z'
        } else {
            // the loop below is faster but functionally equal to:
            // const int signIndex = isoString.indexOf(QRegExp(QStringLiteral("[+-]")));
            int signIndex = isoString.size() - 1;
            Q_ASSERT(signIndex >= 0);
            bool found = false;
            {
                const QChar plus = QLatin1Char('+');
                const QChar minus = QLatin1Char('-');
                do {
                    QChar character(isoString.at(signIndex));
                    found = character == plus || character == minus;
                } while (!found && --signIndex >= 0);
            }

            if (found) {
                bool ok;
                offset = fromOffsetString(isoString.mid(signIndex), &ok);
                if (!ok)
                    return QDateTime();
                isoString = isoString.left(signIndex);
                spec = Qt::OffsetFromUTC;
            }
        }

        // Might be end of day (24:00, including variants), which QTime considers invalid.
        // ISO 8601 (section 4.2.3) says that 24:00 is equivalent to 00:00 the next day.
        bool isMidnight24 = false;
        QTime time = fromIsoTimeString(isoString, format, &isMidnight24);
        if (!time.isValid())
            return QDateTime();
        if (isMidnight24)
            date = date.addDays(1);
        return QDateTime(date, time, spec, offset);
    }
    case Qt::TextDate: {
        QVector<QStringRef> parts = string.splitRef(QLatin1Char(' '), QString::SkipEmptyParts);

        if ((parts.count() < 5) || (parts.count() > 6))
            return QDateTime();

        // Accept "Sun Dec 1 13:02:00 1974" and "Sun 1. Dec 13:02:00 1974"

        // Year and time can be in either order.
        // Guess which by looking for ':' in the time
        int yearPart = 3;
        int timePart = 3;
        if (parts.at(3).contains(QLatin1Char(':')))
            yearPart = 4;
        else if (parts.at(4).contains(QLatin1Char(':')))
            timePart = 4;
        else
            return QDateTime();

        int month = 0;
        int day = 0;
        bool ok = false;

        int year = parts.at(yearPart).toInt(&ok);
        if (!ok || year == 0)
            return QDateTime();

        // Next try month then day
        month = fromShortMonthName(parts.at(1), year);
        if (month)
            day = parts.at(2).toInt(&ok);

        // If failed, try day then month
        if (!ok || !month || !day) {
            month = fromShortMonthName(parts.at(2), year);
            if (month) {
                QStringRef dayStr = parts.at(1);
                if (dayStr.endsWith(QLatin1Char('.'))) {
                    dayStr = dayStr.left(dayStr.size() - 1);
                    day = dayStr.toInt(&ok);
                }
            }
        }

        // If both failed, give up
        if (!ok || !month || !day)
            return QDateTime();

        QDate date(year, month, day);
        if (!date.isValid())
            return QDateTime();

        QVector<QStringRef> timeParts = parts.at(timePart).split(QLatin1Char(':'));
        if (timeParts.count() < 2 || timeParts.count() > 3)
            return QDateTime();

        int hour = timeParts.at(0).toInt(&ok);
        if (!ok)
            return QDateTime();

        int minute = timeParts.at(1).toInt(&ok);
        if (!ok)
            return QDateTime();

        int second = 0;
        int millisecond = 0;
        if (timeParts.count() > 2) {
            const QVector<QStringRef> secondParts = timeParts.at(2).split(QLatin1Char('.'));
            if (secondParts.size() > 2) {
                return QDateTime();
            }

            second = secondParts.first().toInt(&ok);
            if (!ok) {
                return QDateTime();
            }

            if (secondParts.size() > 1) {
                millisecond = secondParts.last().toInt(&ok);
                if (!ok) {
                    return QDateTime();
                }
            }
        }

        QTime time(hour, minute, second, millisecond);
        if (!time.isValid())
            return QDateTime();

        if (parts.count() == 5)
            return QDateTime(date, time, Qt::LocalTime);

        QStringView tz = parts.at(5);
        if (!tz.startsWith(QLatin1String("GMT"), Qt::CaseInsensitive))
            return QDateTime();
        tz = tz.mid(3);
        if (!tz.isEmpty()) {
            int offset = fromOffsetString(tz, &ok);
            if (!ok)
                return QDateTime();
            return QDateTime(date, time, Qt::OffsetFromUTC, offset);
        } else {
            return QDateTime(date, time, Qt::UTC);
        }
    }
    }

    return QDateTime();
}

/*!
    Returns the QDateTime represented by the \a string, using the \a
    format given, or an invalid datetime if the string cannot be parsed.

    Uses the calendar \a cal if supplied, else Gregorian.

    See QDate::fromString() and QTime::fromString() for the expressions
    recognized in the format string to represent parts of the date and time.
    All other input characters will be treated as text. Any sequence of
    characters that are enclosed in single quotes will also be treated as text
    and not be used as an expression.

    \snippet code/src_corelib_tools_qdatetime.cpp 12

    If the format is not satisfied, an invalid QDateTime is returned.
    The expressions that don't have leading zeroes (d, M, h, m, s, z) will be
    greedy. This means that they will use two digits even if this will
    put them outside the range and/or leave too few digits for other
    sections.

    \snippet code/src_corelib_tools_qdatetime.cpp 13

    This could have meant 1 January 00:30.00 but the M will grab
    two digits.

    Incorrectly specified fields of the \a string will cause an invalid
    QDateTime to be returned. For example, consider the following code,
    where the two digit year 12 is read as 1912 (see the table below for all
    field defaults); the resulting datetime is invalid because 23 April 1912
    was a Tuesday, not a Monday:

    \snippet code/src_corelib_tools_qdatetime.cpp 20

    The correct code is:

    \snippet code/src_corelib_tools_qdatetime.cpp 21

    For any field that is not represented in the format, the following
    defaults are used:

    \table
    \header \li Field  \li Default value
    \row    \li Year   \li 1900
    \row    \li Month  \li 1 (January)
    \row    \li Day    \li 1
    \row    \li Hour   \li 0
    \row    \li Minute \li 0
    \row    \li Second \li 0
    \endtable

    For example:

    \snippet code/src_corelib_tools_qdatetime.cpp 14

    \sa toString(), QDate::fromString(), QTime::fromString(),
    QLocale::toDateTime()
*/

QDateTime QDateTime::fromString(const QString &string, const QString &format, QCalendar cal)
{
#if QT_CONFIG(datetimeparser)
    QTime time;
    QDate date;

    QDateTimeParser dt(QVariant::DateTime, QDateTimeParser::FromString, cal);
    // dt.setDefaultLocale(QLocale::c()); ### Qt 6
    if (dt.parseFormat(format) && dt.fromString(string, &date, &time))
        return QDateTime(date, time);
#else
    Q_UNUSED(string);
    Q_UNUSED(format);
    Q_UNUSED(cal);
#endif
    return QDateTime();
}

/*
  \overload
*/

QDateTime QDateTime::fromString(const QString &string, const QString &format)
{
    return fromString(string, format, QCalendar());
}

#endif // datestring
/*!
    \fn QDateTime QDateTime::toLocalTime() const

    Returns a datetime containing the date and time information in
    this datetime, but specified using the Qt::LocalTime definition.

    Example:

    \snippet code/src_corelib_tools_qdatetime.cpp 17

    \sa toTimeSpec()
*/

/*!
    \fn QDateTime QDateTime::toUTC() const

    Returns a datetime containing the date and time information in
    this datetime, but specified using the Qt::UTC definition.

    Example:

    \snippet code/src_corelib_tools_qdatetime.cpp 18

    \sa toTimeSpec()
*/

/*****************************************************************************
  Date/time stream functions
 *****************************************************************************/

#ifndef QT_NO_DATASTREAM
/*!
    \relates QDate

    Writes the \a date to stream \a out.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator<<(QDataStream &out, const QDate &date)
{
    if (out.version() < QDataStream::Qt_5_0)
        return out << quint32(date.jd);
    else
        return out << qint64(date.jd);
}

/*!
    \relates QDate

    Reads a date from stream \a in into the \a date.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator>>(QDataStream &in, QDate &date)
{
    if (in.version() < QDataStream::Qt_5_0) {
        quint32 jd;
        in >> jd;
        // Older versions consider 0 an invalid jd.
        date.jd = (jd != 0 ? jd : QDate::nullJd());
    } else {
        qint64 jd;
        in >> jd;
        date.jd = jd;
    }

    return in;
}

/*!
    \relates QTime

    Writes \a time to stream \a out.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator<<(QDataStream &out, const QTime &time)
{
    if (out.version() >= QDataStream::Qt_4_0) {
        return out << quint32(time.mds);
    } else {
        // Qt3 had no support for reading -1, QTime() was valid and serialized as 0
        return out << quint32(time.isNull() ? 0 : time.mds);
    }
}

/*!
    \relates QTime

    Reads a time from stream \a in into the given \a time.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator>>(QDataStream &in, QTime &time)
{
    quint32 ds;
    in >> ds;
    if (in.version() >= QDataStream::Qt_4_0) {
        time.mds = int(ds);
    } else {
        // Qt3 would write 0 for a null time
        time.mds = (ds == 0) ? QTime::NullTime : int(ds);
    }
    return in;
}

/*!
    \relates QDateTime

    Writes \a dateTime to the \a out stream.

    \sa {Serializing Qt Data Types}
*/
QDataStream &operator<<(QDataStream &out, const QDateTime &dateTime)
{
    QPair<QDate, QTime> dateAndTime;

    if (out.version() >= QDataStream::Qt_5_2) {

        // In 5.2 we switched to using Qt::TimeSpec and added offset support
        dateAndTime = getDateTime(dateTime.d);
        out << dateAndTime << qint8(dateTime.timeSpec());
        if (dateTime.timeSpec() == Qt::OffsetFromUTC)
            out << qint32(dateTime.offsetFromUtc());
#if QT_CONFIG(timezone)
        else if (dateTime.timeSpec() == Qt::TimeZone)
            out << dateTime.timeZone();
#endif // timezone

    } else if (out.version() == QDataStream::Qt_5_0) {

        // In Qt 5.0 we incorrectly serialised all datetimes as UTC.
        // This approach is wrong and should not be used again; it breaks
        // the guarantee that a deserialised local datetime is the same time
        // of day, regardless of which timezone it was serialised in.
        dateAndTime = getDateTime((dateTime.isValid() ? dateTime.toUTC() : dateTime).d);
        out << dateAndTime << qint8(dateTime.timeSpec());

    } else if (out.version() >= QDataStream::Qt_4_0) {

        // From 4.0 to 5.1 (except 5.0) we used QDateTimePrivate::Spec
        dateAndTime = getDateTime(dateTime.d);
        out << dateAndTime;
        switch (dateTime.timeSpec()) {
        case Qt::UTC:
            out << (qint8)QDateTimePrivate::UTC;
            break;
        case Qt::OffsetFromUTC:
            out << (qint8)QDateTimePrivate::OffsetFromUTC;
            break;
        case Qt::TimeZone:
            out << (qint8)QDateTimePrivate::TimeZone;
            break;
        case Qt::LocalTime:
            out << (qint8)QDateTimePrivate::LocalUnknown;
            break;
        }

    } else { // version < QDataStream::Qt_4_0

        // Before 4.0 there was no TimeSpec, only Qt::LocalTime was supported
        dateAndTime = getDateTime(dateTime.d);
        out << dateAndTime;

    }

    return out;
}

/*!
    \relates QDateTime

    Reads a datetime from the stream \a in into \a dateTime.

    \sa {Serializing Qt Data Types}
*/

QDataStream &operator>>(QDataStream &in, QDateTime &dateTime)
{
    QDate dt;
    QTime tm;
    qint8 ts = 0;
    Qt::TimeSpec spec = Qt::LocalTime;
    qint32 offset = 0;
#if QT_CONFIG(timezone)
    QTimeZone tz;
#endif // timezone

    if (in.version() >= QDataStream::Qt_5_2) {

        // In 5.2 we switched to using Qt::TimeSpec and added offset support
        in >> dt >> tm >> ts;
        spec = static_cast<Qt::TimeSpec>(ts);
        if (spec == Qt::OffsetFromUTC) {
            in >> offset;
            dateTime = QDateTime(dt, tm, spec, offset);
#if QT_CONFIG(timezone)
        } else if (spec == Qt::TimeZone) {
            in >> tz;
            dateTime = QDateTime(dt, tm, tz);
#endif // timezone
        } else {
            dateTime = QDateTime(dt, tm, spec);
        }

    } else if (in.version() == QDataStream::Qt_5_0) {

        // In Qt 5.0 we incorrectly serialised all datetimes as UTC
        in >> dt >> tm >> ts;
        spec = static_cast<Qt::TimeSpec>(ts);
        dateTime = QDateTime(dt, tm, Qt::UTC);
        dateTime = dateTime.toTimeSpec(spec);

    } else if (in.version() >= QDataStream::Qt_4_0) {

        // From 4.0 to 5.1 (except 5.0) we used QDateTimePrivate::Spec
        in >> dt >> tm >> ts;
        switch ((QDateTimePrivate::Spec)ts) {
        case QDateTimePrivate::UTC:
            spec = Qt::UTC;
            break;
        case QDateTimePrivate::OffsetFromUTC:
            spec = Qt::OffsetFromUTC;
            break;
        case QDateTimePrivate::TimeZone:
            spec = Qt::TimeZone;
#if QT_CONFIG(timezone)
            // FIXME: need to use a different constructor !
#endif
            break;
        case QDateTimePrivate::LocalUnknown:
        case QDateTimePrivate::LocalStandard:
        case QDateTimePrivate::LocalDST:
            spec = Qt::LocalTime;
            break;
        }
        dateTime = QDateTime(dt, tm, spec, offset);

    } else { // version < QDataStream::Qt_4_0

        // Before 4.0 there was no TimeSpec, only Qt::LocalTime was supported
        in >> dt >> tm;
        dateTime = QDateTime(dt, tm, spec, offset);

    }

    return in;
}
#endif // QT_NO_DATASTREAM

/*****************************************************************************
  Date / Time Debug Streams
*****************************************************************************/

#if !defined(QT_NO_DEBUG_STREAM) && QT_CONFIG(datestring)
QDebug operator<<(QDebug dbg, const QDate &date)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace() << "QDate(";
    if (date.isValid())
        dbg.nospace() << date.toString(Qt::ISODate);
    else
        dbg.nospace() << "Invalid";
    dbg.nospace() << ')';
    return dbg;
}

QDebug operator<<(QDebug dbg, const QTime &time)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace() << "QTime(";
    if (time.isValid())
        dbg.nospace() << time.toString(u"HH:mm:ss.zzz");
    else
        dbg.nospace() << "Invalid";
    dbg.nospace() << ')';
    return dbg;
}

QDebug operator<<(QDebug dbg, const QDateTime &date)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace() << "QDateTime(";
    if (date.isValid()) {
        const Qt::TimeSpec ts = date.timeSpec();
        dbg.noquote() << date.toString(u"yyyy-MM-dd HH:mm:ss.zzz t")
                      << ' ' << ts;
        switch (ts) {
        case Qt::UTC:
            break;
        case Qt::OffsetFromUTC:
            dbg.space() << date.offsetFromUtc() << 's';
            break;
        case Qt::TimeZone:
#if QT_CONFIG(timezone)
            dbg.space() << date.timeZone().id();
#endif // timezone
            break;
        case Qt::LocalTime:
            break;
        }
    } else {
        dbg.nospace() << "Invalid";
    }
    return dbg.nospace() << ')';
}
#endif // debug_stream && datestring

/*! \fn uint qHash(const QDateTime &key, uint seed = 0)
    \relates QHash
    \since 5.0

    Returns the hash value for the \a key, using \a seed to seed the calculation.
*/
uint qHash(const QDateTime &key, uint seed)
{
    // Use to toMSecsSinceEpoch instead of individual qHash functions for
    // QDate/QTime/spec/offset because QDateTime::operator== converts both arguments
    // to the same timezone. If we don't, qHash would return different hashes for
    // two QDateTimes that are equivalent once converted to the same timezone.
    return key.isValid() ? qHash(key.toMSecsSinceEpoch(), seed) : seed;
}

/*! \fn uint qHash(const QDate &key, uint seed = 0)
    \relates QHash
    \since 5.0

    Returns the hash value for the \a key, using \a seed to seed the calculation.
*/
uint qHash(const QDate &key, uint seed) noexcept
{
    return qHash(key.toJulianDay(), seed);
}

/*! \fn uint qHash(const QTime &key, uint seed = 0)
    \relates QHash
    \since 5.0

    Returns the hash value for the \a key, using \a seed to seed the calculation.
*/
uint qHash(const QTime &key, uint seed) noexcept
{
    return qHash(key.msecsSinceStartOfDay(), seed);
}

QT_END_NAMESPACE

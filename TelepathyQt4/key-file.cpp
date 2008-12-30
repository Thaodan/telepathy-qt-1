/*
 * This file is part of TelepathyQt4
 *
 * Copyright (C) 2008 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2008 Nokia Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define IN_TELEPATHY_QT4_INTERNALS
#include "key-file.h"

#include "debug-internal.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QHash>

namespace Telepathy
{

struct KeyFile::Private
{
    QString fileName;
    KeyFile::Status status;
    QHash<QString, QHash<QString, QString> > groups;
    QString currentGroup;

    Private();
    Private(const QString &fName);

    void setFileName(const QString &fName);
    void setError(KeyFile::Status status, const QString &reason);
    bool read();

    bool escapedKey(const QByteArray &data, int from, int to, QString &result);
    bool escapedString(const QByteArray &data, int from, int to, QString &result);

    QStringList allGroups() const;
    QStringList allKeys() const;
    QStringList keys() const;
    bool contains(const QString &key) const;
    QString value(const QString &key) const;
};

KeyFile::Private::Private()
    : status(KeyFile::None)
{
}

KeyFile::Private::Private(const QString &fName)
    : fileName(fName),
      status(KeyFile::NoError),
      currentGroup("general")
{
    read();
}

void KeyFile::Private::setFileName(const QString &fName)
{
    fileName = fName;
    status = KeyFile::NoError;
    currentGroup = "general";
    groups.clear();
    read();
}

void KeyFile::Private::setError(KeyFile::Status status, const QString &reason)
{
    warning() << QString("ERROR: filename(%1) reason(%2)")
                         .arg(fileName).arg(reason);
    status = status;
    groups.clear();
}

bool KeyFile::Private::read()
{
    QFile file(fileName);
    if (!file.exists()) {
        setError(KeyFile::NotFoundError,
                 "file does not exist");
        return false;
    }

    if (!file.open(QFile::ReadOnly)) {
        setError(KeyFile::AccessError,
                 "cannot open file for readonly access");
        return false;
    }

    QByteArray data;
    QByteArray group;
    QString currentGroup("general");
    QHash<QString, QString> groupMap;
    int line = 0;
    int idx;
    while (!file.atEnd()) {
        data = file.readLine().trimmed();
        line++;

        if (data.size() == 0) {
            // skip empty lines
            continue;
        }

        char ch = data.at(0);
        if (ch == '#') {
            // skip comments
            continue;
        }
        else if (ch == '[') {
            if (groupMap.size()) {
                groups[currentGroup] = groupMap;
                groupMap.clear();
            }

            idx = data.indexOf(']');
            if (idx == -1) {
                // line starts with [ and it's not a group
                setError(KeyFile::FormatError,
                         QString("invalid group at line %2 - missing ']'")
                                 .arg(line));
                return false;
            }

            group = data.mid(1, idx - 1).trimmed();
            if (groups.contains(group)) {
                setError(KeyFile::FormatError,
                         QString("duplicated group '%1' at line %2")
                                 .arg(group.constData()).arg(line));
                return false;
            }

            currentGroup = "";
            if (!escapedString(group, 0, group.size(), currentGroup)) {
                setError(KeyFile::FormatError,
                         QString("invalid group '%1' at line %2")
                                 .arg(currentGroup).arg(line));
                return false;
            }
        }
        else {
            idx = data.indexOf('=');
            if (idx == 0) {
                setError(KeyFile::FormatError,
                         QString("format error at line %1 - missing '='")
                                 .arg(line));
                return false;
            }

            // remove trailing spaces
            char ch;
            while ((ch = data.at(idx - 1)) == ' ' || ch == '\t') {
                --idx;
            }

            QString key;
            if (!escapedKey(data, 0, idx, key)) {
                setError(KeyFile::FormatError,
                         QString("invalid key '%1' at line %2")
                                 .arg(key).arg(line));
                return false;
            }

            data = data.mid(idx + 1).trimmed();
            QString value;
            if (!escapedString(data, 0, data.size(), value)) {
                setError(KeyFile::FormatError,
                         QString("invalid key value for key '%2' at line %3")
                                 .arg(key).arg(line));
                return false;
            }

            if (groupMap.contains(key)) {
                setError(KeyFile::FormatError,
                         QString("duplicated key '%1' on group '%2' at line %3")
                                 .arg(key).arg(currentGroup).arg(line));
                return false;
            }
            groupMap[key] = value;
        }
    }

    if (groupMap.size()) {
        groups[currentGroup] = groupMap;
        groupMap.clear();
    }

    return true;
}

bool KeyFile::Private::escapedKey(const QByteArray &data, int from, int to, QString &result)
{
    int i = from;
    bool ret = true;
    while (i < to) {
        uint ch = data.at(i++);
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              (ch == '-'))) {
            ret = false;
        }
        result += ch;
    }
    return ret;
}

bool KeyFile::Private::escapedString(const QByteArray &data, int from, int to, QString &result)
{
    int i = from;
    while (i < to) {
        uint ch = data.at(i++);

        if (ch == '\\') {
            char nextCh = data.at(i++);
            switch (nextCh) {
                case 's':
                    result += ' ';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case ';':
                    // keep \; there so we can split lists properly
                    result += "\\;";
                    break;
                case '\\':
                    result += '\\';
                    break;
                default:
                    return false;
            }
        }
        else {
            result += ch;
        }
    }

    return true;
}

QStringList KeyFile::Private::allGroups() const
{
    return groups.keys();
}

QStringList KeyFile::Private::allKeys() const
{
    QStringList keys;
    QHash<QString, QHash<QString, QString> >::const_iterator itrGroups = groups.begin();
    while (itrGroups != groups.end()) {
        keys << itrGroups.value().keys();
        ++itrGroups;
    }
    return keys;
}

QStringList KeyFile::Private::keys() const
{
    QHash<QString, QString> groupMap = groups[currentGroup];
    return groupMap.keys();
}

bool KeyFile::Private::contains(const QString &key) const
{
    QHash<QString, QString> groupMap = groups[currentGroup];
    return groupMap.contains(key);
}

QString KeyFile::Private::value(const QString &key) const
{
    QHash<QString, QString> groupMap = groups[currentGroup];
    return groupMap.value(key);
}


/**
 * \class KeyFile
 * \headerfile <TelepathyQt4/key-file.h> <TelepathyQt4/KeyFile>
 *
 * The KeyFile class provides an easy way to read key-pair files such as INI
 * style files and .desktop files.
 *
 * It follows the rules regarding string escaping as defined in
 * http://standards.freedesktop.org/desktop-entry-spec/latest/index.html
 */

/**
 * Creates a KeyFile object used to read (key-pair) compliant files.
 *
 * The status will be #KeyFile::None
 * \sa setFileName()
 */
KeyFile::KeyFile()
    : mPriv(new Private())
{
}

/**
 * Creates a KeyFile object used to read (key-pair) compliant files.
 *
 * \param fileName Name of the file to be read.
 */
KeyFile::KeyFile(const QString &fileName)
    : mPriv(new Private(fileName))
{
}

/**
 * Class destructor.
 */
KeyFile::~KeyFile()
{
    delete mPriv;
}

/**
 * Sets the name of the file to be read.
 *
 * \param fileName Name of the file to be read.
 */
void KeyFile::setFileName(const QString &fileName)
{
    mPriv->setFileName(fileName);
}

/**
 * Returns the name of the file associated with this object.
 *
 * \return Name of the file associated with this object.
 */
QString KeyFile::fileName() const
{
    return mPriv->fileName;
}

/**
 * Return a status code indicating the first error that was met by #KeyFile,
 * or KeyFile::NoError if no error occurred.
 *
 * Make sure to use this method if you set the filename to be read using
 * setFileName().
 *
 * \return Status code.
 * \sa setFileName()
 */
KeyFile::Status KeyFile::status() const
{
    return mPriv->status;
}

/**
 * Set the current group to be used while reading keys.
 *
 * Query functions such as keys(), contains() and value() are based on this
 * group.
 *
 * By default a group named "general" is used as the group for global
 * keys and is used as the default group if none is set.
 *
 * \param group Name of the group to be used.
 * \sa group()
 */
void KeyFile::setGroup(const QString &group)
{
    mPriv->currentGroup = group;
}

/**
 * Returns the current group.
 *
 * \return Name of the current group.
 * \sa setGroup()
 */
QString KeyFile::group() const
{
    return mPriv->currentGroup;
}

/**
 * Returns all groups in the desktop file.
 *
 * Global keys will be added to a group named "general".
 *
 * \return List of all groups in the desktop file.
 */
QStringList KeyFile::allGroups() const
{
    return mPriv->allGroups();
}

/**
 * Returns all keys described in the desktop file.
 *
 * \return List of all keys in the desktop file.
 */
QStringList KeyFile::allKeys() const
{
    return mPriv->allKeys();
}

/**
 * Returns a list of keys in the current group.
 *
 * \return List of all keys in the current group.
 * \sa group(), setGroup()
 */
QStringList KeyFile::keys() const
{
    return mPriv->keys();
}

/**
 * Check if the current group contains a key named \a key.
 *
 * \return true if \a key exists, false otherwise.
 * \sa group(), setGroup()
 */
bool KeyFile::contains(const QString &key) const
{
    return mPriv->contains(key);
}

/**
 * Get the value for the key in the current group named \a key.
 *
 * \return Value of \a key, empty string if not found.
 * \sa group(), setGroup()
 */
QString KeyFile::value(const QString &key) const
{
    return mPriv->value(key);
}

}

/**
 * This file is part of TelepathyQt
 *
 * @copyright Copyright (C) 2012 Collabora Ltd. <http://www.collabora.co.uk/>
 * @license LGPL 2.1
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

#ifndef _TelepathyQt_captcha_h_HEADER_GUARD_
#define _TelepathyQt_captcha_h_HEADER_GUARD_

//#ifndef IN_TELEPATHY_QT4_HEADER
//#error IN_TELEPATHY_QT4_HEADER
//#endif

#include <TelepathyQt4/Constants>
#include "captcha-authentication.h"

namespace Tp
{

class PendingCaptchas;

class CaptchaData;
class TP_QT_NO_EXPORT Captcha {
public:
    Captcha();
    Captcha(const Captcha &other);
    ~Captcha();

    Captcha &operator=(const Captcha &rhs);

    QString mimeType() const;
    QString label() const;
    QByteArray data() const;
    CaptchaAuthentication::ChallengeType type() const;
    uint id() const;

private:
    friend class PendingCaptchas;

    Captcha(const QString &mimeType, const QString &label, const QByteArray &data,
            CaptchaAuthentication::ChallengeType type, uint id);

    QSharedDataPointer<CaptchaData> mPriv;
};

}

#endif // TP_CAPTCHA_H

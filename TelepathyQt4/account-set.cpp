/*
 * This file is part of TelepathyQt4
 *
 * Copyright (C) 2010 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2010 Nokia Corporation
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

#include <TelepathyQt4/AccountSet>
#include "TelepathyQt4/account-set-internal.h"

#include "TelepathyQt4/_gen/account-set.moc.hpp"
#include "TelepathyQt4/_gen/account-set-internal.moc.hpp"

#include "TelepathyQt4/debug-internal.h"

#include <TelepathyQt4/Account>
#include <TelepathyQt4/AccountManager>
#include <TelepathyQt4/ConnectionCapabilities>
#include <TelepathyQt4/ConnectionManager>

namespace Tp
{

AccountSet::Private::Private(AccountSet *parent,
        const AccountManagerPtr &accountManager,
        const QList<AccountFilterConstPtr> &filters)
    : parent(parent),
      accountManager(accountManager),
      filters(filters),
      ready(false)
{
    init();
}

AccountSet::Private::Private(AccountSet *parent,
        const AccountManagerPtr &accountManager,
        const QVariantMap &filter)
    : parent(parent),
      accountManager(accountManager),
      ready(false)
{
    AccountPropertyFilterPtr filterObj = AccountPropertyFilter::create();
    for (QVariantMap::const_iterator i = filter.constBegin();
            i != filter.constEnd(); ++i) {
        filterObj->addProperty(i.key(), i.value());
    }
    filters.append(filterObj);
    init();
}

void AccountSet::Private::init()
{
    if (checkFilters()) {
        connectSignals();
        insertAccounts();
        ready = true;
    }
}

bool AccountSet::Private::checkFilters()
{
    foreach (const AccountFilterConstPtr &filter, filters) {
        if (!filter->isValid()) {
            return false;
        }
    }

    return true;
}

void AccountSet::Private::connectSignals()
{
    parent->connect(accountManager.data(),
            SIGNAL(newAccount(Tp::AccountPtr)),
            SLOT(onNewAccount(Tp::AccountPtr)));
}

void AccountSet::Private::insertAccounts()
{
    foreach (const Tp::AccountPtr &account, accountManager->allAccounts()) {
        insertAccount(account);
    }
}

void AccountSet::Private::insertAccount(const Tp::AccountPtr &account)
{
    QString accountPath = account->objectPath();
    Q_ASSERT(!wrappers.contains(accountPath));
    wrapAccount(account);
    filterAccount(account);
}

void AccountSet::Private::removeAccount(const Tp::AccountPtr &account)
{
    QString accountPath = account->objectPath();
    Q_ASSERT(wrappers.contains(accountPath));
    accounts.remove(accountPath);

    AccountWrapper *wrapper = wrappers.take(accountPath);
    Q_ASSERT(wrapper->disconnect(parent));
    wrapper->deleteLater();

    emit parent->accountRemoved(account);
}

void AccountSet::Private::wrapAccount(const AccountPtr &account)
{
    AccountWrapper *wrapper = new AccountWrapper(account, parent);
    parent->connect(wrapper,
            SIGNAL(accountRemoved(Tp::AccountPtr)),
            SLOT(onAccountRemoved(Tp::AccountPtr)));
    parent->connect(wrapper,
            SIGNAL(accountPropertyChanged(Tp::AccountPtr,QString)),
            SLOT(onAccountChanged(Tp::AccountPtr)));
    parent->connect(wrapper,
            SIGNAL(accountCapabilitiesChanged(Tp::AccountPtr,Tp::ConnectionCapabilities*)),
            SLOT(onAccountChanged(Tp::AccountPtr)));
    wrappers.insert(account->objectPath(), wrapper);
}

void AccountSet::Private::filterAccount(const AccountPtr &account)
{
    QString accountPath = account->objectPath();
    Q_ASSERT(wrappers.contains(accountPath));
    AccountWrapper *wrapper = wrappers[accountPath];

    /* account changed, let's check if it matches filter */
    if (accountMatchFilters(wrapper)) {
        if (!accounts.contains(account->objectPath())) {
            accounts.insert(account->objectPath(), account);
            if (ready) {
                emit parent->accountAdded(account);
            }
        }
    } else {
        if (accounts.contains(account->objectPath())) {
            accounts.remove(account->objectPath());
            if (ready) {
                emit parent->accountRemoved(account);
            }
        }
    }
}

bool AccountSet::Private::accountMatchFilters(AccountWrapper *wrapper)
{
    if (filters.isEmpty()) {
        return true;
    }

    AccountPtr account = wrapper->account();
    foreach (const AccountFilterConstPtr &filter, filters) {
        if (!filter->matches(account)) {
            return false;
        }
    }

    return true;
}

AccountSet::Private::AccountWrapper::AccountWrapper(
        const AccountPtr &account, QObject *parent)
    : QObject(parent),
      mAccount(account)
{
    connect(account.data(),
            SIGNAL(removed()),
            SLOT(onAccountRemoved()));
    connect(account.data(),
            SIGNAL(propertyChanged(QString)),
            SLOT(onAccountPropertyChanged(QString)));
    connect(account.data(),
            SIGNAL(capabilitiesChanged(Tp::ConnectionCapabilities*)),
            SLOT(onAccountCapalitiesChanged(Tp::ConnectionCapabilities*)));
}

AccountSet::Private::AccountWrapper::~AccountWrapper()
{
}

ConnectionCapabilities *AccountSet::Private::AccountWrapper::capabilities() const
{
    if (!mAccount->connection().isNull() &&
        mAccount->connection()->status() == Connection::StatusConnected) {
        return mAccount->connection()->capabilities();
    } else {
        if (mAccount->protocolInfo()) {
            return mAccount->protocolInfo()->capabilities();
        }
    }
    /* either we don't have a connected connection or
     * Account::FeatureProtocolInfo is not ready */
    return 0;
}

void AccountSet::Private::AccountWrapper::onAccountRemoved()
{
    emit accountRemoved(mAccount);
}

void AccountSet::Private::AccountWrapper::onAccountPropertyChanged(
        const QString &propertyName)
{
    emit accountPropertyChanged(mAccount, propertyName);
}

void AccountSet::Private::AccountWrapper::onAccountCapalitiesChanged(
        ConnectionCapabilities *caps)
{
    emit accountCapabilitiesChanged(mAccount, caps);
}

/**
 * \class AccountSet
 * \ingroup clientaccount
 * \headerfile TelepathyQt4/account-set.h <TelepathyQt4/AccountSet>
 *
 * \brief The AccountSet class provides an object representing a set of
 * Telepathy accounts filtered by a given criteria.
 *
 * AccountSet is automatically updated whenever accounts that match the given
 * criteria are added, removed or updated.
 *
 * \section account_set_usage_sec Usage
 *
 * \subsection account_set_create_sec Creating an AccountSet object
 *
 * The easiest way to create AccountSet objects is through AccountManager. One
 * can just use the AccountManager convenience methods such as
 * AccountManager::validAccountsSet() to get a set of account objects
 * representing valid accounts.
 *
 * For example:
 *
 * \code
 *
 * class MyClass : public QObject
 * {
 *     QOBJECT
 *
 * public:
 *     MyClass(QObject *parent = 0);
 *     ~MyClass() { }
 *
 * private Q_SLOTS:
 *     void onAccountManagerReady(Tp::PendingOperation *);
 *     void onValidAccountAdded(const Tp::AccountPtr &);
 *     void onValidAccountRemoved(const Tp::AccountPtr &);
 *
 * private:
 *     AccountManagerPtr am;
 *     AccountSetPtr validAccountsSet;
 * };
 *
 * MyClass::MyClass(QObject *parent)
 *     : QObject(parent)
 *       am(AccountManager::create())
 * {
 *     connect(am->becomeReady(),
 *             SIGNAL(finished(Tp::PendingOperation*)),
 *             SLOT(onAccountManagerReady(Tp::PendingOperation*)));
 * }
 *
 * void MyClass::onAccountManagerReady(Tp::PendingOperation *op)
 * {
 *     if (op->isError()) {
 *         qWarning() << "Account manager cannot become ready:" <<
 *             op->errorName() << "-" << op->errorMessage();
 *         return;
 *     }
 *
 *     validAccountsSet = am->validAccountsSet();
 *     connect(validAccountsSet.data(),
 *             SIGNAL(accountAdded(const Tp::AccountPtr &)),
 *             SLOT(onValidAccountAdded(const Tp::AccountPtr &)));
 *     connect(validAccountsSet.data(),
 *             SIGNAL(accountRemoved(const Tp::AccountPtr &)),
 *             SLOT(onValidAccountRemoved(const Tp::AccountPtr &)));
 *
 *     QList<AccountPtr> accounts = validAccountsSet->accounts();
 *     // do something with accounts
 * }
 *
 * void MyClass::onValidAccountAdded(const Tp::AccountPtr &account)
 * {
 *     // do something with account
 * }
 *
 * void MyClass::onValidAccountRemoved(const Tp::AccountPtr &account)
 * {
 *     // do something with account
 * }
 *
 * \endcode
 *
 * You can also define your own filter using AccountManager::filterAccounts:
 *
 * \code
 *
 * void MyClass::onAccountManagerReady(Tp::PendingOperation *op)
 * {
 *     ...
 *
 *     QList<AccountFilterConstPtr> filters;
 *     AccountPropertyFilterPtr filter = AccountPropertyFilter::create();
 *     filter->addProperty(QLatin1String("protocolName"), QLatin1String("jabber"));
 *     filter->addProperty(QLatin1String("enabled"), true);
 *     filters.append(filter);
 *
 *     AccountSetPtr filteredAccountSet = am->filterAccounts(filter);
 *     // connect to AccountSet::accountAdded/accountRemoved signals
 *     QList<AccountPtr> accounts = filteredAccountSet->accounts();
 *     // do something with accounts
 *
 *     ....
 * }
 *
 * \endcode
 *
 * Note that for AccountSet to property work with AccountCapabilityFilter
 * objects, the feature Account::FeatureCapabilities need to be enabled in all
 * accounts return by the AccountManager passed as param in the constructor.
 * The easiest way to do this is to enable AccountManager feature
 * AccountManager::FeatureFilterByCapabilities.
 *
 * AccountSet can also be instantiated directly, but when doing it,
 * the AccountManager object passed as param in the constructor must be ready
 * for AccountSet properly work.
 */

/**
 * Construct a new AccountSet object.
 *
 * \param accountManager An account manager object used to filter accounts.
 *                       The account manager object must be ready.
 * \param filters The desired filter.
 */
AccountSet::AccountSet(const AccountManagerPtr &accountManager,
        const QList<AccountFilterConstPtr> &filters)
    : QObject(),
      mPriv(new Private(this, accountManager, filters))
{
}

/**
 * Construct a new AccountSet object.
 *
 * The \a filter must contain Account property names and values as map items.
 *
 * \param accountManager An account manager object used to filter accounts.
 *                       The account manager object must be ready.
 * \param filter The desired filter.
 */
AccountSet::AccountSet(const AccountManagerPtr &accountManager,
        const QVariantMap &filter)
    : QObject(),
      mPriv(new Private(this, accountManager, filter))
{
}

/**
 * Class destructor.
 */
AccountSet::~AccountSet()
{
}

/**
 * Return the account manager object used to filter accounts.
 *
 * \return The AccountManager object used to filter accounts.
 */
AccountManagerPtr AccountSet::accountManager() const
{
    return mPriv->accountManager;
}

/**
 * Return whether the filter returned by filter()/filters() is valid.
 *
 * If the filter is invalid accounts() will always return an empty list.
 *
 * This method is deprecated and should not be used in newly written code. Use
 * Filter::isValid() instead.
 *
 * \deprecated Use Filter::isValid() instead.
 *
 * \return \c true if the filter returned by filter()/filters() is valid, otherwise \c
 *         false.
 */
bool AccountSet::isFilterValid() const
{
    return _deprecated_isFilterValid();
}

bool AccountSet::_deprecated_isFilterValid() const
{
    foreach (const AccountFilterConstPtr &filter, mPriv->filters) {
        if (!filter->isValid()) {
            return false;
        }
    }

    return true;
}

/**
 * Return the filter used to filter accounts.
 *
 * The filter is composed by Account property names and values as map items.
 *
 * \deprecated
 *
 * \return A QVariantMap representing the filter used to filter accounts.
 */
QVariantMap AccountSet::filter() const
{
    return _deprecated_filter();
}

QVariantMap AccountSet::_deprecated_filter() const
{
    QVariantMap result;
    foreach (const AccountFilterConstPtr &filter, mPriv->filters) {
        const AccountPropertyFilterConstPtr filterObj =
            AccountPropertyFilterConstPtr::dynamicCast(filter);
        if (filterObj) {
            result.unite(filterObj->filter());
        }
    }
    return result;
}

/**
 * Return the filters used to filter accounts.
 *
 * \deprecated
 *
 * \return A list of filter objects used to filter accounts.
 */
QList<AccountFilterConstPtr> AccountSet::filters() const
{
    return _deprecated_filters();
}

QList<AccountFilterConstPtr> AccountSet::_deprecated_filters() const
{
    return mPriv->filters;
}

/**
 * Return a list of account objects that match filters.
 *
 * \return A list of account objects that match filters.
 */
QList<AccountPtr> AccountSet::accounts() const
{
    return mPriv->accounts.values();
}

/**
 * \fn void AccountSet::accountAdded(const Tp::AccountPtr &account);
 *
 * This signal is emitted whenever an account that matches filters is added to
 * this set.
 *
 * \param account The account that was added to this set.
 */

/**
 * \fn void AccountSet::accountRemoved(const Tp::AccountPtr &account);
 *
 * This signal is emitted whenever an account that matches filters is removed
 * from this set.
 *
 * \param account The account that was removed from this set.
 */

void AccountSet::onNewAccount(const AccountPtr &account)
{
    mPriv->insertAccount(account);
}

void AccountSet::onAccountRemoved(const AccountPtr &account)
{
    mPriv->removeAccount(account);
}

void AccountSet::onAccountChanged(const AccountPtr &account)
{
    mPriv->filterAccount(account);
}

} // Tp

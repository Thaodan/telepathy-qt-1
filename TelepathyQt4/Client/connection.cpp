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

#include <TelepathyQt4/Client/Connection>
#include "TelepathyQt4/Client/connection-internal.h"

#include "TelepathyQt4/_gen/cli-connection.moc.hpp"
#include "TelepathyQt4/_gen/cli-connection-body.hpp"
#include "TelepathyQt4/Client/_gen/connection.moc.hpp"
#include "TelepathyQt4/Client/_gen/connection-internal.moc.hpp"

#include "TelepathyQt4/debug-internal.h"

#include <TelepathyQt4/Client/ContactManager>
#include <TelepathyQt4/Client/PendingChannel>
#include <TelepathyQt4/Client/PendingContactAttributes>
#include <TelepathyQt4/Client/PendingContacts>
#include <TelepathyQt4/Client/PendingFailure>
#include <TelepathyQt4/Client/PendingHandles>
#include <TelepathyQt4/Client/PendingVoidMethodCall>

#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QQueue>
#include <QSharedPointer>
#include <QString>
#include <QTimer>
#include <QtGlobal>

/**
 * \addtogroup clientsideproxies Client-side proxies
 *
 * Proxy objects representing remote service objects accessed via D-Bus.
 *
 * In addition to providing direct access to methods, signals and properties
 * exported by the remote objects, some of these proxies offer features like
 * automatic inspection of remote object capabilities, property tracking,
 * backwards compatibility helpers for older services and other utilities.
 */

/**
 * \defgroup clientconn Connection proxies
 * \ingroup clientsideproxies
 *
 * Proxy objects representing remote Telepathy Connection objects.
 */

namespace Telepathy
{
namespace Client
{

struct Connection::Private
{
    /*
     * \enum Connection::Private::Readiness
     *
     * Describes readiness of the Connection for usage. The readiness depends
     * on the state of the remote object. In suitable states, an asynchronous
     * introspection process is started, and the Connection becomes more ready
     * when that process is completed.
     *
     * \value ReadinessJustCreated, The object has just been created and introspection
     *                              is still in progress. No functionality is available.
     *                              The readiness can change to any other state depending
     *                              on the result of the initial state query to the remote
     *                              object.
     * \value ReadinessNotYetConnected The remote object is in the Disconnected state and
     *                                 introspection relevant to that state has been completed.
     *                                 This state is useful for being able to set your presence status
     *                                 (through the SimplePresence interface) before connecting. Most other
     *                                 functionality is unavailable, though.
     *                                 The readiness can change to ReadinessConnecting and ReadinessDead.
     * \value ReadinessConnecting The remote object is in the Connecting state. Most functionality is
     *                            unavailable.
     *                            The readiness can change to ReadinessFull and ReadinessDead.
     * \value ReadinessFull The connection is in the Connected state and all introspection
     *                      has been completed. Most functionality is available.
     *                      The readiness can change to ReadinessDead.
     * \value ReadinessDead The remote object has gone into a state where it can no longer be
     *                      used. No functionality is available.
     *                      No further readiness changes are possible.
     */
    enum Readiness {
        ReadinessJustCreated = 0,
        ReadinessNotYetConnected = 5,
        ReadinessConnecting = 10,
        ReadinessFull = 15,
        ReadinessDead = 20,
        _ReadinessInvalid = 0xffff
    };

    Private(Connection *parent);
    ~Private();

    void startIntrospection();
    void introspectMain();
    void introspectContacts();
    void introspectSimplePresence();
    void introspectSelfContact();
    void introspectSelfHandle();

    void changeReadiness(Readiness newReadiness);

    void updatePendingOperations();

    struct HandleContext;
    class PendingReady;

    // Public object
    Connection *parent;

    // Instance of generated interface class
    ConnectionInterface *baseInterface;

    // Optional interface proxies
    DBus::PropertiesInterface *properties;
    ConnectionInterfaceSimplePresenceInterface *simplePresence;

    bool ready;
    QList<PendingReady *> pendingOperations;

    // Introspection
    bool initialIntrospection;
    Readiness readiness;
    QStringList interfaces;
    QQueue<void (Private::*)()> introspectQueue;

    Connection::Features features;
    Connection::Features pendingFeatures;
    Connection::Features missingFeatures;

    // Introspected properties
    // keep pendingStatus and pendingStatusReason until we emit statusChanged
    // so Connection::status() and Connection::statusReason() are consistent
    uint pendingStatus;
    uint pendingStatusReason;
    uint status;
    uint statusReason;
    bool haveInitialStatus;
    SimpleStatusSpecMap simplePresenceStatuses;
    QSharedPointer<Contact> selfContact;
    QStringList contactAttributeInterfaces;

    uint selfHandle;

    // (Bus connection name, service name) -> HandleContext
    static QMap<QPair<QString, QString>, HandleContext *> handleContexts;
    static QMutex handleContextsLock;
    HandleContext *handleContext;

    ContactManager *contactManager;
};

// Handle tracking
struct Connection::Private::HandleContext
{
    struct Type
    {
        QMap<uint, uint> refcounts;
        QSet<uint> toRelease;
        uint requestsInFlight;
        bool releaseScheduled;

        Type()
            : requestsInFlight(0),
              releaseScheduled(false)
        {
        }
    };

    HandleContext()
        : refcount(0)
    {
    }

    int refcount;
    QMutex lock;
    QMap<uint, Type> types;
};

class Connection::Private::PendingReady : public PendingOperation
{
    // Connection is a friend so it can call finished() etc.
    friend class Connection;

public:
    PendingReady(Connection::Features features, QObject *parent);

    Connection::Features features;
};

Connection::Private::PendingReady::PendingReady(Connection::Features features, QObject *parent)
    : PendingOperation(parent),
      features(features)
{
}

Connection::Private::Private(Connection *parent)
    : parent(parent),
      baseInterface(new ConnectionInterface(parent->dbusConnection(),
                    parent->busName(), parent->objectPath(), parent)),
      properties(0),
      simplePresence(0),
      ready(false),
      initialIntrospection(false),
      readiness(ReadinessJustCreated),
      pendingStatus(Connection::StatusUnknown),
      pendingStatusReason(ConnectionStatusReasonNoneSpecified),
      status(Connection::StatusUnknown),
      statusReason(ConnectionStatusReasonNoneSpecified),
      haveInitialStatus(false),
      selfHandle(0),
      handleContext(0),
      contactManager(new ContactManager(parent))
{
}

Connection::Private::~Private()
{
    // Clear selfContact so its handle will be released cleanly before the handleContext
    selfContact.clear();

    // FIXME: This doesn't look right! In fact, looks absolutely horrendous.
    if (!handleContext) {
        // initial introspection is not done
        return;
    }

    QMutexLocker locker(&handleContextsLock);

    // All handle contexts locked, so safe
    if (!--handleContext->refcount) {
        debug() << "Destroying HandleContext";

        foreach (uint handleType, handleContext->types.keys()) {
            HandleContext::Type type = handleContext->types[handleType];

            if (!type.refcounts.empty()) {
                debug() << " Still had references to" <<
                    type.refcounts.size() << "handles, releasing now";
                baseInterface->ReleaseHandles(handleType, type.refcounts.keys());
            }

            if (!type.toRelease.empty()) {
                debug() << " Was going to release" <<
                    type.toRelease.size() << "handles, doing that now";
                baseInterface->ReleaseHandles(handleType, type.toRelease.toList());
            }
        }

        handleContexts.remove(qMakePair(baseInterface->connection().name(),
                    baseInterface->service()));
        delete handleContext;
    }
    else {
        Q_ASSERT(handleContext->refcount > 0);
    }
}

void Connection::Private::startIntrospection()
{
    debug() << "Connecting to StatusChanged()";

    parent->connect(baseInterface,
                    SIGNAL(StatusChanged(uint, uint)),
                    SLOT(onStatusChanged(uint, uint)));

    debug() << "Calling GetStatus()";

    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(baseInterface->GetStatus(), parent);
    parent->connect(watcher,
                    SIGNAL(finished(QDBusPendingCallWatcher *)),
                    SLOT(gotStatus(QDBusPendingCallWatcher *)));

    QMutexLocker locker(&handleContextsLock);
    QString busConnectionName = baseInterface->connection().name();
    QString serviceName = baseInterface->service();

    if (handleContexts.contains(qMakePair(busConnectionName, serviceName))) {
        debug() << "Reusing existing HandleContext";
        handleContext = handleContexts[qMakePair(busConnectionName, serviceName)];
    }
    else {
        debug() << "Creating new HandleContext";
        handleContext = new HandleContext;
        handleContexts[qMakePair(busConnectionName, serviceName)] = handleContext;
    }

    // All handle contexts locked, so safe
    ++handleContext->refcount;
}

void Connection::Private::introspectMain()
{
    // Introspecting the main interface is currently just calling
    // GetInterfaces(), but it might include other stuff in the future if we
    // gain GetAll-able properties on the connection
    debug() << "Calling GetInterfaces()";
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(baseInterface->GetInterfaces(), parent);
    parent->connect(watcher,
                    SIGNAL(finished(QDBusPendingCallWatcher *)),
                    SLOT(gotInterfaces(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectContacts()
{
    if (!properties) {
        properties = parent->propertiesInterface();
        Q_ASSERT(properties != 0);
    }

    debug() << "Getting available interfaces for GetContactAttributes";
    QDBusPendingCall call =
        properties->Get(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS,
                       "ContactAttributeInterfaces");
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(call, parent);
    parent->connect(watcher,
                    SIGNAL(finished(QDBusPendingCallWatcher *)),
                    SLOT(gotContactAttributeInterfaces(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectSimplePresence()
{
    if (!properties) {
        properties = parent->propertiesInterface();
        Q_ASSERT(properties != 0);
    }

    debug() << "Getting available SimplePresence statuses";
    QDBusPendingCall call =
        properties->Get(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE,
                "Statuses");
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(call, parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher *)),
            SLOT(gotSimpleStatuses(QDBusPendingCallWatcher *)));
}

void Connection::Private::introspectSelfContact()
{
    debug() << "Building self contact";
    // FIXME: these should be features when Connection is sanitized
    PendingContacts *contacts = contactManager->contactsForHandles(
            UIntList() << selfHandle,
            QSet<Contact::Feature>() << Contact::FeatureAlias
                                     << Contact::FeatureAvatarToken
                                     << Contact::FeatureSimplePresence);
    parent->connect(contacts,
            SIGNAL(finished(Telepathy::Client::PendingOperation *)),
            SLOT(gotSelfContact(Telepathy::Client::PendingOperation *)));
}

void Connection::Private::introspectSelfHandle()
{
    parent->connect(baseInterface,
                    SIGNAL(SelfHandleChanged(uint)),
                    SLOT(onSelfHandleChanged(uint)));

    debug() << "Getting self handle";
    QDBusPendingCall call = baseInterface->GetSelfHandle();
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(call, parent);
    parent->connect(watcher,
                    SIGNAL(finished(QDBusPendingCallWatcher *)),
                    SLOT(gotSelfHandle(QDBusPendingCallWatcher *)));
}

void Connection::Private::changeReadiness(Readiness newReadiness)
{
    debug() << "changing readiness from" << readiness <<
        "to" << newReadiness;
    Q_ASSERT(newReadiness != readiness);

    switch (readiness) {
        case ReadinessJustCreated:
            break;
        case ReadinessNotYetConnected:
            Q_ASSERT(newReadiness == ReadinessConnecting
                    || newReadiness == ReadinessDead);
            break;
        case ReadinessConnecting:
            Q_ASSERT(newReadiness == ReadinessFull
                    || newReadiness == ReadinessDead);
            break;
        case ReadinessFull:
            Q_ASSERT(newReadiness == ReadinessDead);
            break;
        case ReadinessDead:
            // clear up introspection queue, no need for that as the connection
            // is dead
            introspectQueue.clear();
        default:
            Q_ASSERT(false);
    }

    debug() << "Readiness changed from" << readiness << "to" << newReadiness;
    readiness = newReadiness;

    if (status != pendingStatus ||
        statusReason != pendingStatusReason) {
        status = pendingStatus;
        statusReason = pendingStatusReason;
        emit parent->statusChanged(status, statusReason);
    }
}

void Connection::Private::updatePendingOperations()
{
    foreach (Private::PendingReady *operation, pendingOperations) {
        if (ready &&
            ((operation->features &
                (features | missingFeatures)) == operation->features)) {
            operation->setFinished();
        }
        if (operation->isFinished()) {
            pendingOperations.removeOne(operation);
        }
    }
}

Connection::PendingConnect::PendingConnect(Connection *parent, Connection::Features features)
    : PendingOperation(parent),
      features(features)
{
    QDBusPendingCall call = parent->baseInterface()->Connect();
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, parent);
    connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            this,
            SLOT(onConnectReply(QDBusPendingCallWatcher*)));
}

void Connection::PendingConnect::onConnectReply(QDBusPendingCallWatcher *watcher)
{
    if (watcher->isError()) {
        setFinishedWithError(watcher->error());
    }
    else {
        connect(qobject_cast<Connection*>(parent())->becomeReady(features),
                SIGNAL(finished(Telepathy::Client::PendingOperation*)),
                SLOT(onBecomeReadyReply(Telepathy::Client::PendingOperation*)));
    }
}

void Connection::PendingConnect::onBecomeReadyReply(Telepathy::Client::PendingOperation *op)
{
    if (op->isError()) {
        setFinishedWithError(op->errorName(), op->errorMessage());
    }
    else {
        setFinished();
    }
}

QMap<QPair<QString, QString>, Connection::Private::HandleContext*> Connection::Private::handleContexts;
QMutex Connection::Private::handleContextsLock;

/**
 * \class Connection
 * \ingroup clientconn
 * \headerfile <TelepathyQt4/Client/connection.h> <TelepathyQt4/Client/Connection>
 *
 * Object representing a Telepathy connection.
 *
 * It adds the following features compared to using ConnectionInterface
 * directly:
 * <ul>
 *  <li>%Connection status tracking</li>
 *  <li>Getting the list of supported interfaces automatically</li>
 *  <li>Getting the valid presence statuses automatically</li>
 *  <li>Shared optional interface proxy instances</li>
 * </ul>
 *
 * The remote object state accessor functions on this object (status(),
 * statusReason(), and so on) don't make any DBus calls; instead,
 * they return values cached from a previous introspection run. The
 * introspection process populates their values in the most efficient way
 * possible based on what the service implements. Their return value is mostly
 * undefined until the introspection process is completed; a status change to
 * StatusConnected indicates that the introspection process is finished. See the
 * individual accessor descriptions for details on which functions can be used
 * in the different states.
 */

/**
 * Construct a new Connection object.
 *
 * \param serviceName Connection service name.
 * \param objectPath Connection object path.
 * \param parent Object parent.
 */
Connection::Connection(const QString &serviceName,
                       const QString &objectPath,
                       QObject *parent)
    : StatefulDBusProxy(QDBusConnection::sessionBus(),
            serviceName, objectPath, parent),
      OptionalInterfaceFactory<Connection>(this),
      mPriv(new Private(this))
{
    mPriv->introspectQueue.enqueue(&Private::startIntrospection);
    QTimer::singleShot(0, this, SLOT(continueIntrospection()));
}

/**
 * Construct a new Connection object.
 *
 * \param bus QDBusConnection to use.
 * \param serviceName Connection service name.
 * \param objectPath Connection object path.
 * \param parent Object parent.
 */
Connection::Connection(const QDBusConnection &bus,
                       const QString &serviceName,
                       const QString &objectPath,
                       QObject *parent)
    : StatefulDBusProxy(bus, serviceName, objectPath, parent),
      OptionalInterfaceFactory<Connection>(this),
      mPriv(new Private(this))
{
    mPriv->introspectQueue.enqueue(&Private::startIntrospection);
    QTimer::singleShot(0, this, SLOT(continueIntrospection()));
}

/**
 * Class destructor.
 */
Connection::~Connection()
{
    delete mPriv;
}

/**
 * Return the connection's status.
 *
 * The returned value may have changed whenever statusChanged() is
 * emitted.
 *
 * \return The status, as defined in Status.
 */
uint Connection::status() const
{
    if (mPriv->readiness == Private::ReadinessJustCreated) {
        warning() << "Connection::status() used with readiness ReadinessJustCreated";
    }

    return mPriv->status;
}

/**
 * Return the reason for the connection's status (which is returned by
 * status()). The validity and change rules are the same as for status().
 *
 * \return The reason, as defined in Status.
 */
uint Connection::statusReason() const
{
    if (mPriv->readiness == Private::ReadinessJustCreated) {
        warning() << "Connection::statusReason() used with readiness ReadinessJustCreated";
    }

    return mPriv->statusReason;
}

/**
 * Return a list of optional interfaces supported by this object. The
 * contents of the list is undefined unless the Connection has status
 * StatusConnecting or StatusConnected. The returned value stays
 * constant for the entire time the connection spends in each of these
 * states; however interfaces might have been added to the supported set by
 * the time StatusConnected is reached.
 *
 * \return Names of the supported interfaces.
 */
QStringList Connection::interfaces() const
{
    // Different check than the others, because the optional interface getters
    // may be used internally with the knowledge about getting the interfaces
    // list, so we don't want this to cause warnings.
    if (mPriv->readiness != Private::ReadinessNotYetConnected &&
        mPriv->readiness != Private::ReadinessFull &&
        mPriv->interfaces.empty()) {
        warning() << "Connection::interfaces() used possibly before the list of interfaces has been received";
    }
    else if (mPriv->readiness == Private::ReadinessDead) {
        warning() << "Connection::interfaces() used with readiness ReadinessDead";
    }

    return mPriv->interfaces;
}

/**
 * Return the handle which represents the user on this connection, which will remain
 * valid for the lifetime of this connection, or until a change in the user's
 * identifier is signalled by the selfHandleChanged signal. If the connection is
 * not yet in the StatusConnected state, the value of this property MAY be zero.
 *
 * \return Self handle.
 */
uint Connection::selfHandle() const
{
    return mPriv->selfHandle;
}

/**
 * Return a dictionary of presence statuses valid for use with the new(er)
 * Telepathy SimplePresence interface on the remote object.
 *
 * The value is undefined if the list returned by interfaces() doesn't
 * contain %TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE.
 *
 * The value may have changed arbitrarily during the time the
 * Connection spends in status StatusConnecting,
 * again staying fixed for the entire time in StatusConnected.
 *
 * \return Dictionary from string identifiers to structs for each valid
 * status.
 */
SimpleStatusSpecMap Connection::allowedPresenceStatuses() const
{
    if (mPriv->missingFeatures & FeatureSimplePresence) {
        warning() << "Trying to retrieve simple presence from connection, but "
                     "simple presence is not supported";
    }
    else if (!(mPriv->features & FeatureSimplePresence)) {
        warning() << "Trying to retrieve simple presence from connection without "
                     "calling Connection::becomeReady(FeatureSimplePresence)";
    }
    else if (mPriv->pendingFeatures & FeatureSimplePresence) {
        warning() << "Trying to retrieve simple presence from connection, but "
                     "simple presence is still being retrieved";
    }

    return mPriv->simplePresenceStatuses;
}

/**
 * Set the self presence status.
 *
 * \a status must be one of the allowed statuses returned by
 * allowedPresenceStatuses().
 *
 * Note that clients SHOULD set the status message for the local user to the empty string,
 * unless the user has actually provided a specific message (i.e. one that
 * conveys more information than the Status).
 *
 * \param status The desired status.
 * \param statusMessage The desired status message.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 *
 * \sa allowedPresenceStatuses()
 */
PendingOperation *Connection::setSelfPresence(const QString &status,
        const QString &statusMessage)
{
    if (!mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE)) {
        return new PendingFailure(this, TELEPATHY_ERROR_NOT_IMPLEMENTED,
                "Connection does not support SimplePresence");
    }
    return new PendingVoidMethodCall(this,
            simplePresenceInterface()->SetPresence(status, statusMessage));
}

QSharedPointer<Contact> Connection::selfContact() const
{
    if (!isReady()) {
        warning() << "Connection::selfContact() used before the connection is ready!";
    }
    // FIXME: add checks for the SelfContact feature having been requested when Connection features
    // actually work sensibly and selfContact is made a feature

    return mPriv->selfContact;
}

/**
 * \fn Connection::optionalInterface(InterfaceSupportedChecking check) const
 *
 * Get a pointer to a valid instance of a given %Connection optional
 * interface class, associated with the same remote object the Connection is
 * associated with, and destroyed at the same time the Connection is
 * destroyed.
 *
 * If the list returned by interfaces() doesn't contain the name of the
 * interface requested <code>0</code> is returned. This check can be
 * bypassed by specifying #BypassInterfaceCheck for <code>check</code>, in
 * which case a valid instance is always returned.
 *
 * If the object is not ready, the list returned by interfaces() isn't
 * guaranteed to yet represent the full set of interfaces supported by the
 * remote object.
 * Hence the check might fail even if the remote object actually supports
 * the requested interface; using #BypassInterfaceCheck is suggested when
 * the Connection is not suitably ready.
 *
 * \sa OptionalInterfaceFactory::interface
 *
 * \tparam Interface Class of the optional interface to get.
 * \param check Should an instance be returned even if it can't be
 *              determined that the remote object supports the
 *              requested interface.
 * \return Pointer to an instance of the interface class, or <code>0</code>.
 */

/**
 * \fn ConnectionInterfaceAliasingInterface *Connection::aliasingInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting an Aliasing interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceAliasingInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfaceAvatarsInterface *Connection::avatarsInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting an Avatars interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceAvatarsInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfaceCapabilitiesInterface *Connection::capabilitiesInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting a Capabilities interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceCapabilitiesInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfacePresenceInterface *Connection::presenceInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting a Presence interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfacePresenceInterface>(check)</code>
 */

/**
 * \fn ConnectionInterfaceSimplePresenceInterface *Connection::simplePresenceInterface(InterfaceSupportedChecking check) const
 *
 * Convenience function for getting a SimplePresence interface proxy.
 *
 * \param check Passed to optionalInterface()
 * \return <code>optionalInterface<ConnectionInterfaceSimplePresenceInterface>(check)</code>
 */

/**
 * \fn DBus::PropertiesInterface *Connection::propertiesInterface() const
 *
 * Convenience function for getting a Properties interface proxy. The
 * Properties interface is not necessarily reported by the services, so a
 * <code>check</code> parameter is not provided, and the interface is
 * always assumed to be present.
 *
 * \sa optionalInterface()
 *
 * \return <code>optionalInterface<DBus::PropertiesInterface>(BypassInterfaceCheck)</code>
 */

void Connection::onStatusChanged(uint status, uint reason)
{
    debug() << "StatusChanged from" << mPriv->status
            << "to" << status << "with reason" << reason;

    if (!mPriv->haveInitialStatus) {
        debug() << "Still haven't got the GetStatus reply, ignoring StatusChanged until we have (but saving reason)";
        mPriv->pendingStatusReason = reason;
        return;
    }

    if (mPriv->pendingStatus == status) {
        warning() << "New status was the same as the old status! Ignoring redundant StatusChanged";
        return;
    }

    if (status == ConnectionStatusConnected &&
        mPriv->pendingStatus != ConnectionStatusConnecting) {
        // CMs aren't meant to go straight from Disconnected to
        // Connected; recover by faking Connecting
        warning() << " Non-compliant CM - went straight to Connected! Faking a transition through Connecting";
        onStatusChanged(ConnectionStatusConnecting, reason);
    }

    mPriv->pendingStatus = status;
    mPriv->pendingStatusReason = reason;

    switch (status) {
        case ConnectionStatusConnected:
            debug() << " Performing introspection for the Connected status";
            mPriv->introspectQueue.enqueue(&Private::introspectMain);
            continueIntrospection();
            break;

        case ConnectionStatusConnecting:
            if (mPriv->readiness < Private::ReadinessConnecting) {
                mPriv->changeReadiness(Private::ReadinessConnecting);
            }
            else {
                warning() << " Got unexpected status change to Connecting";
            }
            break;

        case ConnectionStatusDisconnected:
            if (mPriv->readiness != Private::ReadinessDead) {
                const char *errorName;

                // This is the best we can do right now: in an imminent
                // spec version we should define a different D-Bus error name
                // for each ConnectionStatusReason

                switch (reason) {
                    case ConnectionStatusReasonNoneSpecified:
                    case ConnectionStatusReasonRequested:
                        errorName = TELEPATHY_ERROR_DISCONNECTED;
                        break;

                    case ConnectionStatusReasonNetworkError:
                    case ConnectionStatusReasonAuthenticationFailed:
                    case ConnectionStatusReasonEncryptionError:
                        errorName = TELEPATHY_ERROR_NETWORK_ERROR;
                        break;

                    case ConnectionStatusReasonNameInUse:
                        errorName = TELEPATHY_ERROR_NOT_YOURS;
                        break;

                    case ConnectionStatusReasonCertNotProvided:
                    case ConnectionStatusReasonCertUntrusted:
                    case ConnectionStatusReasonCertExpired:
                    case ConnectionStatusReasonCertNotActivated:
                    case ConnectionStatusReasonCertHostnameMismatch:
                    case ConnectionStatusReasonCertFingerprintMismatch:
                    case ConnectionStatusReasonCertSelfSigned:
                    case ConnectionStatusReasonCertOtherError:
                        errorName = TELEPATHY_ERROR_NETWORK_ERROR;

                    default:
                        errorName = TELEPATHY_ERROR_DISCONNECTED;
                }

                invalidate(QLatin1String(errorName),
                        QString("ConnectionStatusReason = %1").arg(uint(reason)));

                mPriv->changeReadiness(Private::ReadinessDead);
            }
            else {
                warning() << " Got unexpected status change to Disconnected";
            }
            break;

        default:
            warning() << "Unknown connection status" << status;
            break;
    }
}

void Connection::gotStatus(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<uint> reply = *watcher;

    if (reply.isError()) {
        warning().nospace() << "GetStatus() failed with " <<
            reply.error().name() << ":" << reply.error().message();
        invalidate(QLatin1String(TELEPATHY_ERROR_DISCONNECTED),
                QString("ConnectionStatusReason = %1").arg(uint(mPriv->pendingStatusReason)));
        mPriv->changeReadiness(Private::ReadinessDead);
        return;
    }

    uint status = reply.value();

    debug() << "Got connection status" << status;
    mPriv->pendingStatus = status;
    mPriv->haveInitialStatus = true;

    // Don't do any introspection yet if the connection is in the Connecting
    // state; the StatusChanged handler will take care of doing that, if the
    // connection ever gets to the Connected state.
    if (status == ConnectionStatusConnecting) {
        debug() << "Not introspecting yet because the connection is currently Connecting";
        mPriv->changeReadiness(Private::ReadinessConnecting);
        return;
    }

    if (status == ConnectionStatusDisconnected) {
        debug() << "Performing introspection for the Disconnected status";
        mPriv->initialIntrospection = true;
    }
    else {
        if (status != ConnectionStatusConnected) {
            warning() << "Not performing introspection for unknown status" << status;
            return;
        }
        else {
            debug() << "Performing introspection for the Connected status";
        }
    }

    mPriv->introspectQueue.enqueue(&Private::introspectMain);

    continueIntrospection();

    watcher->deleteLater();
}

void Connection::gotInterfaces(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QStringList> reply = *watcher;

    if (!reply.isError()) {
        debug() << "Got reply to GetInterfaces():" << mPriv->interfaces;
        mPriv->interfaces = reply.value();

        if (mPriv->pendingStatus == ConnectionStatusConnected) {
            mPriv->introspectQueue.enqueue(&Private::introspectSelfHandle);
        } else {
            debug() << "Connection basic functionality is ready";
            mPriv->ready = true;
        }

        // if FeatureSimplePresence was requested and the interface exists and
        // the introspect func is not already enqueued, enqueue it.
        if (mPriv->pendingFeatures & FeatureSimplePresence &&
            mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE) &&
            !mPriv->introspectQueue.contains(&Private::introspectSimplePresence)) {
            mPriv->introspectQueue.enqueue(&Private::introspectSimplePresence);
        }
    }
    else {
        warning().nospace() << "GetInterfaces() failed with " <<
            reply.error().name() << ":" << reply.error().message() <<
            " - assuming no new interfaces";
    }

    continueIntrospection();

    watcher->deleteLater();
}

void Connection::gotContactAttributeInterfaces(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QDBusVariant> reply = *watcher;

    // This is junk, but need to do this to make ContactManager behave - ideally, the self contact
    // wouldn't be core, but CAI would, so isReady(<nothing but core>) should return true already
    // at this point, and ContactManager should be happy
    debug() << "Connection basic functionality is ready (Got CAI)";
    mPriv->ready = true;

    if (!reply.isError()) {
        mPriv->contactAttributeInterfaces = qdbus_cast<QStringList>(reply.value().variant());
        debug() << "Got" << mPriv->contactAttributeInterfaces.size() << "contact attribute interfaces";
        mPriv->introspectQueue.enqueue(&Private::introspectSelfContact);
    } else {
        warning().nospace() << "Getting contact attribute interfaces failed with " <<
            reply.error().name() << ":" << reply.error().message();
    }

    continueIntrospection();

    watcher->deleteLater();
}

void Connection::gotSelfContact(PendingOperation *op)
{
    PendingContacts *pending = qobject_cast<PendingContacts *>(op);

    debug() << "Connection basic functionality is ready (Got SelfContact)";
    mPriv->ready = true;

    if (pending->isValid()) {
        Q_ASSERT(pending->contacts().size() == 1);
        QSharedPointer<Contact> contact = pending->contacts()[0];
        if (mPriv->selfContact != contact) {
            mPriv->selfContact = contact;
            emit selfContactChanged();
        }
    } else {
        warning().nospace() << "Getting self contact failed with " <<
            pending->errorName() << ":" << pending->errorMessage();
    }

    continueIntrospection();
}

void Connection::gotSimpleStatuses(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QDBusVariant> reply = *watcher;

    mPriv->pendingFeatures &= ~FeatureSimplePresence;

    if (!reply.isError()) {
        mPriv->features |= FeatureSimplePresence;
        debug() << "Adding FeatureSimplePresence to features";

        mPriv->simplePresenceStatuses = qdbus_cast<SimpleStatusSpecMap>(reply.value().variant());
        debug() << "Got" << mPriv->simplePresenceStatuses.size() << "simple presence statuses";
    }
    else {
        mPriv->missingFeatures |= FeatureSimplePresence;
        debug() << "Adding FeatureSimplePresence to missing features";

        warning().nospace() << "Getting simple presence statuses failed with " <<
            reply.error().name() << ":" << reply.error().message();
    }

    continueIntrospection();

    watcher->deleteLater();
}

void Connection::gotSelfHandle(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<uint> reply = *watcher;

    if (!reply.isError()) {
        mPriv->selfHandle = reply.value();
        debug() << "Got self handle" << mPriv->selfHandle;
    } else {
        warning().nospace() << "Getting self handle failed with " <<
            reply.error().name() << ":" << reply.error().message();
    }

    if (mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS)) {
        mPriv->introspectQueue.enqueue(&Private::introspectContacts);
    } else {
        debug() << "Connection basic functionality is ready (Don't have Contacts)";
        mPriv->ready = true;
    }

    continueIntrospection();

    watcher->deleteLater();
}

/**
 * Get the ConnectionInterface for this Connection. This
 * method is protected since the convenience methods provided by this
 * class should generally be used instead of calling D-Bus methods
 * directly.
 *
 * \return A pointer to the existing ConnectionInterface for this
 *         Connection.
 */
ConnectionInterface *Connection::baseInterface() const
{
    return mPriv->baseInterface;
}

/**
 * Asynchronously creates a channel satisfying the given request.
 *
 * The request MUST contain the following keys:
 *   org.freedesktop.Telepathy.Account.ChannelType
 *   org.freedesktop.Telepathy.Account.TargetHandleType
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingChannel object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingChannel object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingChannel objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingChannel object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingChannel
 *
 * \param request A dictionary containing the desirable properties.
 * \return Pointer to a newly constructed PendingChannel object, tracking
 *         the progress of the request.
 */
PendingChannel *Connection::createChannel(const QVariantMap &request)
{
    if (mPriv->readiness != Private::ReadinessFull) {
        warning() << "Calling createChannel with connection not yet connected";
        return new PendingChannel(this, TELEPATHY_ERROR_NOT_AVAILABLE,
                "Connection not yet connected");
    }

    if (!mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS)) {
        warning() << "Requests interface is not support by this connection";
        return new PendingChannel(this, TELEPATHY_ERROR_NOT_IMPLEMENTED,
                "Connection does not support Requests Interface");
    }

    if (!request.contains(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"))) {
        return new PendingChannel(this, TELEPATHY_ERROR_INVALID_ARGUMENT,
                "Invalid 'request' argument");
    }

    debug() << "Creating a Channel";
    PendingChannel *channel =
        new PendingChannel(this, request, true);
    return channel;
}

/**
 * Asynchronously ensures a channel exists satisfying the given request.
 *
 * The request MUST contain the following keys:
 *   org.freedesktop.Telepathy.Account.ChannelType
 *   org.freedesktop.Telepathy.Account.TargetHandleType
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingChannel object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingChannel object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingChannel objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingChannel object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingChannel
 *
 * \param request A dictionary containing the desirable properties.
 * \return Pointer to a newly constructed PendingChannel object, tracking
 *         the progress of the request.
 */
PendingChannel *Connection::ensureChannel(const QVariantMap &request)
{
    if (mPriv->readiness != Private::ReadinessFull) {
        warning() << "Calling ensureChannel with connection not yet connected";
        return new PendingChannel(this, TELEPATHY_ERROR_NOT_AVAILABLE,
                "Connection not yet connected");
    }

    if (!mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_REQUESTS)) {
        warning() << "Requests interface is not support by this connection";
        return new PendingChannel(this, TELEPATHY_ERROR_NOT_IMPLEMENTED,
                "Connection does not support Requests Interface");
    }

    if (!request.contains(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"))) {
        return new PendingChannel(this, TELEPATHY_ERROR_INVALID_ARGUMENT,
                "Invalid 'request' argument");
    }

    debug() << "Creating a Channel";
    PendingChannel *channel =
        new PendingChannel(this, request, false);
    return channel;
}

/**
 * Request handles of the given type for the given entities (contacts,
 * rooms, lists, etc.).
 *
 * Upon completion, the reply to the request can be retrieved through the
 * returned PendingHandles object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingHandles object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingHandles objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingHandles object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingHandles
 *
 * \param handleType Type for the handles to request, as specified in
 *                   #HandleType.
 * \param names Names of the entities to request handles for.
 * \return Pointer to a newly constructed PendingHandles object, tracking
 *         the progress of the request.
 */
PendingHandles *Connection::requestHandles(uint handleType, const QStringList &names)
{
    debug() << "Request for" << names.length() << "handles of type" << handleType;

    {
        Private::HandleContext *handleContext = mPriv->handleContext;
        QMutexLocker locker(&handleContext->lock);
        handleContext->types[handleType].requestsInFlight++;
    }

    PendingHandles *pending =
        new PendingHandles(this, handleType, names);
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(
                mPriv->baseInterface->RequestHandles(handleType, names),
                pending);

    pending->connect(watcher,
                     SIGNAL(finished(QDBusPendingCallWatcher *)),
                     SLOT(onCallFinished(QDBusPendingCallWatcher *)));

    return pending;
}

/**
 * Request a reference to the given handles. Handles not explicitly
 * requested (via requestHandles()) but eg. observed in a signal need to be
 * referenced to guarantee them staying valid.
 *
 * Upon completion, the reply to the operation can be retrieved through the
 * returned PendingHandles object. The object also provides access to the
 * parameters with which the call was made and a signal to connect to to get
 * notification of the request finishing processing. See the documentation
 * for that class for more info.
 *
 * The returned PendingHandles object should be freed using
 * its QObject::deleteLater() method after it is no longer used. However,
 * all PendingHandles objects resulting from requests to a particular
 * Connection will be freed when the Connection itself is freed. Conversely,
 * this means that the PendingHandles object should not be used after the
 * Connection is destroyed.
 *
 * \sa PendingHandles
 *
 * \param handleType Type of the handles given, as specified in #HandleType.
 * \param handles Handles to request a reference to.
 * \return Pointer to a newly constructed PendingHandles object, tracking
 *         the progress of the request.
 */
PendingHandles *Connection::referenceHandles(uint handleType, const UIntList &handles)
{
    debug() << "Reference of" << handles.length() << "handles of type" << handleType;

    UIntList alreadyHeld;
    UIntList notYetHeld;
    {
        Private::HandleContext *handleContext = mPriv->handleContext;
        QMutexLocker locker(&handleContext->lock);

        foreach (uint handle, handles) {
            if (handleContext->types[handleType].refcounts.contains(handle) ||
                handleContext->types[handleType].toRelease.contains(handle)) {
                alreadyHeld.push_back(handle);
            }
            else {
                notYetHeld.push_back(handle);
            }
        }
    }

    debug() << " Already holding" << alreadyHeld.size() <<
        "of the handles -" << notYetHeld.size() << "to go";

    PendingHandles *pending =
        new PendingHandles(this, handleType, handles, alreadyHeld);

    if (!notYetHeld.isEmpty()) {
        debug() << " Calling HoldHandles";

        QDBusPendingCallWatcher *watcher =
            new QDBusPendingCallWatcher(
                    mPriv->baseInterface->HoldHandles(handleType, notYetHeld),
                    pending);

        pending->connect(watcher,
                         SIGNAL(finished(QDBusPendingCallWatcher *)),
                         SLOT(onCallFinished(QDBusPendingCallWatcher *)));
    }
    else {
        debug() << " All handles already held, not calling HoldHandles";
    }

    return pending;
}

/**
 * Return whether this object has finished its initial setup.
 *
 * This is mostly useful as a sanity check, in code that shouldn't be run
 * until the object is ready. To wait for the object to be ready, call
 * becomeReady() and connect to the finished signal on the result.
 *
 * \param features Which features should be tested.
 * \return \c true if the object has finished initial setup.
 */
bool Connection::isReady(Features features) const
{
    return mPriv->ready
        && ((mPriv->features & features) == features);
}

/**
 * Return a pending operation which will succeed when this object finishes
 * its initial setup, or will fail if a fatal error occurs during this
 * initial setup.
 *
 * \param features Which features should be tested.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when this object has finished or failed its initial setup.
 */
PendingOperation *Connection::becomeReady(Features requestedFeatures)
{
    if (isReady(requestedFeatures)) {
        return new PendingSuccess(this);
    }

    debug() << "Calling becomeReady with requested features:"
            << requestedFeatures;
    foreach (Private::PendingReady *operation, mPriv->pendingOperations) {
        if (operation->features == requestedFeatures) {
            debug() << "Returning cached pending operation";
            return operation;
        }
    }

    if (requestedFeatures & FeatureSimplePresence) {
        // as the feature is optional, if it's know to not be supported,
        // just finish silently
        if (requestedFeatures == FeatureSimplePresence &&
            mPriv->missingFeatures & FeatureSimplePresence) {
            return new PendingSuccess(this);
        }

        // if we already have the interface simple presence enqueue the call to
        // introspect simple presence, otherwise it will be enqueued when/if the
        // interface is available
        if (!(mPriv->features & FeatureSimplePresence) &&
            !(mPriv->pendingFeatures & FeatureSimplePresence) &&
            !(mPriv->missingFeatures & FeatureSimplePresence) &&
            mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE)) {
            mPriv->introspectQueue.enqueue(&Private::introspectSimplePresence);

            // FIXME: Is not particularly good... this might introspect something completely
            // unrelated (the head of the queue) at the wrong time
            QTimer::singleShot(0, this, SLOT(continueIntrospection()));
        } else {
            if (mPriv->readiness == Private::ReadinessFull) {
                // we don't support simple presence but we are online, so
                // we should have all interfaces now, so if simple presence is not
                // present, add it to missing features.
                mPriv->missingFeatures |= FeatureSimplePresence;
            }
        }
    }

    mPriv->pendingFeatures |= requestedFeatures;

    debug() << "Creating new pending operation";
    Private::PendingReady *operation =
        new Private::PendingReady(requestedFeatures, this);
    mPriv->pendingOperations.append(operation);

    mPriv->updatePendingOperations();
    return operation;
}

/**
 * Start an asynchronous request that the connection be connected.
 *
 * The returned PendingOperation will finish successfully when the connection
 * has reached StatusConnected and the requested \a features are all ready, or
 * finish with an error if a fatal error occurs during that process.
 *
 * \return A %PendingOperation, which will emit finished when the
 *         request finishes.
 */
PendingOperation *Connection::requestConnect(Connection::Features features)
{
    return new PendingConnect(this, features);
}

/**
 * Start an asynchronous request that the connection be disconnected.
 * The returned PendingOperation object will signal the success or failure
 * of this request; under normal circumstances, it can be expected to
 * succeed.
 *
 * \return A %PendingOperation, which will emit finished when the
 *         request finishes.
 */
PendingOperation *Connection::requestDisconnect()
{
    return new PendingVoidMethodCall(this, baseInterface()->Disconnect());
}

/**
 * Requests attributes for contacts. Optionally, the handles of the contacts will be referenced
 * automatically. Essentially, this method wraps
 * ConnectionInterfaceContactsInterface::GetContactAttributes(), integrating it with the rest of the
 * handle-referencing machinery.
 *
 * Upon completion, the reply to the request can be retrieved through the returned
 * PendingContactAttributes object. The object also provides access to the parameters with which the
 * call was made and a signal to connect to to get notification of the request finishing processing.
 * See the documentation for that class for more info.
 *
 * If the remote object doesn't support the Contacts interface (as signified by the list returned by
 * interfaces() not containing TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS), the returned
 * PendingContactAttributes instance will fail instantly with the error
 * TELEPATHY_ERROR_NOT_IMPLEMENTED.
 *
 * Similarly, if the connection isn't both connected and ready (<code>status() == StatusConnected &&
 * isReady()</code>), the returned PendingContactAttributes instance will fail instantly with the
 * error TELEPATHY_ERROR_NOT_AVAILABLE.
 *
 * \sa PendingContactAttributes
 *
 * \param contacts Passed to ConnectionInterfaceContactsInterface::GetContactAttributes().
 * \param interfaces Passed to ConnectionInterfaceContactsInterface::GetContactAttributes().
 * \param reference Whether the handles should additionally be referenced.
 * \return Pointer to a newly constructed PendingContactAttributes, tracking the progress of the
 *         request.
 */
PendingContactAttributes *Connection::getContactAttributes(const UIntList &handles,
        const QStringList &interfaces, bool reference)
{
    debug() << "Request for attributes for" << handles.size() << "contacts";

    PendingContactAttributes *pending =
        new PendingContactAttributes(this, handles, interfaces, reference);
    if (!isReady()) {
        warning() << "Connection::getContactAttributes() used when not ready";
        pending->failImmediately(TELEPATHY_ERROR_NOT_AVAILABLE, "The connection isn't ready");
        return pending;
    } /* FIXME: readd this check when Connection isn't FSCKING broken anymore: else if (status() != StatusConnected) {
        warning() << "Connection::getContactAttributes() used with status" << status() << "!= StatusConnected";
        pending->failImmediately(TELEPATHY_ERROR_NOT_AVAILABLE,
                "The connection isn't Connected");
        return pending;
    } */else if (!this->interfaces().contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS)) {
        warning() << "Connection::getContactAttributes() used without the remote object supporting"
                  << "the Contacts interface";
        pending->failImmediately(TELEPATHY_ERROR_NOT_IMPLEMENTED,
                "The connection doesn't support the Contacts interface");
        return pending;
    }

    {
        Private::HandleContext *handleContext = mPriv->handleContext;
        QMutexLocker locker(&handleContext->lock);
        handleContext->types[HandleTypeContact].requestsInFlight++;
    }

    ConnectionInterfaceContactsInterface *contactsInterface =
        optionalInterface<ConnectionInterfaceContactsInterface>();
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(contactsInterface->GetContactAttributes(handles, interfaces,
                    reference));
    pending->connect(watcher, SIGNAL(finished(QDBusPendingCallWatcher*)),
                              SLOT(onCallFinished(QDBusPendingCallWatcher*)));
    return pending;
}

QStringList Connection::contactAttributeInterfaces() const
{
    if (!isReady()) {
        warning() << "Connection::contactAttributeInterfaces() used when not ready";
    } else if (status() != StatusConnected) {
        warning() << "Connection::contactAttributeInterfaces() used with status"
            << status() << "!= StatusConnected";
    } else if (!this->interfaces().contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS)) {
        warning() << "Connection::contactAttributeInterfaces() used without the remote object supporting"
                  << "the Contacts interface";
    }

    return mPriv->contactAttributeInterfaces;
}

ContactManager *Connection::contactManager() const
{
    return mPriv->contactManager;
}

void Connection::refHandle(uint type, uint handle)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    if (handleContext->types[type].toRelease.contains(handle)) {
        handleContext->types[type].toRelease.remove(handle);
    }

    handleContext->types[type].refcounts[handle]++;
}

void Connection::unrefHandle(uint type, uint handle)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    Q_ASSERT(handleContext->types.contains(type));
    Q_ASSERT(handleContext->types[type].refcounts.contains(handle));

    if (!--handleContext->types[type].refcounts[handle]) {
        handleContext->types[type].refcounts.remove(handle);
        handleContext->types[type].toRelease.insert(handle);

        if (!handleContext->types[type].releaseScheduled) {
            if (!handleContext->types[type].requestsInFlight) {
                debug() << "Lost last reference to at least one handle of type" <<
                    type << "and no requests in flight for that type - scheduling a release sweep";
                QMetaObject::invokeMethod(this, "doReleaseSweep",
                        Qt::QueuedConnection, Q_ARG(uint, type));
                handleContext->types[type].releaseScheduled = true;
            }
        }
    }
}

void Connection::doReleaseSweep(uint type)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    Q_ASSERT(handleContext->types.contains(type));
    Q_ASSERT(handleContext->types[type].releaseScheduled);

    debug() << "Entering handle release sweep for type" << type;
    handleContext->types[type].releaseScheduled = false;

    if (handleContext->types[type].requestsInFlight > 0) {
        debug() << " There are requests in flight, deferring sweep to when they have been completed";
        return;
    }

    if (handleContext->types[type].toRelease.isEmpty()) {
        debug() << " No handles to release - every one has been resurrected";
        return;
    }

    debug() << " Releasing" << handleContext->types[type].toRelease.size() << "handles";

    mPriv->baseInterface->ReleaseHandles(type, handleContext->types[type].toRelease.toList());
    handleContext->types[type].toRelease.clear();
}

void Connection::handleRequestLanded(uint type)
{
    Private::HandleContext *handleContext = mPriv->handleContext;
    QMutexLocker locker(&handleContext->lock);

    Q_ASSERT(handleContext->types.contains(type));
    Q_ASSERT(handleContext->types[type].requestsInFlight > 0);

    if (!--handleContext->types[type].requestsInFlight &&
        !handleContext->types[type].toRelease.isEmpty() &&
        !handleContext->types[type].releaseScheduled) {
        debug() << "All handle requests for type" << type <<
            "landed and there are handles of that type to release - scheduling a release sweep";
        QMetaObject::invokeMethod(this, "doReleaseSweep", Qt::QueuedConnection, Q_ARG(uint, type));
        handleContext->types[type].releaseScheduled = true;
    }
}

void Connection::continueIntrospection()
{
    if (mPriv->introspectQueue.isEmpty()) {
        if (mPriv->initialIntrospection) {
            mPriv->initialIntrospection = false;
            if (mPriv->readiness < Private::ReadinessNotYetConnected) {
                mPriv->changeReadiness(Private::ReadinessNotYetConnected);
            }
        }
        else {
            if (mPriv->readiness != Private::ReadinessDead &&
                mPriv->readiness != Private::ReadinessFull) {
                mPriv->changeReadiness(Private::ReadinessFull);

                // we should have all interfaces now, so if an interface is not
                // present and we have a feature for it, add the feature to missing
                // features.
                if (!mPriv->interfaces.contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE)) {
                    debug() << "removing FeatureSimplePresence from pending features";
                    mPriv->pendingFeatures &= ~FeatureSimplePresence;
                    debug() << "adding FeatureSimplePresence to missing features";
                    mPriv->missingFeatures |= FeatureSimplePresence;
                }
                else {
                    // the user requested for FeatureSimplePresence so, now we are
                    // able to get it
                    if (mPriv->pendingFeatures == FeatureSimplePresence) {
                        mPriv->introspectQueue.enqueue(&Private::introspectSimplePresence);
                    }
                }
            }
        }
    }
    else {
        (mPriv->*(mPriv->introspectQueue.dequeue()))();
    }

    mPriv->updatePendingOperations();
}

void Connection::onSelfHandleChanged(uint handle)
{
    mPriv->selfHandle = handle;
    emit selfHandleChanged(handle);

    // FIXME: not ideal - when SelfContact is a feature, should check
    // actualFeatures().contains(SelfContact) instead
    // Also, when we figure out how the Renaming interface should work and how that should map to
    // Contact objects, this might not need any special handling anymore.
    if (interfaces().contains(TELEPATHY_INTERFACE_CONNECTION_INTERFACE_CONTACTS)) {
        mPriv->introspectSelfContact();
    }
}

}
}

#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtDBus/QtDBus>
#include <QtTest/QtTest>

#include <QDateTime>
#include <QString>
#include <QVariantMap>

#include <TelepathyQt4/Connection>
#include <TelepathyQt4/ConnectionFactory>
#include <TelepathyQt4/ContactFactory>
#include <TelepathyQt4/ChannelFactory>
#include <TelepathyQt4/Debug>
#include <TelepathyQt4/PendingReady>
#include <TelepathyQt4/Types>

#include <TelepathyQt4/test-backdoors.h>

#include <telepathy-glib/debug.h>

#include <glib-object.h>
#include <dbus/dbus-glib.h>

#include <tests/lib/glib/contacts-conn.h>
#include <tests/lib/test.h>

using namespace Tp;

class TestDBusProxyFactory : public Test
{
    Q_OBJECT

public:
    TestDBusProxyFactory(QObject *parent = 0)
        : Test(parent),
          mConnService1(0), mConnService2(0)
    { }

protected Q_SLOTS:
    void expectFinished();

private Q_SLOTS:
    void initTestCase();
    void init();

    void testCaching();
    void testDropRefs();
    void testInvalidate();

    void cleanup();
    void cleanupTestCase();

private:
    TpTestsContactsConnection *mConnService1, *mConnService2;
    QString mConnPath1, mConnPath2;
    QString mConnName1, mConnName2;
    ConnectionFactoryPtr mFactory;
    uint mNumFinished;
};

void TestDBusProxyFactory::expectFinished()
{
    mNumFinished++;
}

void TestDBusProxyFactory::initTestCase()
{
    initTestCaseImpl();

    g_type_init();
    g_set_prgname("dbus-proxy-factory");
    tp_debug_set_flags("all");
    dbus_g_bus_get(DBUS_BUS_STARTER, 0);

    gchar *name;
    gchar *connPath;
    GError *error = 0;

    mConnService1 = TP_TESTS_CONTACTS_CONNECTION(g_object_new(
            TP_TESTS_TYPE_CONTACTS_CONNECTION,
            "account", "me1@example.com",
            "protocol", "simple",
            NULL));
    QVERIFY(mConnService1 != 0);
    QVERIFY(tp_base_connection_register(TP_BASE_CONNECTION(mConnService1),
                "contacts", &name, &connPath, &error));
    QVERIFY(error == 0);

    QVERIFY(name != 0);
    QVERIFY(connPath != 0);

    mConnName1 = QLatin1String(name);
    mConnPath1 = QLatin1String(connPath);

    g_free(name);
    g_free(connPath);

    mConnService2 = TP_TESTS_CONTACTS_CONNECTION(g_object_new(
            TP_TESTS_TYPE_CONTACTS_CONNECTION,
            "account", "me2@example.com",
            "protocol", "simple",
            NULL));
    QVERIFY(mConnService2 != 0);
    QVERIFY(tp_base_connection_register(TP_BASE_CONNECTION(mConnService2),
                "contacts", &name, &connPath, &error));
    QVERIFY(error == 0);

    QVERIFY(name != 0);
    QVERIFY(connPath != 0);

    mConnName2 = QLatin1String(name);
    mConnPath2 = QLatin1String(connPath);

    g_free(name);
    g_free(connPath);
}

void TestDBusProxyFactory::init()
{
    initImpl();

    mFactory = ConnectionFactory::create(QDBusConnection::sessionBus(),
            Connection::FeatureCore);
    mNumFinished = 0;
}

void TestDBusProxyFactory::testCaching()
{
    PendingReady *first = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(first != NULL);
    QVERIFY(!first->proxy().isNull());

    PendingReady *same = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(same != NULL);
    QVERIFY(!same->proxy().isNull());

    QCOMPARE(same->proxy().data(), first->proxy().data());

    PendingReady *different = mFactory->proxy(mConnName2, mConnPath2,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(different != NULL);
    QVERIFY(!different->proxy().isNull());

    QVERIFY(different->proxy() != first->proxy());

    ConnectionPtr firstProxy = ConnectionPtr::dynamicCast(first->proxy());

    QVERIFY(!first->isFinished() && !same->isFinished() && !different->isFinished());
    QVERIFY(connect(first, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectFinished())));
    QVERIFY(connect(same, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectFinished())));
    QVERIFY(connect(different, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectFinished())));
    QVERIFY(!first->isFinished() && !same->isFinished() && !different->isFinished());

    while (mNumFinished < 3) {
        mLoop->processEvents();
    }

    QCOMPARE(mNumFinished, 3U);

    PendingReady *another = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(another != NULL);
    QVERIFY(!another->proxy().isNull());

    // Should still be the same even if all the initial requests already finished
    QCOMPARE(another->proxy().data(), firstProxy.data());

    QVERIFY(connect(another, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectSuccessfulCall(Tp::PendingOperation*))));
    QCOMPARE(mLoop->exec(), 0);
}

void TestDBusProxyFactory::testDropRefs()
{
    PendingReady *first = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(first != NULL);
    QVERIFY(!first->proxy().isNull());
    ConnectionPtr firstProxy = ConnectionPtr::dynamicCast(first->proxy());

    QVERIFY(firstProxy->isValid());

    QVERIFY(connect(first, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectSuccessfulCall(Tp::PendingOperation*))));
    QCOMPARE(mLoop->exec(), 0);

    PendingReady *same = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(same != NULL);
    QVERIFY(!same->proxy().isNull());

    // The first one is in scope so we should've got it again
    QCOMPARE(same->proxy().data(), firstProxy.data());

    QVERIFY(connect(same, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectSuccessfulCall(Tp::PendingOperation*))));
    QCOMPARE(mLoop->exec(), 0);

    // Flush the delete event for the PendingReady, which drops the PendingReady ref to the proxy
    mLoop->processEvents();

    // Make the Conn go out of scope
    Connection *firstPtr = firstProxy.data();
    firstProxy.reset();

    PendingReady *different = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(different != NULL);
    QVERIFY(!different->proxy().isNull());

    // The first one has gone out of scope and deleted so we should've got a different one
    QVERIFY(different->proxy().data() != firstPtr);
}

void TestDBusProxyFactory::testInvalidate()
{
    PendingReady *first = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(first != NULL);
    QVERIFY(!first->proxy().isNull());
    ConnectionPtr firstProxy = ConnectionPtr::dynamicCast(first->proxy());

    QVERIFY(firstProxy->isValid());

    QVERIFY(connect(first, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectSuccessfulCall(Tp::PendingOperation*))));
    QCOMPARE(mLoop->exec(), 0);

    PendingReady *same = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(same != NULL);
    QVERIFY(!same->proxy().isNull());

    // The first one is in scope and valid so we should've got it again
    QCOMPARE(same->proxy().data(), firstProxy.data());

    QVERIFY(connect(same, SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectSuccessfulCall(Tp::PendingOperation*))));
    QCOMPARE(mLoop->exec(), 0);

    // Flush the delete event for the PendingReady, which drops the PendingReady ref to the proxy
    mLoop->processEvents();

    // Synthesize an invalidation for the proxy
    QVERIFY(connect(firstProxy.data(), SIGNAL(invalidated(Tp::DBusProxy*,QString,QString)),
                mLoop, SLOT(quit())));
    TestBackdoors::invalidateProxy(firstProxy.data(),
            QLatin1String("im.bonghits.Errors.Synthetic"), QLatin1String(""));
    QCOMPARE(mLoop->exec(), 0);

    QVERIFY(!firstProxy->isValid());

    PendingReady *different = mFactory->proxy(mConnName1, mConnPath1,
            ChannelFactory::create(QDBusConnection::sessionBus()),
            ContactFactory::create());

    QVERIFY(different != NULL);
    ConnectionPtr differentProxy = ConnectionPtr::dynamicCast(different->proxy());
    QVERIFY(!differentProxy.isNull());

    // The first one is invalid so we should've got a different one
    QVERIFY(differentProxy.data() != firstProxy.data());
    QVERIFY(differentProxy->isValid());

    QVERIFY(!differentProxy->isReady());

    QVERIFY(connect(different,
                SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(expectSuccessfulCall(Tp::PendingOperation*))));
    QCOMPARE(mLoop->exec(), 0);

    QVERIFY(differentProxy->isValid());
    QVERIFY(differentProxy->isReady());
}

void TestDBusProxyFactory::cleanup()
{
    mFactory.reset();

    cleanupImpl();
}

void TestDBusProxyFactory::cleanupTestCase()
{
    cleanupTestCaseImpl();
}

QTEST_MAIN(TestDBusProxyFactory)
#include "_gen/dbus-proxy-factory.cpp.moc.hpp"

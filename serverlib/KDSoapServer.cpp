#include "KDSoapServer.h"
#include "KDSoapThreadPool.h"
#include "KDSoapSocketList_p.h"
#include <QMutex>
#include <QFile>
#ifdef Q_OS_UNIX
#include <sys/time.h>
#include <sys/resource.h>
#endif

class KDSoapServer::Private
{
public:
    Private()
        : m_threadPool(0),
          m_mainThreadSocketList(0),
          m_use(KDSoapMessage::LiteralUse),
          m_logLevel(KDSoapServer::LogNothing),
          m_portBeforeSuspend(0)
    {
    }

    ~Private()
    {
        delete m_mainThreadSocketList;
    }

    KDSoapThreadPool* m_threadPool;
    KDSoapSocketList* m_mainThreadSocketList;
    KDSoapMessage::Use m_use;

    QMutex m_logMutex;
    KDSoapServer::LogLevel m_logLevel;
    QString m_logFileName;
    QFile m_logFile;

    QHostAddress m_addressBeforeSuspend;
    quint16 m_portBeforeSuspend;
};

KDSoapServer::KDSoapServer(QObject* parent)
    : QTcpServer(parent),
      d(new KDSoapServer::Private)
{
    // Probably not very useful since we handle them immediately, but cannot hurt.
    setMaxPendingConnections(1000);
}

KDSoapServer::~KDSoapServer()
{
    delete d;
}

void KDSoapServer::incomingConnection(int socketDescriptor)
{
    if (d->m_threadPool) {
        //qDebug() << "incomingConnection: using thread pool";
        d->m_threadPool->handleIncomingConnection(socketDescriptor, this);
    } else {
        //qDebug() << "incomingConnection: using main-thread socketlist";
        if (!d->m_mainThreadSocketList)
            d->m_mainThreadSocketList = new KDSoapSocketList(this /*server*/);
        d->m_mainThreadSocketList->handleIncomingConnection(socketDescriptor);
    }
}

int KDSoapServer::numConnectedSockets() const
{
    if (d->m_threadPool) {
        return d->m_threadPool->numConnectedSockets(this);
    } else if (d->m_mainThreadSocketList) {
        return d->m_mainThreadSocketList->socketCount();
    } else {
        return 0;
    }
}

void KDSoapServer::setThreadPool(KDSoapThreadPool *threadPool)
{
    d->m_threadPool = threadPool;
}

KDSoapThreadPool * KDSoapServer::threadPool() const
{
    return d->m_threadPool;
}

QString KDSoapServer::endPoint() const {
    const QHostAddress address = serverAddress();
    const QString addressStr = address == QHostAddress::Any ? QString::fromLatin1("127.0.0.1") : address.toString();
    return QString::fromLatin1("%1://%2:%3/path")
            .arg(QString::fromLatin1(/*(m_features & Ssl)?"https":*/"http"))
            .arg(addressStr)
            .arg(serverPort());
}

void KDSoapServer::setUse(KDSoapMessage::Use use)
{
    d->m_use = use;
}

KDSoapMessage::Use KDSoapServer::use() const
{
    return d->m_use;
}

void KDSoapServer::setLogLevel(KDSoapServer::LogLevel level)
{
    QMutexLocker lock(&d->m_logMutex);
    d->m_logLevel = level;
}

KDSoapServer::LogLevel KDSoapServer::logLevel() const
{
    QMutexLocker lock(&d->m_logMutex);
    return d->m_logLevel;
}

void KDSoapServer::setLogFileName(const QString &fileName)
{
    QMutexLocker lock(&d->m_logMutex);
    d->m_logFileName = fileName;
}

QString KDSoapServer::logFileName() const
{
    QMutexLocker lock(&d->m_logMutex);
    return d->m_logFileName;
}

void KDSoapServer::log(const QByteArray &text)
{
    QMutexLocker lock(&d->m_logMutex);
    if (!d->m_logFile.isOpen() && !d->m_logFileName.isEmpty()) {
        d->m_logFile.setFileName(d->m_logFileName);
        if (!d->m_logFile.open(QIODevice::Append)) {
            qCritical("Could not open log file for writing: %s", qPrintable(d->m_logFileName));
            d->m_logFileName.clear(); // don't retry every time log() is called
            return;
        }
    }
    d->m_logFile.write(text);
}

void KDSoapServer::flushLogFile()
{
    d->m_logFile.flush();
}

bool KDSoapServer::setExpectedSocketCount(int sockets)
{
    // I hit a system limit when trying to connect more than 1024 sockets in the same process.
    // strace said: socket(PF_INET, SOCK_STREAM|SOCK_CLOEXEC, IPPROTO_IP) = -1 EMFILE (Too many open files)
    // Solution: ulimit -n 4096
    // Or in C code, below.

#ifdef Q_OS_UNIX
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
        qDebug() << "error calling getrlimit";
        return false;
    }
    bool changingHardLimit = false;
    if (sockets > -1) {
        qDebug() << "Current limit" << lim.rlim_cur << lim.rlim_max;
        sockets += 20; // we need some file descriptors too
        if (rlim_t(sockets) <= lim.rlim_cur)
            return true; // nothing to do

        if (rlim_t(sockets) > lim.rlim_max) {
            // Seems we need to run as root then
            lim.rlim_max = sockets;
            qDebug() << "Setting rlim_max to" << sockets;
            changingHardLimit = true;
        }
    }
    lim.rlim_cur = lim.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &lim) == 0) {
        qDebug() << "limit set to" << lim.rlim_cur;
    } else {
        if (changingHardLimit) {
            qDebug() << "WARNING: hard limit is not high enough";
        }
        qDebug() << "error calling setrlimit";
        return false;
    }
#endif
    return true;
}

#if 0
void KDSoapServer::suspend()
{
    d->m_portBeforeSuspend = serverPort();
    d->m_addressBeforeSuspend = serverAddress();
    close();
}

void KDSoapServer::resume()
{
    if (d->m_portBeforeSuspend == 0) {
        qWarning("KDSoapServer: resume() called without calling suspend() first");
    } else {
        if (!listen(d->m_addressBeforeSuspend, d->m_portBeforeSuspend)) {
            qWarning("KDSoapServer: failed to listen on %s port %d", qPrintable(d->m_addressBeforeSuspend.toString()), d->m_portBeforeSuspend);
        }
    }
}
#endif

#include "moc_KDSoapServer.cpp"
#include "KDSoapClientInterface.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDebug>
#include <QBuffer>
#include <QXmlStreamWriter>
#include <QDateTime>

class KDSoapClientInterface::Private
{
public:
    QNetworkAccessManager accessManager;
    QString host;
    QString path;
    QString messageNamespace;
};

KDSoapClientInterface::KDSoapClientInterface(const QString& host, const QString& path, const QString& messageNamespace)
    : d(new Private)
{
    d->host = host;
    d->path = path;
    d->messageNamespace = messageNamespace;
}

KDSoapClientInterface::~KDSoapClientInterface()
{
    delete d;
}

KDSoapMessage KDSoapClientInterface::call(const QString& method, const KDSoapMessage &message)
{
    // Problem is: I don't want a nested event loop here. Too dangerous for GUI programs.
    // I wanted a socket->waitFor... but we don't have access to the actual socket in QNetworkAccess.
    // So the only option that remains is a thread and acquiring a semaphore...
    (void)message; // TODO
    (void)method;
    return KDSoapMessage();
}

static QString variantToTextValue(const QVariant& value)
{
    switch (value.userType())
    {
    case QVariant::Char:
        // fall-through
    case QVariant::String:
        return value.toString();
    case QVariant::Url:
        // xmlpatterns/data/qatomicvalue.cpp says to do this:
        return value.toUrl().toString();
    case QVariant::ByteArray:
        return QString::fromLatin1(value.toByteArray().toBase64());
    case QVariant::Int:
        // fall-through
    case QVariant::LongLong:
        // fall-through
    case QVariant::UInt:
        return QString::number(value.toLongLong());
    case QVariant::ULongLong:
        return QString::number(value.toULongLong());
    case QVariant::Bool:
    case QMetaType::Float:
    case QVariant::Double:
        return value.toString();
    case QVariant::Time:
        return value.toDateTime().toString();
    case QVariant::Date:
        return QDateTime(value.toDate(), QTime(), Qt::UTC).toString();
    case QVariant::DateTime:
        return value.toDateTime().toString();
    default:
        if (value.userType() == qMetaTypeId<float>())
            return QString::number(value.value<float>());

        qDebug() << QString::fromLatin1("QVariants of type %1 are not supported in "
                                        "KDSoap, see the documentation").arg(QLatin1String(value.typeName()));
        return value.toString();
    }
}

static QString variantToXMLType(const QVariant& value)
{
    switch (value.userType())
    {
    case QVariant::Char:
        // fall-through
    case QVariant::String:
        // fall-through
    case QVariant::Url:
        return QLatin1String("xsd:string");
    case QVariant::ByteArray:
        return QLatin1String("xsd:base64Binary");
    case QVariant::Int:
        // fall-through
    case QVariant::LongLong:
        // fall-through
    case QVariant::UInt:
        return QLatin1String("xsd:int");
    case QVariant::ULongLong:
        return QLatin1String("xsd:unsignedInt");
    case QVariant::Bool:
        return QLatin1String("xsd:boolean");
    case QMetaType::Float:
        return QLatin1String("xsd:float");
    case QVariant::Double:
        return QLatin1String("xsd:double");
    case QVariant::Time:
        return QLatin1String("xsd:time"); // correct? xmlpatterns fallsback to datetime because of missing timezone
    case QVariant::Date:
        return QLatin1String("xsd:date");
    case QVariant::DateTime:
        return QLatin1String("xsd:dateTime");
    default:
        if (value.userType() == qMetaTypeId<float>())
            return QLatin1String("xsd:float");
        qDebug() << QString::fromLatin1("QVariants of type %1 are not supported in "
                                        "KDSoap, see the documentation").arg(QLatin1String(value.typeName()));
        return QString();
    }
}

KDSoapPendingCall KDSoapClientInterface::asyncCall(const QString &method, const KDSoapMessage &message, const QString& action)
{
    QUrl url;
    url.setScheme(QString::fromLatin1("http")); // TODO https support
    url.setHost(d->host);
    url.setPath(d->path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("text/xml"));
    // The soap action seems to be namespace + method in most cases, but not always
    // (e.g. urn:GoogleSearchAction for google).
    QString soapAction = action;
    if (soapAction.isEmpty()) {
        // Does the namespace always end with a '/'?
        soapAction = d->messageNamespace + /*QChar::fromLatin1('/') +*/ method;
    }
    qDebug() << "soapAction=" << soapAction;
    request.setRawHeader("SoapAction", soapAction.toUtf8());

    QByteArray data;
    QXmlStreamWriter writer(&data);
    writer.writeStartDocument();

    const QString soapNS = QString::fromLatin1("http://schemas.xmlsoap.org/soap/envelope/");
    const QString xmlSchemaNS = QString::fromLatin1("http://www.w3.org/1999/XMLSchema");
    const QString xmlSchemaInstanceNS = QString::fromLatin1("http://www.w3.org/1999/XMLSchema-instance");
    writer.writeNamespace(soapNS, QLatin1String("soap"));
    writer.writeNamespace(xmlSchemaNS, QLatin1String("xsd"));
    writer.writeNamespace(xmlSchemaInstanceNS, QLatin1String("xsi"));

    writer.writeStartElement(soapNS, QLatin1String("Envelope"));
    writer.writeAttribute(soapNS, QLatin1String("encodingStyle"), QLatin1String("http://schemas.xmlsoap.org/soap/encoding/"));

    writer.writeStartElement(soapNS, QLatin1String("Body"));
    writer.writeStartElement(d->messageNamespace, method);

    // Arguments
    const QMap<QString, QVariant> args = message.arguments();
    QMapIterator<QString, QVariant> it(args);
    while (it.hasNext()) {
        it.next();
        const QVariant value = it.value();
        const QString type = variantToXMLType(value);
        if (!type.isEmpty()) {
            writer.writeStartElement(d->messageNamespace, it.key());
            writer.writeAttribute(xmlSchemaInstanceNS, QLatin1String("type"), type);
            writer.writeCharacters(variantToTextValue(value));
            writer.writeEndElement();
        }
    }

    writer.writeEndElement(); // <method>
    writer.writeEndElement(); // Body
    writer.writeEndElement(); // Envelope
    writer.writeEndDocument();

    qDebug() << data;

    QBuffer* buffer = new QBuffer;
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);

    QNetworkReply* reply = d->accessManager.post(request, buffer);
    return KDSoapPendingCall(reply, buffer);
}
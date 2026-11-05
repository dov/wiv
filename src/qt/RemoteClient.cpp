#include "RemoteClient.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

#include <cstdio>

namespace givqt
{

void splitRemoteCommand(const QString& command, QString& method, QString& paramsJson)
{
    const QStringList words = command.split(' ', Qt::SkipEmptyParts);
    method = words.value(0);
    QJsonArray params;
    for (int i = 1; i < words.size(); ++i) params.append(words[i]);
    paramsJson = QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact));
}

int runRemoteClient(const QString& host, int port, const QString& method, const QString& paramsJson)
{
    QJsonValue params; // null unless given
    if (!paramsJson.isEmpty())
    {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || doc.isNull())
        {
            std::fprintf(stderr, "Invalid --json_params: %s\n", qPrintable(parseError.errorString()));
            return -1;
        }
        params = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
    }

    QJsonObject request;
    request["jsonrpc"] = QStringLiteral("2.0");
    request["method"] = method;
    request["params"] = params.isNull() ? QJsonValue(QJsonArray()) : params;
    request["id"] = 1;
    const QByteArray body = QJsonDocument(request).toJson(QJsonDocument::Compact);

    QTcpSocket socket;
    socket.connectToHost(host, static_cast<quint16>(port));
    if (!socket.waitForConnected(3000))
    {
        std::fprintf(stderr, "No giv remote server found at port %s:%d!\n", qPrintable(host), port);
        return -1;
    }

    QByteArray http = "POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    socket.write(http);
    socket.flush();

    // The server answers with one HTTP-wrapped response and closes. A
    // pick_coordinate call only answers on the next click, so no timeout
    // once the request is out.
    QByteArray response;
    while (socket.state() != QAbstractSocket::UnconnectedState || socket.bytesAvailable())
    {
        socket.waitForReadyRead(-1);
        response += socket.readAll();
    }

    const int headerEnd = response.indexOf("\r\n\r\n");
    const QByteArray payload = headerEnd >= 0 ? response.mid(headerEnd + 4) : response;
    std::printf("%s\n", payload.constData());
    return 0;
}

} // namespace givqt

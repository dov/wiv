#include "RpcServer.h"

#include "MainWindow.h"
#include "VulkanViewport.h"

#include <spdlog/spdlog.h>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

namespace givqt
{

RpcServer::RpcServer(MainWindow* mainWindow, VulkanViewport* viewport, QObject* parent)
    : QObject(parent), mainWindow_(mainWindow), viewport_(viewport)
{
    connect(&server_, &QTcpServer::newConnection, this, &RpcServer::onNewConnection);
    connect(viewport_, &VulkanViewport::clicked, this, &RpcServer::onViewportClicked);
}

bool RpcServer::start(quint16 port)
{
    if (!server_.listen(QHostAddress::LocalHost, port))
    {
        spdlog::error("RpcServer: failed to listen on 127.0.0.1:{}: {}", port,
                       server_.errorString().toStdString());
        return false;
    }
    spdlog::info("RpcServer: listening on 127.0.0.1:{}", port);
    return true;
}

void RpcServer::onNewConnection()
{
    while (server_.hasPendingConnections())
    {
        QTcpSocket* socket = server_.nextPendingConnection();
        buffers_.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() { onDisconnected(socket); });
    }
}

void RpcServer::onReadyRead(QTcpSocket* socket)
{
    auto it = buffers_.find(socket);
    if (it == buffers_.end()) return;

    it.value().append(socket->readAll());
    if (tryHandleRequest(socket, it.value())) buffers_.remove(socket);
}

void RpcServer::onDisconnected(QTcpSocket* socket)
{
    buffers_.remove(socket);
    if (pendingPick_ == socket) pendingPick_ = nullptr;
    socket->deleteLater();
}

bool RpcServer::tryHandleRequest(QTcpSocket* socket, const QByteArray& buffer)
{
    static const QByteArray kHeaderEnd = "\r\n\r\n";
    int headerEnd = buffer.indexOf(kHeaderEnd);
    if (headerEnd < 0) return false; // still waiting on the rest of the headers

    QByteArray headers = buffer.left(headerEnd);
    qint64 contentLength = 0;
    for (const QByteArray& line : headers.split('\n'))
    {
        QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("content-length:"))
        {
            contentLength = trimmed.mid(trimmed.indexOf(':') + 1).trimmed().toLongLong();
            break;
        }
    }

    int bodyStart = headerEnd + kHeaderEnd.size();
    if (buffer.size() - bodyStart < contentLength) return false; // body not fully arrived yet

    QByteArray body = buffer.mid(bodyStart, contentLength);
    QJsonDocument requestDoc = QJsonDocument::fromJson(body);
    QJsonObject request = requestDoc.object();
    QString method = request.value("method").toString();
    QJsonArray params = request.value("params").toArray();
    QJsonValue id = request.value("id");

    QString error;
    bool pickHandled = false;
    QJsonValue result = dispatch(method, params, socket, error, pickHandled);
    if (pickHandled) return true; // pick_coordinate: response deferred to onViewportClicked()

    QJsonObject response;
    response["jsonrpc"] = QStringLiteral("2.0");
    response["id"] = id;
    if (error.isEmpty())
        response["result"] = result;
    else
    {
        QJsonObject errorObj;
        errorObj["code"] = method.isEmpty() ? -32600 : -32000;
        errorObj["message"] = error;
        response["error"] = errorObj;
    }

    writeHttpJson(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->disconnectFromHost();
    return true;
}

QJsonValue RpcServer::dispatch(const QString& method, const QJsonArray& params, QTcpSocket* socket, QString& error,
                                bool& pickHandled)
{
    if (method == "ping") return QStringLiteral("pong");

    if (method == "help")
        return QStringLiteral(
            "ping | help | load_file [filename] | giv_string [\"-append\",] text | "
            "get_transformation | set_transformation [sx, sy, shx, shy] | pick_coordinate");

    if (method == "load_file")
    {
        if (params.isEmpty() || !params[0].isString())
        {
            error = "load_file requires a filename string parameter";
            return {};
        }
        mainWindow_->loadFiles({params[0].toString().toStdString()});
        return true;
    }

    if (method == "giv_string")
    {
        if (params.isEmpty())
        {
            error = "giv_string requires a text parameter";
            return {};
        }
        bool append = false;
        QString text;
        if (params.size() >= 2 && params[0].isString() && params[0].toString() == QStringLiteral("-append"))
        {
            append = true;
            text = params[1].toString();
        }
        else
        {
            text = params[0].toString();
        }
        QString buildError;
        if (!viewport_->applyGivString(text.toStdString(), append, &buildError))
        {
            error = buildError.isEmpty() ? QStringLiteral("giv_string: failed to rebuild scene") : buildError;
            return {};
        }
        return true;
    }

    if (method == "get_transformation")
    {
        double scaleX = 0, scaleY = 0, shiftX = 0, shiftY = 0;
        viewport_->getTransformation(scaleX, scaleY, shiftX, shiftY);
        return QJsonArray{scaleX, scaleY, shiftX, shiftY};
    }

    if (method == "set_transformation")
    {
        if (params.size() < 4)
        {
            error = "set_transformation requires [scaleX, scaleY, shiftX, shiftY]";
            return {};
        }
        viewport_->setTransformation(params[0].toDouble(), params[1].toDouble(), params[2].toDouble(),
                                      params[3].toDouble());
        return true;
    }

    if (method == "pick_coordinate")
    {
        if (pendingPick_ != nullptr)
        {
            error = "Busy!";
            return {};
        }
        pendingPick_ = socket;
        pickHandled = true;
        return {};
    }

    error = "unknown method: " + method;
    return {};
}

void RpcServer::onViewportClicked(double x, double y, int button, int modifiers)
{
    if (!pendingPick_) return;
    QTcpSocket* socket = pendingPick_;
    pendingPick_ = nullptr;

    // The request's "id" was already consumed inside tryHandleRequest()'s
    // scope; giv's own json-rpc responses to pick_coordinate don't echo a
    // request id back either (it's a fire-and-wait call), so this reuses a
    // fixed id rather than threading the original one through the deferred
    // path.
    QJsonObject response;
    response["jsonrpc"] = QStringLiteral("2.0");
    response["id"] = QJsonValue();
    response["result"] = QJsonArray{x, y, button, modifiers};

    writeHttpJson(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->disconnectFromHost();
    buffers_.remove(socket);
}

void RpcServer::writeHttpJson(QTcpSocket* socket, const QByteArray& jsonBody)
{
    QByteArray httpResponse = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(jsonBody.size()) + "\r\nConnection: close\r\n\r\n" + jsonBody;
    socket->write(httpResponse);
    socket->flush();
}

} // namespace givqt

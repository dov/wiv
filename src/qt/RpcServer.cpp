// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "RpcServer.h"

#include "MainWindow.h"
#include "VulkanViewport.h"

#include "../Colormaps.h"

#include <spdlog/spdlog.h>

#include <optional>

#include <QCursor>
#include <QEventLoop>
#include <QHostAddress>
#include <QTimer>
#include <QImage>
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
            "get_transformation | set_transformation [sx, sy, shx, shy] | pick_coordinate | "
            "list_actions | trigger_action id [checked] | resize_window [w, h] | "
            "set_colormap [name] | set_contrast [min, max] | mouse_move [x, y] (default: viewport centre) | "
            "export_image [filename, width, height] | screenshot filename [\"window\"|\"viewport\"]");

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

    if (method == "export_image")
    {
        if (params.isEmpty() || !params[0].isString())
        {
            error = "export_image requires a filename string parameter";
            return {};
        }
        // The current view (scale and center) at the given size, by default
        // the size of the viewport.
        QSize size = viewport_->visibleSizePx();
        if (params.size() >= 3) size = QSize(params[1].toInt(), params[2].toInt());
        VulkanViewport::ExportView view;
        view.kind = VulkanViewport::ExportView::Kind::Current;
        QString exportError;
        if (!viewport_->exportImage(params[0].toString(), size.width(), size.height(), view, &exportError))
        {
            error = exportError;
            return {};
        }
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

    if (method == "list_actions")
    {
        QJsonArray ids;
        for (const QString& id : mainWindow_->actionIds()) ids.append(id);
        return ids;
    }

    if (method == "trigger_action")
    {
        if (params.isEmpty() || !params[0].isString())
        {
            error = "trigger_action requires an action id string (see list_actions)";
            return {};
        }
        std::optional<bool> checked;
        if (params.size() >= 2 && params[1].isBool()) checked = params[1].toBool();
        if (!mainWindow_->triggerAction(params[0].toString(), checked, error)) return {};
        return true;
    }

    if (method == "resize_window")
    {
        if (params.size() < 2)
        {
            error = "resize_window requires [width, height]";
            return {};
        }
        mainWindow_->resize(params[0].toInt(), params[1].toInt());
        return true;
    }

    if (method == "set_colormap")
    {
        QString want = params.isEmpty() ? QString() : params[0].toString();
        for (int i = 0; i < static_cast<int>(giv::colormaps::Id::Count); ++i)
        {
            auto id = static_cast<giv::colormaps::Id>(i);
            if (want.compare(QString::fromUtf8(giv::colormaps::name(id)), Qt::CaseInsensitive) == 0 ||
                (id == giv::colormaps::Id::None && want.isEmpty()))
            {
                viewport_->setColormap(id, id != giv::colormaps::Id::None);
                return true;
            }
        }
        error = "unknown colormap: " + want;
        return {};
    }

    if (method == "set_contrast")
    {
        if (params.size() < 2)
        {
            error = "set_contrast requires [min, max]";
            return {};
        }
        viewport_->setContrast(static_cast<float>(params[0].toDouble()), static_cast<float>(params[1].toDouble()));
        return true;
    }

    if (method == "mouse_move")
    {
        // Moves the real pointer to (x, y), in the viewport's own coordinates
        // (logical pixels from its top-left), so hover features (balloons) can be
        // driven. Two small steps so the viewport sees motion even if the pointer
        // was already there.
        // No parameters: the centre of the viewport.
        const QPoint target = params.size() >= 2 ? QPoint(params[0].toInt(), params[1].toInt())
                                                 : QPoint(viewport_->width() / 2, viewport_->height() / 2);
        QCursor::setPos(viewport_->mapToGlobal(target + QPoint(-6, -4)));
        QEventLoop loop1;
        QTimer::singleShot(150, &loop1, &QEventLoop::quit);
        loop1.exec();
        QCursor::setPos(viewport_->mapToGlobal(target));
        return true;
    }

    if (method == "screenshot")
    {
        if (params.isEmpty() || !params[0].isString())
        {
            error = "screenshot requires a filename string";
            return {};
        }
        const bool viewportOnly = params.size() >= 2 && params[1].toString() == QStringLiteral("viewport");
        QImage img = mainWindow_->grabScreenshot(viewportOnly);
        if (img.isNull() || !img.save(params[0].toString()))
        {
            error = "screenshot: could not grab or save " + params[0].toString();
            return {};
        }
        return QJsonArray{img.width(), img.height()};
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

void RpcServer::abortPick()
{
    if (!pendingPick_) return;
    QTcpSocket* socket = pendingPick_;
    pendingPick_ = nullptr;

    QJsonObject errorObj;
    errorObj["code"] = -32000;
    errorObj["message"] = QStringLiteral("Aborted");
    QJsonObject response;
    response["jsonrpc"] = QStringLiteral("2.0");
    response["id"] = QJsonValue();
    response["error"] = errorObj;

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

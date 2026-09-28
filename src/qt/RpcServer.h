#pragma once
//
// RpcServer.h - json-rpc remote-control server, wiv's counterpart to giv's
// glib-jsonrpc (src/glib-jsonrpc/ in the legacy tree): lets an external
// script push live commands (load a file, inject marks, read/set the
// pan/zoom transform, wait for the next click) into a running wiv instance
// over a socket, the same workflow giv's python/giv-client*.py and
// sync-two-givs.py scripts use.
//
// Wire format matches giv exactly (so those existing scripts work against
// wiv unmodified): a bare-bones HTTP POST wrapping a JSON-RPC 2.0 request
// body, replied to with a bare-bones "HTTP/1.1 200 OK" wrapping the JSON-RPC
// response, then the connection is closed - one request per connection, no
// keep-alive, no real HTTP semantics (headers besides Content-Length are
// ignored). Bound to 127.0.0.1 only, matching giv's default.
//
// Since wiv (unlike giv/GTK) is single-threaded, the whole server runs
// inline on the Qt GUI thread via QTcpServer's normal async signals - no
// worker thread, no idle-add/cond-wait marshaling of the kind giv's
// GThreadedSocketService needed.
//
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QTcpServer>

#include <memory>

class QTcpSocket;

namespace givqt
{

class MainWindow;
class VulkanViewport;

class RpcServer : public QObject
{
    Q_OBJECT

public:
    RpcServer(MainWindow* mainWindow, VulkanViewport* viewport, QObject* parent = nullptr);

    // Starts listening on 127.0.0.1:port. Returns false (and logs via
    // spdlog) on failure, e.g. the port is already in use.
    bool start(quint16 port);

private:
    MainWindow* mainWindow_ = nullptr;
    VulkanViewport* viewport_ = nullptr;
    QTcpServer server_;

    // Per-connection accumulated bytes, until a full HTTP header + body has
    // arrived - see onReadyRead().
    QHash<QTcpSocket*, QByteArray> buffers_;

    // The socket waiting on the next viewport click, if a pick_coordinate
    // call is in flight - only one at a time (a second concurrent call gets
    // an immediate "Busy!" error, matching giv's async_busy behavior).
    QTcpSocket* pendingPick_ = nullptr;

    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    void onDisconnected(QTcpSocket* socket);

    // Parses one complete HTTP-wrapped JSON-RPC request out of `buffer`,
    // dispatches it, and writes the HTTP-wrapped response to `socket`.
    // Returns false if `buffer` doesn't yet contain a complete request (the
    // caller should wait for more readyRead() data).
    bool tryHandleRequest(QTcpSocket* socket, const QByteArray& buffer);

    // method dispatch - each returns the JSON-RPC "result" value, or sets
    // *error and returns an unused value on failure. `pickHandled` is set
    // true (and no response written yet) for a pick_coordinate call that
    // will reply asynchronously later, from onViewportClicked().
    QJsonValue dispatch(const QString& method, const QJsonArray& params, QTcpSocket* socket, QString& error,
                         bool& pickHandled);

    void onViewportClicked(double x, double y, int button, int modifiers);

    void writeHttpJson(QTcpSocket* socket, const QByteArray& jsonBody);
};

} // namespace givqt

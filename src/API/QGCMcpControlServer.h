#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtHttpServer/QHttpServer>
#include <QtHttpServer/QHttpServerResponse>
#include <QtNetwork/QTcpServer>
#include <optional>

#include "PlanMasterController.h"

class QHttpServerRequest;
class Vehicle;

/// Local authenticated control bridge used by the companion MCP stdio server.
///
/// The bridge only binds to loopback and must be explicitly enabled at startup.
class QGCMcpControlServer : public QObject
{
    Q_OBJECT

public:
    explicit QGCMcpControlServer(QObject* parent = nullptr);

    [[nodiscard]] bool start(quint16 port, const QByteArray& token);

private:
    struct ActionResult
    {
        bool ok = false;
        QJsonObject value;
        QString error;
    };

    QHttpServerResponse _handleStatus(const QHttpServerRequest& request);
    QHttpServerResponse _handleCall(const QHttpServerRequest& request);

    [[nodiscard]] bool _isAuthorized(const QHttpServerRequest& request) const;
    [[nodiscard]] ActionResult _dispatch(const QString& name, const QJsonObject& arguments);
    [[nodiscard]] ActionResult _status(const QJsonObject& arguments);
    [[nodiscard]] ActionResult _vehicleAction(const QString& name, const QJsonObject& arguments);
    [[nodiscard]] ActionResult _planAction(const QString& name, const QJsonObject& arguments);

    [[nodiscard]] Vehicle* _vehicle(const QJsonObject& arguments, QString& error) const;
    [[nodiscard]] static std::optional<QGeoCoordinate> _coordinate(const QJsonObject& arguments, bool altitudeRequired,
                                                                   QString& error);
    [[nodiscard]] static bool _confirmed(const QJsonObject& arguments, QString& error);
    [[nodiscard]] static ActionResult _success(const QJsonObject& value = {});
    [[nodiscard]] static ActionResult _failure(const QString& error);
    [[nodiscard]] static QJsonObject _vehicleJson(Vehicle* vehicle);
    [[nodiscard]] static QHttpServerResponse _jsonResponse(
        const QJsonObject& json, QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::Ok);

    QHttpServer _httpServer;
    QTcpServer _tcpServer;
    QByteArray _token;
    PlanMasterController _planController;
};

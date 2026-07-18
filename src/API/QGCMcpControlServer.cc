#include "QGCMcpControlServer.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponse>
#include <QtNetwork/QHostAddress>
#include <cmath>

#include "Fact.h"
#include "MissionController.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

QGC_LOGGING_CATEGORY(QGCMcpControlServerLog, "API.QGCMcpControlServer")

namespace {

constexpr qsizetype MAX_REQUEST_BYTES = 1024 * 1024;
constexpr qsizetype MAX_PLAN_FILE_BYTES = 10 * 1024 * 1024;

bool secureEquals(const QByteArray& first, const QByteArray& second)
{
    if (first.size() != second.size()) {
        return false;
    }

    unsigned char difference = 0;
    for (qsizetype index = 0; index < first.size(); ++index) {
        difference |= static_cast<unsigned char>(first.at(index)) ^ static_cast<unsigned char>(second.at(index));
    }
    return difference == 0;
}

}  // namespace

QGCMcpControlServer::QGCMcpControlServer(QObject* parent)
    : QObject(parent), _httpServer(this), _tcpServer(this), _planController(this)
{
    _planController.setFlyView(false);
    _planController.start();
}

bool QGCMcpControlServer::start(quint16 port, const QByteArray& token)
{
    if (token.size() < 32) {
        qCWarning(QGCMcpControlServerLog) << "QGC_MCP_TOKEN must contain at least 32 characters";
        return false;
    }

    _token = token;

    (void) _httpServer.route(QStringLiteral("/v1/status"), QHttpServerRequest::Method::Get, this,
                             [this](const QHttpServerRequest& request) { return _handleStatus(request); });
    (void) _httpServer.route(QStringLiteral("/v1/tools/call"), QHttpServerRequest::Method::Post, this,
                             [this](const QHttpServerRequest& request) { return _handleCall(request); });

    if (!_tcpServer.listen(QHostAddress::LocalHost, port)) {
        qCWarning(QGCMcpControlServerLog) << "Could not listen on loopback port" << port << _tcpServer.errorString();
        return false;
    }
    if (!_httpServer.bind(&_tcpServer)) {
        qCWarning(QGCMcpControlServerLog) << "Could not bind HTTP server to loopback listener";
        _tcpServer.close();
        return false;
    }

    qCDebug(QGCMcpControlServerLog) << "MCP control bridge listening on 127.0.0.1:" << _tcpServer.serverPort();
    return true;
}

QHttpServerResponse QGCMcpControlServer::_handleStatus(const QHttpServerRequest& request)
{
    if (!_isAuthorized(request)) {
        return _jsonResponse(
            QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unauthorized")}},
            QHttpServerResponse::StatusCode::Unauthorized);
    }

    const ActionResult result = _status({});
    return _jsonResponse(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("result"), result.value}});
}

QHttpServerResponse QGCMcpControlServer::_handleCall(const QHttpServerRequest& request)
{
    if (!_isAuthorized(request)) {
        return _jsonResponse(
            QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Unauthorized")}},
            QHttpServerResponse::StatusCode::Unauthorized);
    }
    if (request.body().size() > MAX_REQUEST_BYTES) {
        return _jsonResponse(
            QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Request too large")}},
            QHttpServerResponse::StatusCode::PayloadTooLarge);
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &parseError);
    if ((parseError.error != QJsonParseError::NoError) || !document.isObject()) {
        return _jsonResponse(QJsonObject{{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("Invalid JSON object")}},
                             QHttpServerResponse::StatusCode::BadRequest);
    }

    const QJsonObject body = document.object();
    const QString name = body.value(QStringLiteral("name")).toString();
    const QJsonValue argumentsValue = body.value(QStringLiteral("arguments"));
    if (name.isEmpty() || (!argumentsValue.isUndefined() && !argumentsValue.isObject())) {
        return _jsonResponse(
            QJsonObject{{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("name and object arguments are required")}},
            QHttpServerResponse::StatusCode::BadRequest);
    }

    const ActionResult result = _dispatch(name, argumentsValue.toObject());
    if (result.ok) {
        return _jsonResponse(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("result"), result.value}});
    }
    return _jsonResponse(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), result.error}});
}

bool QGCMcpControlServer::_isAuthorized(const QHttpServerRequest& request) const
{
    const QByteArray expected = QByteArrayLiteral("Bearer ") + _token;
    return secureEquals(request.value(QByteArrayLiteral("Authorization")), expected);
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_dispatch(const QString& name, const QJsonObject& arguments)
{
    if ((name == QStringLiteral("get_status")) || (name == QStringLiteral("select_vehicle")) ||
        name.startsWith(QLatin1String("vehicle_"))) {
        if (name == QStringLiteral("get_status")) {
            return _status(arguments);
        }
        return _vehicleAction(name, arguments);
    }
    if (name.startsWith(QLatin1String("plan_"))) {
        return _planAction(name, arguments);
    }
    return _failure(QStringLiteral("Unknown tool: %1").arg(name));
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_status(const QJsonObject& arguments)
{
    QJsonObject result;
    QJsonArray vehicles;
    const QmlObjectListModel* vehicleModel = MultiVehicleManager::instance()->vehicles();
    for (int index = 0; index < vehicleModel->count(); ++index) {
        Vehicle* vehicle = vehicleModel->value<Vehicle*>(index);
        if (vehicle) {
            vehicles.append(_vehicleJson(vehicle));
        }
    }

    QString error;
    Vehicle* activeVehicle = _vehicle(arguments, error);
    result.insert(QStringLiteral("active_vehicle"),
                  activeVehicle ? QJsonValue(_vehicleJson(activeVehicle)) : QJsonValue());
    result.insert(QStringLiteral("vehicles"), vehicles);
    result.insert(
        QStringLiteral("plan"),
        QJsonObject{
            {QStringLiteral("offline"), _planController.offline()},
            {QStringLiteral("sync_in_progress"), _planController.syncInProgress()},
            {QStringLiteral("dirty_for_upload"), _planController.dirtyForUpload()},
            {QStringLiteral("mission_visual_items"), _planController.missionController()->visualItems()->count()},
            {QStringLiteral("rally_points"), _planController.rallyPointController()->points()->count()}});
    return _success(result);
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_vehicleAction(const QString& name, const QJsonObject& arguments)
{
    if (name == QStringLiteral("select_vehicle")) {
        const QJsonValue vehicleIdValue = arguments.value(QStringLiteral("vehicle_id"));
        const double numericVehicleId = vehicleIdValue.toDouble(-1.0);
        if (!vehicleIdValue.isDouble() || (std::floor(numericVehicleId) != numericVehicleId)) {
            return _failure(QStringLiteral("vehicle_id must be an integer"));
        }
        const int vehicleId = static_cast<int>(numericVehicleId);
        Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(vehicleId);
        if (!vehicle) {
            return _failure(QStringLiteral("Connected vehicle %1 was not found").arg(vehicleId));
        }
        const bool activationPending = MultiVehicleManager::instance()->activeVehicle() != vehicle;
        MultiVehicleManager::instance()->setActiveVehicle(vehicle);
        return _success(QJsonObject{{QStringLiteral("accepted"), true},
                                    {QStringLiteral("vehicle_id"), vehicleId},
                                    {QStringLiteral("activation_pending"), activationPending}});
    }

    QString error;
    Vehicle* vehicle = _vehicle(arguments, error);
    if (!vehicle) {
        return _failure(error);
    }
    if (!_confirmed(arguments, error)) {
        return _failure(error);
    }

    if (name == QStringLiteral("vehicle_set_armed")) {
        if (!arguments.value(QStringLiteral("armed")).isBool()) {
            return _failure(QStringLiteral("armed must be a boolean"));
        }
        const bool arm = arguments.value(QStringLiteral("armed")).toBool();
        if (!arm && vehicle->flying()) {
            return _failure(QStringLiteral("In-flight disarming is not exposed by the MCP bridge"));
        }
        vehicle->setArmedShowError(arm);
    } else if (name == QStringLiteral("vehicle_takeoff")) {
        const double altitude = arguments.value(QStringLiteral("altitude_m")).toDouble(-1.0);
        const double minimumAltitude = vehicle->minimumTakeoffAltitudeMeters();
        if (!std::isfinite(altitude) || (altitude < minimumAltitude) || (altitude > 500.0)) {
            return _failure(QStringLiteral("altitude_m must be between %1 and 500 meters").arg(minimumAltitude));
        }
        if (vehicle->flying()) {
            return _failure(QStringLiteral("Vehicle is already flying"));
        }
        vehicle->guidedModeTakeoff(altitude);
    } else if (name == QStringLiteral("vehicle_land")) {
        vehicle->guidedModeLand();
    } else if (name == QStringLiteral("vehicle_rtl")) {
        vehicle->guidedModeRTL(arguments.value(QStringLiteral("smart_rtl")).toBool(false));
    } else if (name == QStringLiteral("vehicle_pause")) {
        vehicle->pauseVehicle();
    } else if (name == QStringLiteral("vehicle_start_mission")) {
        vehicle->startMission();
    } else if (name == QStringLiteral("vehicle_goto")) {
        const std::optional<QGeoCoordinate> coordinate = _coordinate(arguments, false, error);
        if (!coordinate) {
            return _failure(error);
        }
        if (!vehicle->guidedModeGotoLocation(*coordinate)) {
            return _failure(QStringLiteral("Go-to command was rejected by QGroundControl safety checks"));
        }
    } else if (name == QStringLiteral("vehicle_set_roi")) {
        const std::optional<QGeoCoordinate> coordinate = _coordinate(arguments, false, error);
        if (!coordinate) {
            return _failure(error);
        }
        vehicle->guidedModeROI(*coordinate);
    } else if (name == QStringLiteral("vehicle_clear_roi")) {
        vehicle->stopGuidedModeROI();
    } else {
        return _failure(QStringLiteral("Unknown vehicle tool: %1").arg(name));
    }

    return _success(
        QJsonObject{{QStringLiteral("accepted"), true}, {QStringLiteral("vehicle"), _vehicleJson(vehicle)}});
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_planAction(const QString& name, const QJsonObject& arguments)
{
    if (_planController.syncInProgress()) {
        return _failure(QStringLiteral("A plan upload or download is already in progress"));
    }

    QString error;
    if (name == QStringLiteral("plan_get")) {
        return _success(QJsonObject{{QStringLiteral("plan"), _planController.saveToJson().object()},
                                    {QStringLiteral("dirty_for_upload"), _planController.dirtyForUpload()}});
    }
    if (name == QStringLiteral("plan_clear")) {
        _planController.removeAll();
    } else if (name == QStringLiteral("plan_load")) {
        const QString path = arguments.value(QStringLiteral("path")).toString();
        const QFileInfo fileInfo(path);
        if (!fileInfo.isFile() || !fileInfo.isReadable()) {
            return _failure(QStringLiteral("Plan file is not readable: %1").arg(path));
        }
        if (fileInfo.size() > MAX_PLAN_FILE_BYTES) {
            return _failure(QStringLiteral("Plan file exceeds the 10 MiB safety limit"));
        }
        _planController.loadFromFile(fileInfo.absoluteFilePath());
        if (_planController.currentPlanFile().isEmpty()) {
            return _failure(QStringLiteral("QGroundControl could not load the plan file"));
        }
    } else if (name == QStringLiteral("plan_save")) {
        const QString path = arguments.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            return _failure(QStringLiteral("path is required"));
        }
        if (!_planController.saveToFile(path)) {
            return _failure(QStringLiteral("QGroundControl could not save the plan file"));
        }
    } else if ((name == QStringLiteral("plan_add_waypoint")) || (name == QStringLiteral("plan_add_takeoff")) ||
               (name == QStringLiteral("plan_add_roi"))) {
        const std::optional<QGeoCoordinate> coordinate = _coordinate(arguments, true, error);
        if (!coordinate) {
            return _failure(error);
        }

        VisualMissionItem* visualItem = nullptr;
        MissionController* missionController = _planController.missionController();
        if (name == QStringLiteral("plan_add_waypoint")) {
            visualItem = missionController->insertSimpleMissionItem(*coordinate, -1);
        } else if (name == QStringLiteral("plan_add_takeoff")) {
            visualItem = missionController->insertTakeoffItem(*coordinate, -1);
        } else {
            visualItem = missionController->insertROIMissionItem(*coordinate, -1);
        }
        if (!visualItem) {
            return _failure(QStringLiteral("QGroundControl could not create the mission item"));
        }
        if (SimpleMissionItem* simpleItem = qobject_cast<SimpleMissionItem*>(visualItem)) {
            if (simpleItem->specifiesAltitude()) {
                simpleItem->altitude()->setRawValue(coordinate->altitude());
            }
        }
    } else if (name == QStringLiteral("plan_add_rally_point")) {
        const std::optional<QGeoCoordinate> coordinate = _coordinate(arguments, true, error);
        if (!coordinate) {
            return _failure(error);
        }
        RallyPointController* rallyController = _planController.rallyPointController();
        rallyController->addPoint(*coordinate);
        RallyPoint* rallyPoint = qobject_cast<RallyPoint*>(rallyController->currentRallyPoint());
        if (!rallyPoint) {
            return _failure(QStringLiteral("QGroundControl could not create the rally point"));
        }
        rallyPoint->setCoordinate(*coordinate);
    } else if (name == QStringLiteral("plan_upload")) {
        if (!_confirmed(arguments, error)) {
            return _failure(error);
        }
        if (_planController.readyForSaveState() != VisualMissionItem::ReadyForSave) {
            return _failure(QStringLiteral("Plan has incomplete items or is waiting for terrain data"));
        }
        const MissionController::SendToVehiclePreCheckState preCheck =
            _planController.missionController()->sendToVehiclePreCheck();
        if (preCheck == MissionController::SendToVehiclePreCheckStateNoActiveVehicle) {
            return _failure(QStringLiteral("No active vehicle is connected"));
        }
        if (preCheck == MissionController::SendToVehiclePreCheckStateActiveMission) {
            return _failure(QStringLiteral("Pause the active mission before uploading a replacement plan"));
        }
        if ((preCheck == MissionController::SendToVehiclePreCheckStateFirwmareVehicleMismatch) &&
            !arguments.value(QStringLiteral("allow_firmware_mismatch")).toBool(false)) {
            return _failure(QStringLiteral("Plan firmware or vehicle type does not match the active vehicle"));
        }
        _planController.sendToVehicle();
        return _success(QJsonObject{{QStringLiteral("accepted"), true},
                                    {QStringLiteral("sync_in_progress"), _planController.syncInProgress()}});
    } else {
        return _failure(QStringLiteral("Unknown plan tool: %1").arg(name));
    }

    return _success(QJsonObject{{QStringLiteral("plan"), _planController.saveToJson().object()},
                                {QStringLiteral("dirty_for_upload"), _planController.dirtyForUpload()}});
}

Vehicle* QGCMcpControlServer::_vehicle(const QJsonObject& arguments, QString& error) const
{
    MultiVehicleManager* manager = MultiVehicleManager::instance();
    Vehicle* vehicle = nullptr;
    if (arguments.contains(QStringLiteral("vehicle_id"))) {
        const QJsonValue vehicleIdValue = arguments.value(QStringLiteral("vehicle_id"));
        const double numericVehicleId = vehicleIdValue.toDouble(-1.0);
        if (!vehicleIdValue.isDouble() || (std::floor(numericVehicleId) != numericVehicleId)) {
            error = QStringLiteral("vehicle_id must be an integer");
            return nullptr;
        }
        vehicle = manager->getVehicleById(static_cast<int>(numericVehicleId));
    } else {
        vehicle = manager->activeVehicle();
    }
    if (!vehicle) {
        error = QStringLiteral("No matching active vehicle is connected");
    }
    return vehicle;
}

std::optional<QGeoCoordinate> QGCMcpControlServer::_coordinate(const QJsonObject& arguments, bool altitudeRequired,
                                                               QString& error)
{
    const QJsonValue latitudeValue = arguments.value(QStringLiteral("latitude"));
    const QJsonValue longitudeValue = arguments.value(QStringLiteral("longitude"));
    const QJsonValue altitudeValue = arguments.value(QStringLiteral("altitude_m"));
    if (!latitudeValue.isDouble() || !longitudeValue.isDouble() || (altitudeRequired && !altitudeValue.isDouble())) {
        error = altitudeRequired ? QStringLiteral("latitude, longitude, and altitude_m are required numbers")
                                 : QStringLiteral("latitude and longitude are required numbers");
        return std::nullopt;
    }

    const double latitude = latitudeValue.toDouble();
    const double longitude = longitudeValue.toDouble();
    const double altitude = altitudeValue.isDouble() ? altitudeValue.toDouble() : 0.0;
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(altitude) || (latitude < -90.0) ||
        (latitude > 90.0) || (longitude < -180.0) || (longitude > 180.0) || (altitude < -1000.0) ||
        (altitude > 100000.0)) {
        error = QStringLiteral("Coordinate is outside valid latitude, longitude, or altitude bounds");
        return std::nullopt;
    }

    const QGeoCoordinate coordinate(latitude, longitude, altitude);
    if (!coordinate.isValid()) {
        error = QStringLiteral("Coordinate is invalid");
        return std::nullopt;
    }
    return coordinate;
}

bool QGCMcpControlServer::_confirmed(const QJsonObject& arguments, QString& error)
{
    if (!arguments.value(QStringLiteral("confirm")).toBool(false)) {
        error = QStringLiteral("This action affects a vehicle or uploads a plan; set confirm to true");
        return false;
    }
    return true;
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_success(const QJsonObject& value)
{
    return ActionResult{.ok = true, .value = value, .error = {}};
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_failure(const QString& error)
{
    return ActionResult{.ok = false, .value = {}, .error = error};
}

QJsonObject QGCMcpControlServer::_vehicleJson(Vehicle* vehicle)
{
    if (!vehicle) {
        return {};
    }

    const QGeoCoordinate coordinate = vehicle->coordinate();
    const QGeoCoordinate home = vehicle->homePosition();
    const auto coordinateJson = [](const QGeoCoordinate& value) -> QJsonValue {
        if (!value.isValid()) {
            return QJsonValue(QJsonValue::Null);
        }
        return QJsonObject{{QStringLiteral("latitude"), value.latitude()},
                           {QStringLiteral("longitude"), value.longitude()},
                           {QStringLiteral("altitude_m"), value.altitude()}};
    };

    return QJsonObject{{QStringLiteral("id"), vehicle->id()},
                       {QStringLiteral("active"), MultiVehicleManager::instance()->activeVehicle() == vehicle},
                       {QStringLiteral("armed"), vehicle->armed()},
                       {QStringLiteral("flying"), vehicle->flying()},
                       {QStringLiteral("flight_mode"), vehicle->flightMode()},
                       {QStringLiteral("firmware"), vehicle->firmwareTypeString()},
                       {QStringLiteral("vehicle_type"), vehicle->vehicleTypeString()},
                       {QStringLiteral("coordinate"), coordinateJson(coordinate)},
                       {QStringLiteral("home"), coordinateJson(home)}};
}

QHttpServerResponse QGCMcpControlServer::_jsonResponse(const QJsonObject& json, QHttpServerResponse::StatusCode status)
{
    return QHttpServerResponse(QByteArrayLiteral("application/json"),
                               QJsonDocument(json).toJson(QJsonDocument::Compact), status);
}

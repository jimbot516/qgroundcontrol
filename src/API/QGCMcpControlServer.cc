#include "QGCMcpControlServer.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtHttpServer/QHttpServerRequest>
#include <QtHttpServer/QHttpServerResponse>
#include <QtNetwork/QHostAddress>
#include <cmath>

#include "BatteryFactGroupListModel.h"
#include "Fact.h"
#include "HealthAndArmingCheckReport.h"
#include "MissionController.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QGCMAVLink.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

QGC_LOGGING_CATEGORY(QGCMcpControlServerLog, "API.QGCMcpControlServer")

namespace {

constexpr qsizetype MAX_REQUEST_BYTES = 1024 * 1024;
constexpr qsizetype MAX_PLAN_FILE_BYTES = 10 * 1024 * 1024;

QJsonValue finiteFactValue(Fact* fact)
{
    if (!fact) {
        return QJsonValue(QJsonValue::Null);
    }
    const double value = fact->rawValue().toDouble();
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

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
    MultiVehicleManager* manager = MultiVehicleManager::instance();
    (void) connect(manager, &MultiVehicleManager::vehicleAdded, this, &QGCMcpControlServer::_trackVehicle);
    const QmlObjectListModel* vehicleModel = manager->vehicles();
    for (int index = 0; index < vehicleModel->count(); ++index) {
        _trackVehicle(vehicleModel->value<Vehicle*>(index));
    }

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
    if (name == QStringLiteral("get_health")) {
        return _health(arguments);
    }
    if (name == QStringLiteral("get_command_status")) {
        return _commandStatus(arguments);
    }
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

QGCMcpControlServer::ActionResult QGCMcpControlServer::_health(const QJsonObject& arguments)
{
    QString error;
    Vehicle* vehicle = _vehicle(arguments, error);
    if (!vehicle) {
        return _failure(error);
    }

    HealthAndArmingCheckReport* report = vehicle->healthAndArmingCheckReport();
    QJsonArray problems;
    if (report) {
        QmlObjectListModel* problemModel = report->problemsForCurrentMode();
        for (int index = 0; index < problemModel->count(); ++index) {
            HealthAndArmingCheckProblem* problem = problemModel->value<HealthAndArmingCheckProblem*>(index);
            if (problem) {
                problems.append(QJsonObject{{QStringLiteral("message"), problem->message()},
                                            {QStringLiteral("description"), problem->description()},
                                            {QStringLiteral("severity"), problem->severity()}});
            }
        }
    }

    QJsonArray batteries;
    QmlObjectListModel* batteryModel = vehicle->batteries();
    for (int index = 0; index < batteryModel->count(); ++index) {
        BatteryFactGroup* battery = batteryModel->value<BatteryFactGroup*>(index);
        if (battery) {
            batteries.append(
                QJsonObject{{QStringLiteral("id"), finiteFactValue(battery->id())},
                            {QStringLiteral("percent_remaining"), finiteFactValue(battery->percentRemaining())},
                            {QStringLiteral("voltage_v"), finiteFactValue(battery->voltage())},
                            {QStringLiteral("current_a"), finiteFactValue(battery->current())},
                            {QStringLiteral("temperature_c"), finiteFactValue(battery->temperature())},
                            {QStringLiteral("charge_state"), finiteFactValue(battery->chargeState())}});
        }
    }

    const bool reportSupported = report && report->supported();
    const bool canArm = reportSupported ? report->canArm()
                                        : (vehicle->readyToFlyAvailable()
                                               ? vehicle->readyToFly()
                                               : (vehicle->allSensorsHealthy() && vehicle->prearmError().isEmpty()));
    const QJsonValue readyToFly =
        vehicle->readyToFlyAvailable() ? QJsonValue(vehicle->readyToFly()) : QJsonValue(QJsonValue::Null);

    return _success(QJsonObject{
        {QStringLiteral("vehicle_id"), vehicle->id()},
        {QStringLiteral("can_arm"), canArm},
        {QStringLiteral("can_takeoff"), reportSupported ? report->canTakeoff() : canArm},
        {QStringLiteral("can_start_mission"), reportSupported ? report->canStartMission() : canArm},
        {QStringLiteral("ready_to_fly"), readyToFly},
        {QStringLiteral("ready_to_fly_available"), vehicle->readyToFlyAvailable()},
        {QStringLiteral("all_sensors_healthy"), vehicle->allSensorsHealthy()},
        {QStringLiteral("prearm_error"), vehicle->prearmError()},
        {QStringLiteral("gps_state"), reportSupported ? report->gpsState() : QString()},
        {QStringLiteral("health_report_supported"), reportSupported},
        {QStringLiteral("has_warnings_or_errors"), reportSupported && report->hasWarningsOrErrors()},
        {QStringLiteral("sensor_bits"), QJsonObject{{QStringLiteral("present"), vehicle->sensorsPresentBits()},
                                                    {QStringLiteral("enabled"), vehicle->sensorsEnabledBits()},
                                                    {QStringLiteral("healthy"), vehicle->sensorsHealthBits()},
                                                    {QStringLiteral("unhealthy"), vehicle->sensorsUnhealthyBits()}}},
        {QStringLiteral("link"),
         QJsonObject{{QStringLiteral("messages_received"), static_cast<double>(vehicle->mavlinkReceivedCount())},
                     {QStringLiteral("messages_sent"), static_cast<double>(vehicle->mavlinkSentCount())},
                     {QStringLiteral("messages_lost"), static_cast<double>(vehicle->mavlinkLossCount())},
                     {QStringLiteral("loss_percent"), vehicle->mavlinkLossPercent()}}},
        {QStringLiteral("batteries"), batteries},
        {QStringLiteral("problems"), problems},
    });
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_commandStatus(const QJsonObject& arguments)
{
    const QJsonValue afterValue = arguments.value(QStringLiteral("after_sequence"));
    const double afterNumeric = afterValue.isUndefined() ? 0.0 : afterValue.toDouble(-1.0);
    if (!std::isfinite(afterNumeric) || (afterNumeric < 0.0) || (std::floor(afterNumeric) != afterNumeric)) {
        return _failure(QStringLiteral("after_sequence must be a non-negative integer"));
    }
    const quint64 afterSequence = static_cast<quint64>(afterNumeric);

    const QJsonValue limitValue = arguments.value(QStringLiteral("limit"));
    const double limitNumeric = limitValue.isUndefined() ? 20.0 : limitValue.toDouble(-1.0);
    if (!std::isfinite(limitNumeric) || (limitNumeric < 1.0) || (limitNumeric > 100.0) ||
        (std::floor(limitNumeric) != limitNumeric)) {
        return _failure(QStringLiteral("limit must be an integer between 1 and 100"));
    }
    const int limit = static_cast<int>(limitNumeric);

    int vehicleId = -1;
    if (arguments.contains(QStringLiteral("vehicle_id"))) {
        QString error;
        Vehicle* vehicle = _vehicle(arguments, error);
        if (!vehicle) {
            return _failure(error);
        }
        vehicleId = vehicle->id();
    }

    int command = -1;
    if (arguments.contains(QStringLiteral("command"))) {
        const double commandNumeric = arguments.value(QStringLiteral("command")).toDouble(-1.0);
        if (!std::isfinite(commandNumeric) || (commandNumeric < 0.0) || (commandNumeric > 65535.0) ||
            (std::floor(commandNumeric) != commandNumeric)) {
            return _failure(QStringLiteral("command must be an integer between 0 and 65535"));
        }
        command = static_cast<int>(commandNumeric);
    }

    int targetComponent = -1;
    if (arguments.contains(QStringLiteral("target_component"))) {
        const double componentNumeric = arguments.value(QStringLiteral("target_component")).toDouble(-1.0);
        if (!std::isfinite(componentNumeric) || (componentNumeric < 0.0) || (componentNumeric > 255.0) ||
            (std::floor(componentNumeric) != componentNumeric)) {
            return _failure(QStringLiteral("target_component must be an integer between 0 and 255"));
        }
        targetComponent = static_cast<int>(componentNumeric);
    }

    QJsonArray matches;
    for (const QJsonValue& value : std::as_const(_commandResults)) {
        const QJsonObject result = value.toObject();
        if ((static_cast<quint64>(result.value(QStringLiteral("sequence")).toDouble()) <= afterSequence) ||
            ((vehicleId >= 0) && (result.value(QStringLiteral("vehicle_id")).toInt() != vehicleId)) ||
            ((command >= 0) && (result.value(QStringLiteral("command")).toInt() != command)) ||
            ((targetComponent >= 0) && (result.value(QStringLiteral("target_component")).toInt() != targetComponent))) {
            continue;
        }
        matches.append(result);
        if (matches.size() >= limit) {
            break;
        }
    }

    QJsonValue pending(QJsonValue::Null);
    if (command >= 0) {
        QString error;
        Vehicle* vehicle = _vehicle(arguments, error);
        if (vehicle) {
            const int component = targetComponent >= 0 ? targetComponent : vehicle->defaultComponentId();
            pending = vehicle->isMavCommandPending(component, static_cast<MAV_CMD>(command));
        }
    }

    return _success(QJsonObject{
        {QStringLiteral("latest_sequence"), static_cast<double>(_nextCommandSequence - 1)},
        {QStringLiteral("pending"), pending},
        {QStringLiteral("results"), matches},
    });
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

    const quint64 commandStatusAfterSequence = _nextCommandSequence - 1;

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

    return _success(QJsonObject{
        {QStringLiteral("accepted"), true},
        {QStringLiteral("command_status_after_sequence"), static_cast<double>(commandStatusAfterSequence)},
        {QStringLiteral("vehicle"), _vehicleJson(vehicle)},
    });
}

QGCMcpControlServer::ActionResult QGCMcpControlServer::_planAction(const QString& name, const QJsonObject& arguments)
{
    if (name == QStringLiteral("plan_validate")) {
        return _planValidation(arguments);
    }
    if (_planController.syncInProgress()) {
        return _failure(QStringLiteral("A plan upload or download is already in progress"));
    }

    QString error;
    if (name == QStringLiteral("plan_get")) {
        return _success(QJsonObject{{QStringLiteral("plan"), _planController.saveToJson().object()},
                                    {QStringLiteral("dirty_for_upload"), _planController.dirtyForUpload()}});
    }
    if (name == QStringLiteral("plan_download")) {
        if (_planController.offline()) {
            return _failure(QStringLiteral("No active vehicle is connected"));
        }
        if (_planController.dirtyForUpload() &&
            !arguments.value(QStringLiteral("confirm_discard_local_changes")).toBool(false)) {
            return _failure(
                QStringLiteral("The editable plan has local changes; set confirm_discard_local_changes to true"));
        }
        _planController.loadFromVehicle();
        if (!_planController.syncInProgress()) {
            return _failure(QStringLiteral("QGroundControl could not start the plan download"));
        }
        return _success(QJsonObject{{QStringLiteral("accepted"), true}, {QStringLiteral("sync_in_progress"), true}});
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

QGCMcpControlServer::ActionResult QGCMcpControlServer::_planValidation(const QJsonObject& arguments)
{
    QJsonArray errors;
    QJsonArray warnings;
    QJsonArray uploadBlockers;
    QString readyForSave;
    const int readyState = _planController.readyForSaveState();
    switch (readyState) {
        case VisualMissionItem::ReadyForSave:
            readyForSave = QStringLiteral("ready");
            break;
        case VisualMissionItem::NotReadyForSaveTerrain:
            readyForSave = QStringLiteral("waiting_for_terrain");
            errors.append(QStringLiteral("The plan is waiting for terrain data"));
            break;
        case VisualMissionItem::NotReadyForSaveData:
            readyForSave = QStringLiteral("incomplete_data");
            errors.append(QStringLiteral("One or more mission items are incomplete"));
            break;
        default:
            readyForSave = QStringLiteral("unknown");
            errors.append(QStringLiteral("QGroundControl returned an unknown plan readiness state"));
            break;
    }

    if (_planController.syncInProgress()) {
        uploadBlockers.append(QStringLiteral("A plan upload or download is already in progress"));
    }

    int missionItemCount = 0;
    int invalidCoordinateCount = 0;
    int terrainCollisionCount = 0;
    bool hasTakeoff = false;
    bool hasLanding = false;
    QmlObjectListModel* visualItems = _planController.missionController()->visualItems();
    for (int index = 0; index < visualItems->count(); ++index) {
        VisualMissionItem* item = visualItems->value<VisualMissionItem*>(index);
        if (!item || item->homePosition()) {
            continue;
        }
        ++missionItemCount;
        hasTakeoff = hasTakeoff || item->isTakeoffItem();
        hasLanding = hasLanding || item->isLandCommand();
        if (item->specifiesCoordinate() && !item->coordinate().isValid()) {
            ++invalidCoordinateCount;
        }
        if (item->terrainCollision()) {
            ++terrainCollisionCount;
        }
    }

    if (invalidCoordinateCount > 0) {
        errors.append(QStringLiteral("%1 mission item(s) have invalid coordinates").arg(invalidCoordinateCount));
    }
    if (terrainCollisionCount > 0) {
        warnings.append(QStringLiteral("%1 mission item(s) report a terrain collision").arg(terrainCollisionCount));
    }
    if (missionItemCount == 0) {
        warnings.append(QStringLiteral("The mission is empty"));
    } else {
        if (!hasTakeoff) {
            warnings.append(QStringLiteral("The mission has no explicit takeoff item"));
        }
        if (!hasLanding) {
            warnings.append(QStringLiteral("The mission has no explicit landing item"));
        }
    }

    QString uploadPrecheck;
    const MissionController::SendToVehiclePreCheckState preCheck =
        _planController.missionController()->sendToVehiclePreCheck();
    switch (preCheck) {
        case MissionController::SendToVehiclePreCheckStateOk:
            uploadPrecheck = QStringLiteral("ok");
            break;
        case MissionController::SendToVehiclePreCheckStateNoActiveVehicle:
            uploadPrecheck = QStringLiteral("no_active_vehicle");
            uploadBlockers.append(QStringLiteral("No active vehicle is connected"));
            break;
        case MissionController::SendToVehiclePreCheckStateFirwmareVehicleMismatch:
            uploadPrecheck = QStringLiteral("firmware_vehicle_mismatch");
            if (arguments.value(QStringLiteral("allow_firmware_mismatch")).toBool(false)) {
                warnings.append(QStringLiteral("Plan firmware or vehicle type does not match the active vehicle"));
            } else {
                uploadBlockers.append(
                    QStringLiteral("Plan firmware or vehicle type does not match the active vehicle"));
            }
            break;
        case MissionController::SendToVehiclePreCheckStateActiveMission:
            uploadPrecheck = QStringLiteral("active_mission");
            uploadBlockers.append(QStringLiteral("Pause the active mission before uploading a replacement plan"));
            break;
    }

    const QJsonObject plan = _planController.saveToJson().object();
    const QJsonObject geoFence = plan.value(QStringLiteral("geoFence")).toObject();
    const int rallyPointCount = _planController.rallyPointController()->points()->count();
    const bool planValid = errors.isEmpty();
    return _success(QJsonObject{
        {QStringLiteral("plan_valid"), planValid},
        {QStringLiteral("upload_ready"), planValid && uploadBlockers.isEmpty() && !_planController.syncInProgress()},
        {QStringLiteral("ready_for_save"), readyForSave},
        {QStringLiteral("upload_precheck"), uploadPrecheck},
        {QStringLiteral("dirty_for_upload"), _planController.dirtyForUpload()},
        {QStringLiteral("counts"),
         QJsonObject{
             {QStringLiteral("mission_items"), missionItemCount},
             {QStringLiteral("rally_points"), rallyPointCount},
             {QStringLiteral("geofence_circles"), geoFence.value(QStringLiteral("circles")).toArray().size()},
             {QStringLiteral("geofence_polygons"), geoFence.value(QStringLiteral("polygons")).toArray().size()}}},
        {QStringLiteral("has_takeoff"), hasTakeoff},
        {QStringLiteral("has_landing"), hasLanding},
        {QStringLiteral("errors"), errors},
        {QStringLiteral("warnings"), warnings},
        {QStringLiteral("upload_blockers"), uploadBlockers},
    });
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

void QGCMcpControlServer::_trackVehicle(Vehicle* vehicle)
{
    if (!vehicle) {
        return;
    }
    (void) connect(vehicle, &Vehicle::mavCommandResult, this, &QGCMcpControlServer::_mavCommandResult,
                   Qt::UniqueConnection);
}

void QGCMcpControlServer::_mavCommandResult(int vehicleId, int targetComponent, int command, int ackResult,
                                            int failureCode)
{
    const auto typedFailureCode = static_cast<Vehicle::MavCmdResultFailureCode_t>(failureCode);
    const bool accepted =
        (typedFailureCode == Vehicle::MavCmdResultCommandResultOnly) && (ackResult == MAV_RESULT_ACCEPTED);
    _commandResults.append(QJsonObject{
        {QStringLiteral("sequence"), static_cast<double>(_nextCommandSequence++)},
        {QStringLiteral("recorded_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("vehicle_id"), vehicleId},
        {QStringLiteral("target_component"), targetComponent},
        {QStringLiteral("command"), command},
        {QStringLiteral("ack_result"), ackResult},
        {QStringLiteral("ack_result_name"), QGCMAVLink::mavResultToString(static_cast<uint8_t>(ackResult))},
        {QStringLiteral("failure_code"), failureCode},
        {QStringLiteral("failure"), Vehicle::mavCmdResultFailureCodeToString(typedFailureCode)},
        {QStringLiteral("accepted"), accepted},
    });
    while (_commandResults.size() > MAX_COMMAND_RESULTS) {
        _commandResults.removeAt(0);
    }
}

QHttpServerResponse QGCMcpControlServer::_jsonResponse(const QJsonObject& json, QHttpServerResponse::StatusCode status)
{
    return QHttpServerResponse(QByteArrayLiteral("application/json"),
                               QJsonDocument(json).toJson(QJsonDocument::Compact), status);
}

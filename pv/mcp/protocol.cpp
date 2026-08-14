/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "protocol.hpp"

namespace pv {
namespace mcp {

Protocol::Protocol(const ToolProvider& tools) :
	tools_(tools)
{
}

bool Protocol::handle(const QJsonObject& request, QJsonObject& response) const
{
	const QJsonValue id = request.value(QStringLiteral("id"));
	const bool notification = id.isUndefined();

	if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
		response = error_response(id, -32600, QStringLiteral("Invalid JSON-RPC request"));
		return !notification;
	}

	const QString method = request.value(QStringLiteral("method")).toString();
	if (method.isEmpty()) {
		response = error_response(id, -32600, QStringLiteral("Missing method"));
		return !notification;
	}

	if (notification)
		return false;

	if (method == QStringLiteral("initialize")) {
		const QJsonObject params = request.value(QStringLiteral("params")).toObject();
		QString version = params.value(QStringLiteral("protocolVersion")).toString();
		if (version.isEmpty())
			version = QStringLiteral("2025-06-18");

		QJsonObject server_info;
		server_info.insert(QStringLiteral("name"), QStringLiteral("pulseview"));
		server_info.insert(QStringLiteral("version"), QStringLiteral("0.1"));

		QJsonObject tool_capabilities;
		tool_capabilities.insert(QStringLiteral("listChanged"), false);
		QJsonObject capabilities;
		capabilities.insert(QStringLiteral("tools"), tool_capabilities);

		QJsonObject result;
		result.insert(QStringLiteral("protocolVersion"), version);
		result.insert(QStringLiteral("capabilities"), capabilities);
		result.insert(QStringLiteral("serverInfo"), server_info);
		response = success_response(id, result);
		return true;
	}

	if (method == QStringLiteral("tools/list")) {
		QJsonObject result;
		result.insert(QStringLiteral("tools"), tools_.list_tools());
		response = success_response(id, result);
		return true;
	}

	if (method == QStringLiteral("tools/call")) {
		const QJsonObject params = request.value(QStringLiteral("params")).toObject();
		const QString name = params.value(QStringLiteral("name")).toString();
		const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
		if (name.isEmpty()) {
			response = error_response(id, -32602, QStringLiteral("Missing tool name"));
			return true;
		}

		QJsonObject result;
		QString error;
		if (!tools_.call_tool(name, arguments, result, error)) {
			response = error_response(id, -32602, error);
			return true;
		}

		response = success_response(id, result);
		return true;
	}

	response = error_response(id, -32601, QStringLiteral("Method not found"));
	return true;
}

QJsonObject Protocol::error_response(const QJsonValue& id, int code,
	const QString& message) const
{
	QJsonObject error;
	error.insert(QStringLiteral("code"), code);
	error.insert(QStringLiteral("message"), message);

	QJsonObject response;
	response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
	response.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue() : id);
	response.insert(QStringLiteral("error"), error);
	return response;
}

QJsonObject Protocol::success_response(const QJsonValue& id,
	const QJsonObject& result) const
{
	QJsonObject response;
	response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
	response.insert(QStringLiteral("id"), id);
	response.insert(QStringLiteral("result"), result);
	return response;
}

} // namespace mcp
} // namespace pv

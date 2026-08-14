/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PULSEVIEW_PV_MCP_PROTOCOL_HPP
#define PULSEVIEW_PV_MCP_PROTOCOL_HPP

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace pv {
namespace mcp {

class ToolProvider
{
public:
	virtual ~ToolProvider() = default;
	virtual QJsonArray list_tools() const = 0;
	virtual bool call_tool(const QString& name, const QJsonObject& arguments,
		QJsonObject& result, QString& error) const = 0;
};

class Protocol
{
public:
	explicit Protocol(const ToolProvider& tools);

	// Returns false for notifications, which have no JSON-RPC response.
	bool handle(const QJsonObject& request, QJsonObject& response) const;

private:
	QJsonObject error_response(const QJsonValue& id, int code,
		const QString& message) const;
	QJsonObject success_response(const QJsonValue& id,
		const QJsonObject& result) const;

	const ToolProvider& tools_;
};

} // namespace mcp
} // namespace pv

#endif // PULSEVIEW_PV_MCP_PROTOCOL_HPP

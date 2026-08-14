/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PULSEVIEW_PV_MCP_TOOLS_HPP
#define PULSEVIEW_PV_MCP_TOOLS_HPP

#include "protocol.hpp"

namespace pv {
namespace mcp {

class SessionRegistry;

class Tools : public ToolProvider
{
public:
	explicit Tools(SessionRegistry& sessions);

	QJsonArray list_tools() const override;
	bool call_tool(const QString& name, const QJsonObject& arguments,
		QJsonObject& result, QString& error) const override;

private:
	QJsonObject list_sessions() const;
	bool get_view_context(const QJsonObject& arguments,
		QJsonObject& result, QString& error) const;
	bool query_annotations(const QJsonObject& arguments,
		QJsonObject& result, QString& error) const;
	bool set_cursors(const QJsonObject& arguments,
		QJsonObject& result, QString& error) const;
	bool zoom_to_range(const QJsonObject& arguments,
		QJsonObject& result, QString& error) const;

	SessionRegistry& sessions_;
};

} // namespace mcp
} // namespace pv

#endif // PULSEVIEW_PV_MCP_TOOLS_HPP

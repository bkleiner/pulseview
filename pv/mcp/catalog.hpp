/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PULSEVIEW_PV_MCP_CATALOG_HPP
#define PULSEVIEW_PV_MCP_CATALOG_HPP

#include <QJsonArray>
#include <QString>

namespace pv {
namespace mcp {

QJsonArray tool_catalog();
QString server_instructions();

} // namespace mcp
} // namespace pv

#endif // PULSEVIEW_PV_MCP_CATALOG_HPP

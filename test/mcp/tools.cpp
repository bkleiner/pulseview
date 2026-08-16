/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <boost/test/unit_test.hpp>

#include "pv/mcp/sessionregistry.hpp"
#include "pv/mcp/tools.hpp"
#include "test/test.hpp"

BOOST_AUTO_TEST_SUITE(McpToolsTest)

BOOST_AUTO_TEST_CASE(tool_surface)
{
	pv::mcp::SessionRegistry sessions;
	pv::mcp::Tools tools(sessions);
	const QJsonArray listed = tools.list_tools();
	BOOST_REQUIRE_EQUAL(listed.size(), 9);

	const QStringList expected = {QStringLiteral("get_session"),
		QStringLiteral("get_capture"), QStringLiteral("start_capture"),
		QStringLiteral("set_session"), QStringLiteral("save_session"),
		QStringLiteral("load_session"), QStringLiteral("query_annotations"),
		QStringLiteral("set_cursors"), QStringLiteral("zoom_to_range")};
	for (int index = 0; index < listed.size(); index++)
		BOOST_CHECK_EQUAL(listed.at(index).toObject().value(QStringLiteral("name"))
			.toString(), expected.at(index));

	BOOST_CHECK(listed.at(6).toObject().value(QStringLiteral("annotations"))
		.toObject().value(QStringLiteral("readOnlyHint")).toBool());
	BOOST_CHECK(!listed.at(2).toObject().value(QStringLiteral("annotations"))
		.toObject().value(QStringLiteral("readOnlyHint")).toBool());
	BOOST_CHECK(!listed.at(7).toObject().value(QStringLiteral("annotations"))
		.toObject().value(QStringLiteral("readOnlyHint")).toBool());

	const QJsonObject query_tool = listed.at(6).toObject();
	const QJsonObject query_properties = query_tool.value(QStringLiteral("inputSchema"))
		.toObject().value(QStringLiteral("properties")).toObject();
	BOOST_CHECK(query_properties.contains(QStringLiteral("continuation_token")));
	BOOST_CHECK_EQUAL(query_properties.value(QStringLiteral("limit")).toObject()
		.value(QStringLiteral("maximum")).toInt(), 1000);
}

BOOST_AUTO_TEST_CASE(write_tools_require_a_session)
{
	pv::mcp::SessionRegistry sessions;
	pv::mcp::Tools tools(sessions);
	QJsonObject result;
	QString error;
	BOOST_CHECK(!tools.call_tool(QStringLiteral("set_cursors"),
		QJsonObject{{QStringLiteral("generation"), QStringLiteral("1")},
			{QStringLiteral("visible"), false}}, result, error));
	BOOST_CHECK_EQUAL(error, QStringLiteral("No active PulseView session"));
}

BOOST_AUTO_TEST_SUITE_END()

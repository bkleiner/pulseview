/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <boost/test/unit_test.hpp>

#include "pv/mcp/protocol.hpp"
#include "test/test.hpp"

namespace {

class StubTools : public pv::mcp::ToolProvider
{
public:
	QJsonArray list_tools() const override
	{
		QJsonObject tool;
		tool.insert(QStringLiteral("name"), QStringLiteral("test_tool"));
		return QJsonArray{tool};
	}

	bool call_tool(const QString& name, const QJsonObject& arguments,
		QJsonObject& result, QString& error) const override
	{
		(void)arguments;
		if (name != QStringLiteral("test_tool")) {
			error = QStringLiteral("unknown tool");
			return false;
		}

		result.insert(QStringLiteral("ok"), true);
		return true;
	}
};

QJsonObject request(int id, const QString& method)
{
	QJsonObject value;
	value.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
	value.insert(QStringLiteral("id"), id);
	value.insert(QStringLiteral("method"), method);
	return value;
}

} // namespace

BOOST_AUTO_TEST_SUITE(McpProtocolTest)

BOOST_AUTO_TEST_CASE(initialize)
{
	StubTools tools;
	pv::mcp::Protocol protocol(tools);
	QJsonObject input = request(1, QStringLiteral("initialize"));
	QJsonObject params;
	params.insert(QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18"));
	input.insert(QStringLiteral("params"), params);

	QJsonObject response;
	BOOST_REQUIRE(protocol.handle(input, response));
	BOOST_CHECK_EQUAL(response.value(QStringLiteral("id")).toInt(), 1);
	const QJsonObject result = response.value(QStringLiteral("result")).toObject();
	BOOST_CHECK_EQUAL(result.value(QStringLiteral("protocolVersion")).toString(),
		QStringLiteral("2025-06-18"));
	BOOST_CHECK(result.value(QStringLiteral("capabilities")).toObject()
		.contains(QStringLiteral("tools")));
	BOOST_CHECK(result.value(QStringLiteral("instructions")).toString()
		.contains(QStringLiteral("active PulseView session")));
}

BOOST_AUTO_TEST_CASE(list_and_call_tools)
{
	StubTools tools;
	pv::mcp::Protocol protocol(tools);

	QJsonObject response;
	BOOST_REQUIRE(protocol.handle(request(2, QStringLiteral("tools/list")), response));
	const QJsonArray listed = response.value(QStringLiteral("result")).toObject()
		.value(QStringLiteral("tools")).toArray();
	BOOST_REQUIRE_EQUAL(listed.size(), 1);
	BOOST_CHECK_EQUAL(listed.at(0).toObject().value(QStringLiteral("name")).toString(),
		QStringLiteral("test_tool"));

	QJsonObject call = request(3, QStringLiteral("tools/call"));
	QJsonObject params;
	params.insert(QStringLiteral("name"), QStringLiteral("test_tool"));
	params.insert(QStringLiteral("arguments"), QJsonObject());
	call.insert(QStringLiteral("params"), params);
	BOOST_REQUIRE(protocol.handle(call, response));
	BOOST_CHECK(response.value(QStringLiteral("result")).toObject()
		.value(QStringLiteral("ok")).toBool());
}

BOOST_AUTO_TEST_CASE(notification_has_no_response)
{
	StubTools tools;
	pv::mcp::Protocol protocol(tools);
	QJsonObject notification;
	notification.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
	notification.insert(QStringLiteral("method"),
		QStringLiteral("notifications/initialized"));

	QJsonObject response;
	BOOST_CHECK(!protocol.handle(notification, response));
}

BOOST_AUTO_TEST_SUITE_END()

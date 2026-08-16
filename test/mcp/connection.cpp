/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <boost/test/unit_test.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>

#include "pv/mcp/server.hpp"
#include "pv/mcp/sessionregistry.hpp"

namespace {

QCoreApplication& application()
{
	static int argc = 1;
	static char program_name[] = "pulseview-mcp-test";
	static char *argv[] = {program_name, nullptr};
	static QCoreApplication instance(argc, argv);
	return instance;
}

bool wait_for_line(QLocalSocket& socket, int timeout_ms)
{
	QElapsedTimer timer;
	timer.start();
	while (!socket.canReadLine() && timer.elapsed() < timeout_ms) {
		application().processEvents(QEventLoop::AllEvents, 10);
		QThread::msleep(1);
	}
	return socket.canReadLine();
}

bool wait_for_line(QProcess& process, int timeout_ms)
{
	QElapsedTimer timer;
	timer.start();
	while (!process.canReadLine() && timer.elapsed() < timeout_ms) {
		application().processEvents(QEventLoop::AllEvents, 10);
		process.waitForReadyRead(10);
	}
	return process.canReadLine();
}

} // namespace

BOOST_AUTO_TEST_SUITE(McpConnectionTest)

BOOST_AUTO_TEST_CASE(unix_socket_round_trip)
{
	(void)application();
	const QString socket_path = QDir::temp().filePath(
		QStringLiteral("pulseview-mcp-test-%1.sock")
			.arg(QCoreApplication::applicationPid()));

	pv::mcp::SessionRegistry sessions;
	pv::mcp::Server server(sessions);
	const bool listening = server.listen(socket_path);
	BOOST_REQUIRE_MESSAGE(listening, server.error_string().toStdString());

	const QFileInfo socket_info(socket_path);
	BOOST_CHECK(socket_info.permission(QFileDevice::ReadOwner));
	BOOST_CHECK(socket_info.permission(QFileDevice::WriteOwner));
	BOOST_CHECK(!socket_info.permission(QFileDevice::ReadGroup));
	BOOST_CHECK(!socket_info.permission(QFileDevice::ReadOther));

	QLocalSocket client;
	client.connectToServer(socket_path);
	BOOST_REQUIRE(client.waitForConnected(1000));
	application().processEvents();

	QJsonObject request;
	request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
	request.insert(QStringLiteral("id"), 1);
	request.insert(QStringLiteral("method"), QStringLiteral("tools/list"));
	request.insert(QStringLiteral("params"), QJsonObject());
	client.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
	client.flush();

	BOOST_REQUIRE(wait_for_line(client, 1000));
	QJsonParseError error;
	const QJsonDocument response = QJsonDocument::fromJson(client.readLine(), &error);
	BOOST_REQUIRE_EQUAL(error.error, QJsonParseError::NoError);
	BOOST_REQUIRE(response.isObject());
	const QJsonArray tools = response.object().value(QStringLiteral("result"))
		.toObject().value(QStringLiteral("tools")).toArray();
	BOOST_REQUIRE_EQUAL(tools.size(), 9);
	BOOST_CHECK_EQUAL(tools.at(0).toObject().value(QStringLiteral("name"))
		.toString().toStdString(), "get_session");
	BOOST_CHECK_EQUAL(tools.at(6).toObject().value(QStringLiteral("name"))
		.toString().toStdString(), "query_annotations");
	BOOST_CHECK_EQUAL(tools.at(7).toObject().value(QStringLiteral("name"))
		.toString().toStdString(), "set_cursors");
	BOOST_CHECK_EQUAL(tools.at(8).toObject().value(QStringLiteral("name"))
		.toString().toStdString(), "zoom_to_range");
}

BOOST_AUTO_TEST_CASE(bridge_survives_and_reconnects)
{
	(void)application();
	const QString socket_path = QDir::temp().filePath(
		QStringLiteral("pulseview-mcp-bridge-test-%1.sock")
			.arg(QCoreApplication::applicationPid()));
	const QString bridge_path = QDir(QCoreApplication::applicationDirPath())
		.absoluteFilePath(QStringLiteral("../pulseview-mcp"));
	BOOST_REQUIRE(QFileInfo::exists(bridge_path));

	QProcess bridge;
	bridge.setProgram(bridge_path);
	bridge.setArguments({QStringLiteral("--socket"), socket_path});
	bridge.start();
	BOOST_REQUIRE(bridge.waitForStarted(1000));

	auto send = [&](int id, const QString& method, const QJsonObject& params = QJsonObject()) {
		QJsonObject request{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
			{QStringLiteral("id"), id}, {QStringLiteral("method"), method},
			{QStringLiteral("params"), params}};
		bridge.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
		bridge.waitForBytesWritten(1000);
		BOOST_REQUIRE(wait_for_line(bridge, 2000));
		return QJsonDocument::fromJson(bridge.readLine()).object();
	};

	QJsonObject response = send(1, QStringLiteral("initialize"),
		QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")}});
	BOOST_CHECK_EQUAL(response.value(QStringLiteral("id")).toInt(), 1);
	BOOST_CHECK(bridge.state() == QProcess::Running);

	response = send(2, QStringLiteral("tools/call"), QJsonObject{
		{QStringLiteral("name"), QStringLiteral("get_session")},
		{QStringLiteral("arguments"), QJsonObject()}});
	BOOST_CHECK(response.value(QStringLiteral("result")).toObject()
		.value(QStringLiteral("isError")).toBool());
	BOOST_CHECK(bridge.state() == QProcess::Running);

	pv::mcp::SessionRegistry sessions;
	{
		pv::mcp::Server server(sessions);
		BOOST_REQUIRE_MESSAGE(server.listen(socket_path), server.error_string().toStdString());
		response = send(3, QStringLiteral("tools/call"), QJsonObject{
			{QStringLiteral("name"), QStringLiteral("get_session")},
			{QStringLiteral("arguments"), QJsonObject()}});
		BOOST_CHECK(response.value(QStringLiteral("error")).toObject()
			.value(QStringLiteral("message")).toString()
			.contains(QStringLiteral("No active PulseView session")));
		BOOST_CHECK(bridge.state() == QProcess::Running);
	}
	application().processEvents();
	response = send(4, QStringLiteral("tools/call"), QJsonObject{
		{QStringLiteral("name"), QStringLiteral("get_session")},
		{QStringLiteral("arguments"), QJsonObject()}});
	BOOST_CHECK(response.value(QStringLiteral("result")).toObject()
		.value(QStringLiteral("isError")).toBool());
	BOOST_CHECK(bridge.state() == QProcess::Running);
	{
		pv::mcp::Server restarted_server(sessions);
		BOOST_REQUIRE_MESSAGE(restarted_server.listen(socket_path),
			restarted_server.error_string().toStdString());
		response = send(5, QStringLiteral("tools/call"), QJsonObject{
			{QStringLiteral("name"), QStringLiteral("get_session")},
			{QStringLiteral("arguments"), QJsonObject()}});
		BOOST_CHECK(response.value(QStringLiteral("error")).toObject()
			.value(QStringLiteral("message")).toString()
			.contains(QStringLiteral("No active PulseView session")));
	}

	bridge.closeWriteChannel();
	BOOST_CHECK(bridge.waitForFinished(1000));
}

BOOST_AUTO_TEST_SUITE_END()

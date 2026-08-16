/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <unistd.h>

#include "pv/mcp/catalog.hpp"

namespace {

QString default_socket_path()
{
	#ifdef Q_OS_UNIX
	return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
		.filePath(QStringLiteral("pulseview-mcp-%1.sock").arg(::getuid()));
	#else
	QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
	if (directory.isEmpty())
		directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	return QDir(directory).filePath(QStringLiteral("pulseview-mcp.sock"));
	#endif
}

void write_message(const QJsonObject& message)
{
	const QByteArray output = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
	fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
	fflush(stdout);
}

QJsonObject error_response(const QJsonValue& id, int code, const QString& message)
{
	return QJsonObject{
		{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
		{QStringLiteral("id"), id},
		{QStringLiteral("error"), QJsonObject{
			{QStringLiteral("code"), code},
			{QStringLiteral("message"), message}}}};
}

QJsonObject tool_error_response(const QJsonValue& id, const QString& message)
{
	const QJsonObject content_item{
		{QStringLiteral("type"), QStringLiteral("text")},
		{QStringLiteral("text"), message}};
	return QJsonObject{
		{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
		{QStringLiteral("id"), id},
		{QStringLiteral("result"), QJsonObject{
			{QStringLiteral("content"), QJsonArray{content_item}},
			{QStringLiteral("isError"), true}}}};
}

class Bridge : public QObject
{
public:
	Bridge(const QString& socket_path, QObject *parent = nullptr) :
		QObject(parent), socket_path_(socket_path), stdin_notifier_(STDIN_FILENO,
			QSocketNotifier::Read, this)
	{
		connect(&stdin_notifier_, &QSocketNotifier::activated,
			this, [this](int) { read_stdin(); });
		connect(&socket_, &QLocalSocket::readyRead, this, [this]() { read_backend(); });
		connect(&socket_, &QLocalSocket::disconnected,
			this, [this]() { backend_disconnected(); });
	}

private:
	bool ensure_backend()
	{
		if (socket_.state() == QLocalSocket::ConnectedState)
			return true;
		socket_.abort();
		socket_.connectToServer(socket_path_);
		return socket_.waitForConnected(150);
	}

	void read_stdin()
	{
		char buffer[8192];
		const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
		if (count <= 0) {
			stdin_notifier_.setEnabled(false);
			QCoreApplication::quit();
			return;
		}
		stdin_buffer_.append(buffer, count);
		while (true) {
			const int newline = stdin_buffer_.indexOf('\n');
			if (newline < 0)
				break;
			const QByteArray line = stdin_buffer_.left(newline).trimmed();
			stdin_buffer_.remove(0, newline + 1);
			if (!line.isEmpty())
				handle_client_message(line);
		}
	}

	void handle_client_message(const QByteArray& line)
	{
		QJsonParseError parse_error;
		const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
		if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
			write_message(error_response(QJsonValue(), -32700,
				QStringLiteral("Invalid JSON")));
			return;
		}

		const QJsonObject request = document.object();
		const QJsonValue id = request.value(QStringLiteral("id"));
		const QString method = request.value(QStringLiteral("method")).toString();
		if (method == QStringLiteral("initialize")) {
			QString version = request.value(QStringLiteral("params")).toObject()
				.value(QStringLiteral("protocolVersion")).toString();
			if (version.isEmpty())
				version = QStringLiteral("2025-06-18");
			const QJsonObject result{
				{QStringLiteral("protocolVersion"), version},
				{QStringLiteral("capabilities"), QJsonObject{
					{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
				{QStringLiteral("serverInfo"), QJsonObject{
					{QStringLiteral("name"), QStringLiteral("pulseview")},
					{QStringLiteral("version"), QStringLiteral("0.2")}}},
				{QStringLiteral("instructions"), pv::mcp::server_instructions()}};
			write_message(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
				{QStringLiteral("id"), id}, {QStringLiteral("result"), result}});
			return;
		}
		if (method == QStringLiteral("tools/list")) {
			write_message(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
				{QStringLiteral("id"), id}, {QStringLiteral("result"), QJsonObject{
					{QStringLiteral("tools"), pv::mcp::tool_catalog()}}}});
			return;
		}
		if (id.isUndefined())
			return;
		if (method != QStringLiteral("tools/call")) {
			write_message(error_response(id, -32601, QStringLiteral("Method not found")));
			return;
		}
		if (!ensure_backend()) {
			write_message(tool_error_response(id, QStringLiteral(
				"PulseView is not running. Start PulseView and retry this tool; "
				"the MCP connection will reconnect automatically.")));
			return;
		}

		pending_ids_.append(id);
		socket_.write(line + '\n');
		socket_.flush();
	}

	void read_backend()
	{
		backend_buffer_.append(socket_.readAll());
		while (true) {
			const int newline = backend_buffer_.indexOf('\n');
			if (newline < 0)
				break;
			const QByteArray line = backend_buffer_.left(newline).trimmed();
			backend_buffer_.remove(0, newline + 1);
			if (line.isEmpty())
				continue;
			const QJsonDocument response = QJsonDocument::fromJson(line);
			if (response.isObject())
				pending_ids_.removeOne(response.object().value(QStringLiteral("id")));
			fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
			fputc('\n', stdout);
			fflush(stdout);
		}
	}

	void backend_disconnected()
	{
		backend_buffer_.clear();
		for (const QJsonValue& id : pending_ids_)
			write_message(tool_error_response(id, QStringLiteral(
				"PulseView disconnected while handling the request. Restart it and retry.")));
		pending_ids_.clear();
	}

	QString socket_path_;
	QLocalSocket socket_;
	QSocketNotifier stdin_notifier_;
	QByteArray stdin_buffer_;
	QByteArray backend_buffer_;
	QList<QJsonValue> pending_ids_;
};

} // namespace

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	QString socket_path = default_socket_path();
	const QStringList arguments = application.arguments();
	if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--socket"))
		socket_path = arguments.at(2);
	else if (arguments.size() != 1) {
		fprintf(stderr, "Usage: pulseview-mcp [--socket PATH]\n");
		return 2;
	}

	Bridge bridge(socket_path);
	return application.exec();
}

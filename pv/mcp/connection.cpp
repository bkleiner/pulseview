/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "connection.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include "protocol.hpp"

namespace pv {
namespace mcp {

Connection::Connection(QLocalSocket *socket, const Protocol& protocol,
	QObject *parent) :
	QObject(parent),
	socket_(socket),
	protocol_(protocol)
{
	socket_->setParent(this);
	connect(socket_, SIGNAL(readyRead()), this, SLOT(read_messages()));
	connect(socket_, SIGNAL(disconnected()), this, SLOT(deleteLater()));
}

void Connection::read_messages()
{
	input_.append(socket_->readAll());
	if (input_.size() > MaxMessageSize) {
		reject(-32600, QStringLiteral("MCP message exceeds size limit"));
		return;
	}

	while (true) {
		const int newline = input_.indexOf('\n');
		if (newline < 0)
			return;

		QByteArray message = input_.left(newline).trimmed();
		input_.remove(0, newline + 1);
		if (message.isEmpty())
			continue;

		QJsonParseError parse_error;
		const QJsonDocument document = QJsonDocument::fromJson(message, &parse_error);
		if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
			reject(-32700, QStringLiteral("Invalid JSON: %1").arg(parse_error.errorString()));
			return;
		}

		QJsonObject response;
		if (protocol_.handle(document.object(), response))
			send(response);
	}
}

void Connection::reject(int code, const QString& message)
{
	QJsonObject error;
	error.insert(QStringLiteral("code"), code);
	error.insert(QStringLiteral("message"), message);

	QJsonObject response;
	response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
	response.insert(QStringLiteral("id"), QJsonValue());
	response.insert(QStringLiteral("error"), error);
	send(response);
	socket_->disconnectFromServer();
}

void Connection::send(const QJsonObject& response)
{
	QByteArray output = QJsonDocument(response).toJson(QJsonDocument::Compact);
	output.append('\n');
	socket_->write(output);
}

} // namespace mcp
} // namespace pv

/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PULSEVIEW_PV_MCP_CONNECTION_HPP
#define PULSEVIEW_PV_MCP_CONNECTION_HPP

#include <QObject>

class QLocalSocket;

namespace pv {
namespace mcp {

class Protocol;

class Connection : public QObject
{
	Q_OBJECT

public:
	Connection(QLocalSocket *socket, const Protocol& protocol,
		QObject *parent = nullptr);

private Q_SLOTS:
	void read_messages();

private:
	static const qint64 MaxMessageSize = 1024 * 1024;

	void reject(int code, const QString& message);
	void send(const QJsonObject& response);

	QLocalSocket *socket_;
	const Protocol& protocol_;
	QByteArray input_;
};

} // namespace mcp
} // namespace pv

#endif // PULSEVIEW_PV_MCP_CONNECTION_HPP

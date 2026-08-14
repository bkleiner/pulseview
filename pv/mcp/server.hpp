/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PULSEVIEW_PV_MCP_SERVER_HPP
#define PULSEVIEW_PV_MCP_SERVER_HPP

#include <memory>

#include <QObject>
#include <QString>

class QLocalServer;

namespace pv {
namespace mcp {

class Protocol;
class SessionRegistry;
class Tools;

class Server : public QObject
{
	Q_OBJECT

public:
	explicit Server(SessionRegistry& sessions, QObject *parent = nullptr);
	~Server();

	bool listen(const QString& socket_path = QString());
	QString socket_path() const;
	QString error_string() const;

	static QString default_socket_path();

private Q_SLOTS:
	void accept_connections();

private:
	QLocalServer *server_;
	std::unique_ptr<Tools> tools_;
	std::unique_ptr<Protocol> protocol_;
	QString socket_path_;
	QString error_string_;
};

} // namespace mcp
} // namespace pv

#endif // PULSEVIEW_PV_MCP_SERVER_HPP

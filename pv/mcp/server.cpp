/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "server.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "connection.hpp"
#include "protocol.hpp"
#include "tools.hpp"

namespace pv {
namespace mcp {

Server::Server(SessionRegistry& sessions, QObject *parent) :
	QObject(parent),
	server_(new QLocalServer(this)),
	tools_(new Tools(sessions)),
	protocol_(new Protocol(*tools_))
{
	connect(server_, SIGNAL(newConnection()), this, SLOT(accept_connections()));
}

Server::~Server()
{
	server_->close();
	if (!socket_path_.isEmpty())
		QLocalServer::removeServer(socket_path_);
}

bool Server::listen(const QString& requested_path)
{
	socket_path_ = requested_path.isEmpty() ? default_socket_path() : requested_path;
	error_string_.clear();

	const QFileInfo socket_info(socket_path_);
	if (!QDir().mkpath(socket_info.absolutePath())) {
		error_string_ = QStringLiteral("Cannot create MCP socket directory: %1")
			.arg(socket_info.absolutePath());
		return false;
	}

	if (QFileInfo::exists(socket_path_)) {
		QLocalSocket probe;
		probe.connectToServer(socket_path_);
		if (probe.waitForConnected(100)) {
			error_string_ = QStringLiteral("Another PulseView MCP server is already listening at %1")
				.arg(socket_path_);
			return false;
		}

		if (!QLocalServer::removeServer(socket_path_)) {
			error_string_ = QStringLiteral("Cannot remove stale MCP socket: %1")
				.arg(socket_path_);
			return false;
		}
	}

	if (!server_->listen(socket_path_)) {
		error_string_ = server_->errorString();
		return false;
	}

#ifdef Q_OS_UNIX
	const QByteArray native_path = QFile::encodeName(socket_path_);
	if (::chmod(native_path.constData(), S_IRUSR | S_IWUSR) != 0) {
		error_string_ = QStringLiteral("Cannot restrict MCP socket permissions: %1")
			.arg(socket_path_);
		server_->close();
		QLocalServer::removeServer(socket_path_);
		return false;
	}
#endif

	return true;
}

QString Server::socket_path() const
{
	return socket_path_;
}

QString Server::error_string() const
{
	return error_string_;
}

QString Server::default_socket_path()
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

void Server::accept_connections()
{
	while (server_->hasPendingConnections())
		new Connection(server_->nextPendingConnection(), *protocol_, this);
}

} // namespace mcp
} // namespace pv

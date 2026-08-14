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
#include <QElapsedTimer>
#include <QLockFile>
#include <QLocalSocket>
#include <QProcess>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>

#include <unistd.h>

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

bool connect_to_pulseview(QLocalSocket& socket, const QString& socket_path,
	int timeout_ms)
{
	QElapsedTimer timer;
	timer.start();

	do {
		socket.abort();
		socket.connectToServer(socket_path);
		if (socket.waitForConnected(250))
			return true;
		QThread::msleep(100);
	} while (timer.elapsed() < timeout_ms);

	return false;
}

bool launch_pulseview(const QString& socket_path)
{
	QString runtime_directory = QDir(QStandardPaths::writableLocation(
		QStandardPaths::TempLocation)).filePath(
		QStringLiteral("pulseview-mcp-%1").arg(::getuid()));
	if (!QDir().mkpath(runtime_directory))
		return false;

	QProcess process;
	process.setProgram(QStringLiteral("pulseview"));
	process.setArguments({QStringLiteral("--mcp-socket"), socket_path});
	process.setWorkingDirectory(runtime_directory);
	process.setStandardOutputFile(QProcess::nullDevice());
	process.setStandardErrorFile(QProcess::nullDevice());
	return process.startDetached();
}

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

	QLocalSocket socket;
	if (!connect_to_pulseview(socket, socket_path, 500)) {
		const QString lock_path = socket_path + QStringLiteral(".launch.lock");
		QLockFile launch_lock(lock_path);
		launch_lock.setStaleLockTime(10000);

		if (launch_lock.tryLock(500)) {
			// Another bridge may have launched PulseView while this process
			// waited for the launch lock.
			if (!connect_to_pulseview(socket, socket_path, 500) &&
				!launch_pulseview(socket_path)) {
				fprintf(stderr, "pulseview-mcp: failed to launch pulseview\n");
				return 1;
			}
			launch_lock.unlock();
		}

		if (!connect_to_pulseview(socket, socket_path, 8000)) {
		fprintf(stderr, "pulseview-mcp: cannot connect to %s: %s\n",
			socket_path.toLocal8Bit().constData(),
			socket.errorString().toLocal8Bit().constData());
		return 1;
		}
	}

	QSocketNotifier stdin_notifier(STDIN_FILENO, QSocketNotifier::Read);
	QObject::connect(&stdin_notifier, &QSocketNotifier::activated,
		[&socket, &stdin_notifier](int) {
			char buffer[8192];
			const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
			if (count > 0) {
				socket.write(buffer, count);
				socket.flush();
			} else
				stdin_notifier.setEnabled(false);
		});

	QObject::connect(&socket, &QLocalSocket::readyRead, [&socket]() {
		const QByteArray output = socket.readAll();
		fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
		fflush(stdout);
	});
	QObject::connect(&socket, &QLocalSocket::disconnected,
		&application, &QCoreApplication::quit);

	return application.exec();
}

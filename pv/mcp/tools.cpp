/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tools.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QThread>

#include <glibmm/variant.h>
#include <libsigrokcxx/libsigrokcxx.hpp>

#ifdef ENABLE_DECODE
#include "pv/data/decodesignal.hpp"
#endif
#include "pv/devices/device.hpp"
#include "pv/devicemanager.hpp"
#include "pv/session.hpp"
#include "pv/storesession.hpp"
#include "pv/toolbars/mainbar.hpp"
#include "pv/data/logic.hpp"
#include "pv/data/logicsegment.hpp"
#include "pv/data/signalbase.hpp"
#include "pv/views/trace/cursor.hpp"
#include "pv/views/trace/cursorpair.hpp"
#include "pv/views/trace/logicsignal.hpp"
#include "pv/views/trace/signal.hpp"
#include "pv/views/trace/view.hpp"
#include "pv/views/trace/viewport.hpp"
#include "catalog.hpp"
#include "sessionregistry.hpp"

namespace pv {
namespace mcp {

namespace {

QString capture_state_name(Session::capture_state state)
{
	switch (state) {
	case Session::Stopped:
		return QStringLiteral("stopped");
	case Session::AwaitingTrigger:
		return QStringLiteral("awaiting_trigger");
	case Session::Running:
		return QStringLiteral("running");
	}

	return QStringLiteral("unknown");
}

bool resolve_session(const SessionRegistry& sessions, const QJsonObject& arguments,
	SessionRegistry::Entry& entry, QString& error)
{
	if (arguments.contains(QStringLiteral("session_id")) &&
		!arguments.value(QStringLiteral("session_id")).isString()) {
		error = QStringLiteral("session_id must be a string");
		return false;
	}
	const QString requested_id = arguments.value(QStringLiteral("session_id")).toString();
	if (requested_id.isEmpty()) {
		if (!sessions.active(entry)) {
			error = QStringLiteral("No active PulseView session");
			return false;
		}
		return true;
	}

	bool valid = false;
	const uint64_t id = requested_id.toULongLong(&valid);
	if (!valid || !sessions.resolve(id, entry)) {
		error = QStringLiteral("Unknown or stale session ID: %1").arg(requested_id);
		return false;
	}
	return true;
}

std::shared_ptr<views::trace::View> trace_view(
	const SessionRegistry::Entry& entry, QString& error)
{
	const auto view = std::dynamic_pointer_cast<views::trace::View>(
		entry.session->main_view());
	if (!view)
		error = QStringLiteral("Session has no main trace view");
	return view;
}

QJsonObject view_context(const SessionRegistry::Entry& entry,
	const std::shared_ptr<views::trace::View>& view)
{
	QJsonObject result;
	const double samplerate = entry.session->get_samplerate();
	const double visible_start_seconds = view->offset().convert_to<double>();
	const double visible_end_seconds = visible_start_seconds +
		(view->viewport()->width() * view->scale());
	const auto extents = view->get_time_extents();

	result.insert(QStringLiteral("session_id"), QString::number(entry.id));
	result.insert(QStringLiteral("view_id"), QStringLiteral("%1:main").arg(entry.id));
	result.insert(QStringLiteral("generation"), QString::number(entry.generation));
	result.insert(QStringLiteral("segment_id"), static_cast<int>(view->current_segment()));
	result.insert(QStringLiteral("samplerate"), samplerate);
	result.insert(QStringLiteral("seconds_per_pixel"), view->scale());
	result.insert(QStringLiteral("visible_start_seconds"), visible_start_seconds);
	result.insert(QStringLiteral("visible_end_seconds"), visible_end_seconds);
	result.insert(QStringLiteral("visible_start_sample"), QString::number(
		static_cast<int64_t>(visible_start_seconds * samplerate)));
	result.insert(QStringLiteral("visible_end_sample"), QString::number(
		static_cast<int64_t>(visible_end_seconds * samplerate)));
	result.insert(QStringLiteral("extent_start_seconds"), extents.first.convert_to<double>());
	result.insert(QStringLiteral("extent_end_seconds"), extents.second.convert_to<double>());
	result.insert(QStringLiteral("cursors_visible"), view->cursors_shown());

	if (view->cursors_shown()) {
		const auto cursors = view->cursors();
		const double first = cursors->first()->time().convert_to<double>();
		const double second = cursors->second()->time().convert_to<double>();
		QJsonObject cursor_range;
		cursor_range.insert(QStringLiteral("start_seconds"), std::min(first, second));
		cursor_range.insert(QStringLiteral("end_seconds"), std::max(first, second));
		cursor_range.insert(QStringLiteral("start_sample"), QString::number(
			static_cast<int64_t>(std::min(first, second) * samplerate)));
		cursor_range.insert(QStringLiteral("end_sample"), QString::number(
			static_cast<int64_t>(std::max(first, second) * samplerate)));
		result.insert(QStringLiteral("cursor_range"), cursor_range);
	}
	return result;
}

bool sample_value(const QJsonObject& arguments, const QString& name,
	uint64_t& value, QString& error)
{
	bool valid = false;
	value = arguments.value(name).toString().toULongLong(&valid);
	if (!valid) {
		error = QStringLiteral("%1 must be an unsigned integer string").arg(name);
		return false;
	}
	return true;
}

bool require_generation(const SessionRegistry::Entry& entry,
	const QJsonObject& arguments, QString& error)
{
	if (arguments.value(QStringLiteral("generation")).toString() !=
		QString::number(entry.generation)) {
		error = QStringLiteral("Stale generation; current generation is %1")
			.arg(entry.generation);
		return false;
	}
	return true;
}

const sigrok::TriggerMatchType* trigger_type(const QString& name, bool& valid)
{
	valid = true;
	const QString value = name.toLower();
	if (value.isEmpty() || value == QStringLiteral("none"))
		return nullptr;
	if (value == QStringLiteral("low") || value == QStringLiteral("zero"))
		return sigrok::TriggerMatchType::ZERO;
	if (value == QStringLiteral("high") || value == QStringLiteral("one"))
		return sigrok::TriggerMatchType::ONE;
	if (value == QStringLiteral("rising"))
		return sigrok::TriggerMatchType::RISING;
	if (value == QStringLiteral("falling"))
		return sigrok::TriggerMatchType::FALLING;
	if (value == QStringLiteral("edge") || value == QStringLiteral("change"))
		return sigrok::TriggerMatchType::EDGE;
	valid = false;
	return nullptr;
}

QString trigger_name(const sigrok::TriggerMatchType *type)
{
	if (!type)
		return QStringLiteral("none");
	switch (type->id()) {
	case SR_TRIGGER_ZERO: return QStringLiteral("low");
	case SR_TRIGGER_ONE: return QStringLiteral("high");
	case SR_TRIGGER_RISING: return QStringLiteral("rising");
	case SR_TRIGGER_FALLING: return QStringLiteral("falling");
	case SR_TRIGGER_EDGE: return QStringLiteral("edge");
	default: return QStringLiteral("unknown");
	}
}

#ifdef ENABLE_DECODE
struct AnnotationEvent {
	uint32_t signal_index;
	QString signal_name;
	data::AnnotationSnapshot annotation;
};
#endif

} // namespace

Tools::Tools(SessionRegistry& sessions) :
	sessions_(sessions)
{
}

QJsonArray Tools::list_tools() const
{
	return tool_catalog();
}

bool Tools::call_tool(const QString& name, const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	QJsonObject structured;
	if (name == QStringLiteral("get_session")) {
		if (!get_session(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("get_capture")) {
		if (!get_capture(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("start_capture")) {
		if (!start_capture(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("set_session")) {
		if (!set_session(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("save_session")) {
		if (!save_session(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("load_session")) {
		if (!load_session(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("query_annotations")) {
		if (!query_annotations(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("set_cursors")) {
		if (!set_cursors(arguments, structured, error))
			return false;
	} else if (name == QStringLiteral("zoom_to_range")) {
		if (!zoom_to_range(arguments, structured, error))
			return false;
	} else {
		error = QStringLiteral("Unknown tool: %1").arg(name);
		return false;
	}
	const QString text = QString::fromUtf8(
		QJsonDocument(structured).toJson(QJsonDocument::Compact));

	QJsonObject content_item;
	content_item.insert(QStringLiteral("type"), QStringLiteral("text"));
	content_item.insert(QStringLiteral("text"), text);

	result.insert(QStringLiteral("content"), QJsonArray{content_item});
	result.insert(QStringLiteral("structuredContent"), structured);
	result.insert(QStringLiteral("isError"), false);
	return true;
}

bool Tools::get_session(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("all_sessions")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("get_session does not accept %1").arg(it.key());
			return false;
		}
	if (arguments.contains(QStringLiteral("all_sessions")) &&
		!arguments.value(QStringLiteral("all_sessions")).isBool()) {
		error = QStringLiteral("all_sessions must be a boolean");
		return false;
	}

	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error))
		return false;
	const std::shared_ptr<Session>& session = entry.session;
	result.insert(QStringLiteral("session_id"), QString::number(entry.id));
	result.insert(QStringLiteral("generation"), QString::number(entry.generation));
	result.insert(QStringLiteral("name"), session->name());
	result.insert(QStringLiteral("active"), entry.active);
	result.insert(QStringLiteral("capture_state"),
		capture_state_name(session->get_capture_state()));
	result.insert(QStringLiteral("samplerate"), session->get_samplerate());
	result.insert(QStringLiteral("data_saved"), session->data_saved());
	if (!session->save_path().isEmpty())
		result.insert(QStringLiteral("file_path"), session->save_path());

	const std::shared_ptr<devices::Device> device = session->device();
	if (device) {
		result.insert(QStringLiteral("device_name"),
			QString::fromStdString(device->full_name()));
		if (const std::shared_ptr<sigrok::Device> sr_device = device->device()) {
			QJsonObject config;
			auto read_config = [&](const QString& name, const sigrok::ConfigKey *key) {
				try {
					if (sr_device->config_check(key, sigrok::Capability::GET))
						config.insert(name, QString::number(device->read_config<uint64_t>(key)));
				} catch (const sigrok::Error&) {
				}
			};
			read_config(QStringLiteral("samplerate"), sigrok::ConfigKey::SAMPLERATE);
			read_config(QStringLiteral("num_samples"), sigrok::ConfigKey::LIMIT_SAMPLES);
			read_config(QStringLiteral("capture_ratio"), sigrok::ConfigKey::CAPTURE_RATIO);
			result.insert(QStringLiteral("config"), config);
		}
	}

	QJsonArray segments;
	const uint32_t highest_segment = session->get_highest_segment_id();
	for (uint32_t id = 0; id <= highest_segment; id++)
		segments.append(QJsonObject{
			{QStringLiteral("segment_id"), static_cast<int>(id)},
			{QStringLiteral("sample_count"),
				QString::number(session->get_segment_sample_count(id))}});
	result.insert(QStringLiteral("segments"), segments);

	std::shared_ptr<sigrok::Trigger> trigger;
	if (session->session())
		trigger = session->session()->trigger();
	QJsonArray channels;
	QJsonArray decode_signals;
#ifdef ENABLE_DECODE
	uint32_t decode_index = 0;
#endif
	for (const auto& base : session->signalbases()) {
		if (base->is_decode_signal()) {
#ifdef ENABLE_DECODE
			const auto signal = std::dynamic_pointer_cast<data::DecodeSignal>(base);
			QJsonArray decoders;
			for (const auto& decoder : signal->decoder_stack()) {
				const srd_decoder* srd = decoder->get_srd_decoder();
				decoders.append(QJsonObject{
					{QStringLiteral("id"), QString::fromUtf8(srd->id)},
					{QStringLiteral("name"), QString::fromUtf8(srd->name)},
					{QStringLiteral("stack_level"), decoder->get_stack_level()}});
			}
			decode_signals.append(QJsonObject{
				{QStringLiteral("decode_signal_id"), QString::number(decode_index++)},
				{QStringLiteral("name"), signal->display_name()},
				{QStringLiteral("decoders"), decoders}});
#endif
			continue;
		}

		QJsonObject channel{
			{QStringLiteral("name"), base->name()},
			{QStringLiteral("internal_name"), base->internal_name()},
			{QStringLiteral("enabled"), base->enabled()},
			{QStringLiteral("color"), base->color().name()},
			{QStringLiteral("index"), static_cast<int>(base->index())}};
		const bool logic = base->type() == data::SignalBase::LogicChannel;
		channel.insert(QStringLiteral("type"), logic ? QStringLiteral("logic") :
			base->type() == data::SignalBase::AnalogChannel ? QStringLiteral("analog") :
			QStringLiteral("other"));
		if (logic) {
			channel.insert(QStringLiteral("bit"), static_cast<int>(base->logic_bit_index()));
			const sigrok::TriggerMatchType *match_type = nullptr;
			if (trigger)
				for (const auto& stage : trigger->stages())
					for (const auto& match : stage->matches())
						if (match->channel() == base->channel())
							match_type = match->type();
			channel.insert(QStringLiteral("trigger"), trigger_name(match_type));
		}
		channels.append(channel);
	}
	result.insert(QStringLiteral("channels"), channels);
	result.insert(QStringLiteral("decode_signals"), decode_signals);

	QString view_error;
	if (const auto view = trace_view(entry, view_error))
		result.insert(QStringLiteral("view"), view_context(entry, view));

	if (arguments.value(QStringLiteral("all_sessions")).toBool(false)) {
		QJsonArray open_sessions;
		for (const SessionRegistry::Entry& other : sessions_.entries())
			open_sessions.append(QJsonObject{
				{QStringLiteral("session_id"), QString::number(other.id)},
				{QStringLiteral("generation"), QString::number(other.generation)},
				{QStringLiteral("name"), other.session->name()},
				{QStringLiteral("active"), other.active},
				{QStringLiteral("capture_state"),
					capture_state_name(other.session->get_capture_state())}});
		result.insert(QStringLiteral("open_sessions"), open_sessions);
	}
	return true;
}

bool Tools::get_capture(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("segment_id"),
		QStringLiteral("start_sample"), QStringLiteral("end_sample"),
		QStringLiteral("channels"), QStringLiteral("mode"), QStringLiteral("limit")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("get_capture does not accept %1").arg(it.key());
			return false;
		}

	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error))
		return false;
	const auto view = trace_view(entry, error);
	if (!view)
		return false;
	const uint32_t segment_id = arguments.contains(QStringLiteral("segment_id")) ?
		static_cast<uint32_t>(arguments.value(QStringLiteral("segment_id")).toInt(-1)) :
		view->current_segment();
	if (segment_id > entry.session->get_highest_segment_id()) {
		error = QStringLiteral("Unknown segment ID: %1").arg(segment_id);
		return false;
	}

	const uint64_t sample_count = entry.session->get_segment_sample_count(segment_id);
	uint64_t start = 0;
	uint64_t end = sample_count;
	const bool has_start = arguments.contains(QStringLiteral("start_sample"));
	const bool has_end = arguments.contains(QStringLiteral("end_sample"));
	if (has_start != has_end) {
		error = QStringLiteral("start_sample and end_sample must be supplied together");
		return false;
	}
	if (has_start && (!sample_value(arguments, QStringLiteral("start_sample"), start, error) ||
		!sample_value(arguments, QStringLiteral("end_sample"), end, error)))
		return false;
	if (start > end || end > sample_count) {
		error = QStringLiteral("Capture range %1..%2 is outside segment 0..%3")
			.arg(start).arg(end).arg(sample_count);
		return false;
	}

	const QString mode = arguments.value(QStringLiteral("mode")).toString(QStringLiteral("edges"));
	if (mode != QStringLiteral("edges") && mode != QStringLiteral("bits")) {
		error = QStringLiteral("mode must be edges or bits");
		return false;
	}
	const int default_limit = mode == QStringLiteral("bits") ? 1000000 : 10000;
	const int limit = arguments.value(QStringLiteral("limit")).toInt(default_limit);
	if (limit < 1 || limit > 1000000) {
		error = QStringLiteral("limit must be between 1 and 1000000");
		return false;
	}
	if (mode == QStringLiteral("bits") && end - start > static_cast<uint64_t>(limit)) {
		error = QStringLiteral("Requested %1 samples; limit is %2")
			.arg(end - start).arg(limit);
		return false;
	}

	QSet<QString> wanted;
	if (arguments.contains(QStringLiteral("channels"))) {
		if (!arguments.value(QStringLiteral("channels")).isArray()) {
			error = QStringLiteral("channels must be an array of names");
			return false;
		}
		for (const QJsonValue& value : arguments.value(QStringLiteral("channels")).toArray())
			wanted.insert(value.toString());
	}

	QJsonArray channels;
	std::map<const data::LogicSegment*, QByteArray> raw_cache;
	for (const auto& base : entry.session->signalbases()) {
		if (base->type() != data::SignalBase::LogicChannel || !base->enabled() ||
			(!wanted.isEmpty() && !wanted.contains(base->name())))
			continue;
		const auto logic = base->logic_data();
		if (!logic || segment_id >= logic->logic_segments().size())
			continue;
		const auto segment = logic->logic_segments().at(segment_id);
		QJsonObject channel{
			{QStringLiteral("name"), base->name()},
			{QStringLiteral("index"), static_cast<int>(base->index())},
			{QStringLiteral("bit"), static_cast<int>(base->logic_bit_index())}};

		if (start == end) {
			if (mode == QStringLiteral("edges")) {
				channel.insert(QStringLiteral("initial"), false);
				channel.insert(QStringLiteral("edges"), QJsonArray());
			} else
				channel.insert(QStringLiteral("bits_base64"), QString());
			channels.append(channel);
			continue;
		}

		if (mode == QStringLiteral("edges")) {
			QByteArray initial_sample(static_cast<int>(segment->unit_size()), '\0');
			segment->get_samples(static_cast<int64_t>(start),
				static_cast<int64_t>(start + 1),
				reinterpret_cast<uint8_t*>(initial_sample.data()));
			const unsigned int bit = base->logic_bit_index();
			const bool initial = (static_cast<uint8_t>(initial_sample.at(bit / 8)) &
				static_cast<uint8_t>(1U << (bit % 8))) != 0;
			channel.insert(QStringLiteral("initial"), initial);
			QJsonArray edges;
			uint64_t position = start;
			bool truncated = false;
			while (position < end) {
				std::vector<data::LogicSegment::EdgePair> next;
				segment->get_subsampled_edges(next, position, end, 1.0f,
					static_cast<int>(bit), true);
				if (next.empty() || next.front().first < 0 ||
					static_cast<uint64_t>(next.front().first) >= end)
					break;
				const uint64_t edge_sample = static_cast<uint64_t>(next.front().first);
				if (edge_sample <= position) {
					position++;
					continue;
				}
				if (edges.size() >= limit) {
					truncated = true;
					break;
				}
				edges.append(QJsonArray{QString::number(edge_sample), next.front().second});
				position = edge_sample;
			}
			channel.insert(QStringLiteral("edges"), edges);
			channel.insert(QStringLiteral("truncated"), truncated);
		} else {
			QByteArray& raw = raw_cache[segment.get()];
			if (raw.isEmpty()) {
				raw.resize(static_cast<int>((end - start) * segment->unit_size()));
				segment->get_samples(static_cast<int64_t>(start), static_cast<int64_t>(end),
					reinterpret_cast<uint8_t*>(raw.data()));
			}
			QByteArray packed(static_cast<int>(((end - start) + 7) / 8), '\0');
			const unsigned int bit = base->logic_bit_index();
			for (uint64_t index = 0; index < end - start; index++)
				if ((static_cast<uint8_t>(raw.at(static_cast<int>(
					index * segment->unit_size() + bit / 8))) & (1U << (bit % 8))) != 0)
					packed[static_cast<int>(index / 8)] = static_cast<char>(
						static_cast<uint8_t>(packed.at(static_cast<int>(index / 8))) |
						(1U << (index % 8)));
			channel.insert(QStringLiteral("bits_base64"), QString::fromLatin1(packed.toBase64()));
		}
		channels.append(channel);
	}

	result.insert(QStringLiteral("session_id"), QString::number(entry.id));
	result.insert(QStringLiteral("generation"), QString::number(entry.generation));
	result.insert(QStringLiteral("segment_id"), static_cast<int>(segment_id));
	result.insert(QStringLiteral("samplerate"), entry.session->get_samplerate());
	result.insert(QStringLiteral("start_sample"), QString::number(start));
	result.insert(QStringLiteral("end_sample"), QString::number(end));
	result.insert(QStringLiteral("mode"), mode);
	result.insert(QStringLiteral("channels"), channels);
	return true;
}

bool Tools::start_capture(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("generation"),
		QStringLiteral("samplerate"), QStringLiteral("num_samples"),
		QStringLiteral("wait"), QStringLiteral("timeout_ms")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("start_capture does not accept %1").arg(it.key());
			return false;
		}
	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error) ||
		!require_generation(entry, arguments, error))
		return false;
	if (entry.session->get_capture_state() != Session::Stopped) {
		error = QStringLiteral("A capture is already running");
		return false;
	}
	const auto device = entry.session->device();
	const auto sr_device = device ? device->device() : nullptr;
	if (!sr_device) {
		error = QStringLiteral("The active session has no device");
		return false;
	}
	try {
		for (const auto& setting : std::vector<std::pair<QString, const sigrok::ConfigKey*>>{
			{QStringLiteral("samplerate"), sigrok::ConfigKey::SAMPLERATE},
			{QStringLiteral("num_samples"), sigrok::ConfigKey::LIMIT_SAMPLES}}) {
			if (!arguments.contains(setting.first))
				continue;
			uint64_t value;
			if (!sample_value(arguments, setting.first, value, error))
				return false;
			if (!sr_device->config_check(setting.second, sigrok::Capability::SET)) {
				error = QStringLiteral("Device does not support setting %1").arg(setting.first);
				return false;
			}
			sr_device->config_set(setting.second, Glib::Variant<guint64>::create(value));
		}
	} catch (const sigrok::Error& exception) {
		error = QString::fromUtf8(exception.what());
		return false;
	}
	if (entry.session->main_bar())
		entry.session->main_bar()->refresh_config_selectors();

	const auto capture_error_mutex = std::make_shared<std::mutex>();
	const auto capture_error = std::make_shared<QString>();
	entry.session->start_capture([capture_error_mutex, capture_error](const QString message) {
		std::lock_guard<std::mutex> lock(*capture_error_mutex);
		*capture_error = message;
	});
	if (!arguments.value(QStringLiteral("wait")).toBool(true)) {
		result.insert(QStringLiteral("state"), QStringLiteral("starting"));
		return true;
	}

	const int timeout_ms = arguments.value(QStringLiteral("timeout_ms")).toInt(30000);
	QElapsedTimer timer;
	timer.start();
	bool observed_running = false;
	while (timer.elapsed() < timeout_ms) {
		QCoreApplication::processEvents(QEventLoop::ExcludeSocketNotifiers, 10);
		{
			std::lock_guard<std::mutex> lock(*capture_error_mutex);
			if (!capture_error->isEmpty()) {
				error = *capture_error;
				return false;
			}
		}
		const Session::capture_state state = entry.session->get_capture_state();
		observed_running |= state != Session::Stopped;
		const uint32_t latest_segment = entry.session->get_highest_segment_id();
		if (state == Session::Stopped && (observed_running ||
			entry.session->get_segment_sample_count(latest_segment) > 0))
			break;
		QThread::msleep(1);
	}
	if (entry.session->get_capture_state() != Session::Stopped ||
		entry.session->get_segment_sample_count(entry.session->get_highest_segment_id()) == 0) {
		error = QStringLiteral("Capture did not finish within timeout_ms");
		return false;
	}
	result.insert(QStringLiteral("state"), QStringLiteral("stopped"));
	result.insert(QStringLiteral("segment_id"),
		static_cast<int>(entry.session->get_highest_segment_id()));
	result.insert(QStringLiteral("sample_count"), QString::number(
		entry.session->get_segment_sample_count(entry.session->get_highest_segment_id())));
	return true;
}

bool Tools::set_session(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("generation"),
		QStringLiteral("samplerate"), QStringLiteral("num_samples"),
		QStringLiteral("capture_ratio"), QStringLiteral("channels")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("set_session does not accept %1").arg(it.key());
			return false;
		}
	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error) ||
		!require_generation(entry, arguments, error))
		return false;
	if (entry.session->get_capture_state() != Session::Stopped) {
		error = QStringLiteral("Cannot configure a running capture");
		return false;
	}

	QJsonObject applied;
	QJsonArray warnings;
	bool changed = false;
	const auto device = entry.session->device();
	const auto sr_device = device ? device->device() : nullptr;
	for (const auto& setting : std::vector<std::pair<QString, const sigrok::ConfigKey*>>{
		{QStringLiteral("samplerate"), sigrok::ConfigKey::SAMPLERATE},
		{QStringLiteral("num_samples"), sigrok::ConfigKey::LIMIT_SAMPLES},
		{QStringLiteral("capture_ratio"), sigrok::ConfigKey::CAPTURE_RATIO}}) {
		if (!arguments.contains(setting.first))
			continue;
		uint64_t value = 0;
		if (setting.first == QStringLiteral("capture_ratio")) {
			const int ratio = arguments.value(setting.first).toInt(-1);
			if (ratio < 0 || ratio > 100) {
				error = QStringLiteral("capture_ratio must be between 0 and 100");
				return false;
			}
			value = static_cast<uint64_t>(ratio);
		}
		else if (!sample_value(arguments, setting.first, value, error))
			return false;
		try {
			if (!sr_device ||
				!sr_device->config_check(setting.second, sigrok::Capability::SET)) {
				warnings.append(QStringLiteral("%1 is not supported").arg(setting.first));
				continue;
			}
			sr_device->config_set(setting.second, Glib::Variant<guint64>::create(value));
			applied.insert(setting.first, QString::number(value));
			changed = true;
		} catch (const sigrok::Error& exception) {
			warnings.append(QStringLiteral("%1: %2").arg(setting.first,
				QString::fromUtf8(exception.what())));
		}
	}
	if (entry.session->main_bar())
		entry.session->main_bar()->refresh_config_selectors();

	QJsonArray channel_results;
	if (arguments.contains(QStringLiteral("channels"))) {
		if (!arguments.value(QStringLiteral("channels")).isArray()) {
			error = QStringLiteral("channels must be an array");
			return false;
		}
		const auto view = std::dynamic_pointer_cast<views::trace::View>(entry.session->main_view());
		for (const QJsonValue& value : arguments.value(QStringLiteral("channels")).toArray()) {
			const QJsonObject update = value.toObject();
			const QString name = update.value(QStringLiteral("name")).toString();
			QJsonObject channel_result{{QStringLiteral("name"), name}};
			std::shared_ptr<data::SignalBase> base;
			for (const auto& candidate : entry.session->signalbases())
				if (candidate->name() == name || candidate->internal_name() == name) {
					base = candidate;
					break;
				}
			if (!base) {
				channel_result.insert(QStringLiteral("error"), QStringLiteral("not_found"));
				channel_results.append(channel_result);
				continue;
			}
			if (update.contains(QStringLiteral("label"))) {
				base->set_name(update.value(QStringLiteral("label")).toString());
				channel_result.insert(QStringLiteral("label"), base->name());
				changed = true;
			}
			if (update.contains(QStringLiteral("color"))) {
				const QColor color(update.value(QStringLiteral("color")).toString());
				if (color.isValid()) {
					base->set_color(color);
					channel_result.insert(QStringLiteral("color"), color.name());
					changed = true;
				} else
					channel_result.insert(QStringLiteral("color_error"), QStringLiteral("invalid_color"));
			}
			if (update.contains(QStringLiteral("trigger"))) {
				bool valid = false;
				const sigrok::TriggerMatchType *type = trigger_type(
					update.value(QStringLiteral("trigger")).toString(), valid);
				std::shared_ptr<views::trace::LogicSignal> logic_signal;
				if (view)
					for (const auto& signal : view->list_by_type<views::trace::LogicSignal>())
						if (signal->base() == base)
							logic_signal = signal;
				if (!valid)
					channel_result.insert(QStringLiteral("trigger_error"), QStringLiteral("unknown_type"));
				else if (!logic_signal)
					channel_result.insert(QStringLiteral("trigger_error"), QStringLiteral("not_logic"));
				else if (!logic_signal->set_trigger_match(type))
					channel_result.insert(QStringLiteral("trigger_error"), QStringLiteral("unsupported"));
				else
					channel_result.insert(QStringLiteral("trigger"), trigger_name(type));
				if (valid && logic_signal && !channel_result.contains(
					QStringLiteral("trigger_error")))
					changed = true;
			}
			channel_results.append(channel_result);
		}
	}

	if (changed)
		sessions_.changed(entry.session.get());
	SessionRegistry::Entry updated;
	sessions_.resolve(entry.id, updated);
	result.insert(QStringLiteral("generation"), QString::number(updated.generation));
	result.insert(QStringLiteral("applied"), applied);
	result.insert(QStringLiteral("channels"), channel_results);
	if (!warnings.isEmpty())
		result.insert(QStringLiteral("warnings"), warnings);
	return true;
}

bool Tools::save_session(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("generation"),
		QStringLiteral("path"), QStringLiteral("overwrite")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("save_session does not accept %1").arg(it.key());
			return false;
		}
	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error) ||
		!require_generation(entry, arguments, error))
		return false;
	if (entry.session->get_capture_state() != Session::Stopped) {
		error = QStringLiteral("Cannot save while capture is running");
		return false;
	}
	const QString requested_path = arguments.value(QStringLiteral("path")).toString();
	if (requested_path.isEmpty() || !QFileInfo(requested_path).isAbsolute()) {
		error = QStringLiteral("path must be absolute");
		return false;
	}
	const QString path = QFileInfo(requested_path).absoluteFilePath();
	if (QFileInfo::exists(path) && !arguments.value(QStringLiteral("overwrite")).toBool(false)) {
		error = QStringLiteral("Refusing to overwrite existing file: %1").arg(path);
		return false;
	}
	std::shared_ptr<sigrok::OutputFormat> format;
	try {
		format = entry.session->device_manager().context()->output_formats().at("srzip");
	} catch (...) {
		error = QStringLiteral("Native srzip output is unavailable");
		return false;
	}
	StoreSession store(path.toStdString(), format,
		std::map<std::string, Glib::VariantBase>(), std::make_pair(0ULL, 0ULL),
		*entry.session);
	std::atomic<bool> completed(false);
	QObject::connect(&store, &StoreSession::store_successful,
		&store, [&completed]() { completed = true; }, Qt::DirectConnection);
	if (!store.start()) {
		error = store.error();
		return false;
	}
	while (!completed.load()) {
		QCoreApplication::processEvents(QEventLoop::ExcludeSocketNotifiers, 10);
		QThread::msleep(1);
	}
	store.wait();
	if (!store.error().isEmpty()) {
		error = store.error();
		return false;
	}
	entry.session->set_save_path(QFileInfo(path).absolutePath());
	entry.session->set_name(QFileInfo(path).fileName());
	QMetaObject::invokeMethod(entry.session.get(), "on_data_saved", Qt::DirectConnection);
	SessionRegistry::Entry updated;
	sessions_.resolve(entry.id, updated);
	result.insert(QStringLiteral("path"), path);
	result.insert(QStringLiteral("bytes"), QString::number(QFileInfo(path).size()));
	result.insert(QStringLiteral("generation"), QString::number(updated.generation));
	return true;
}

bool Tools::load_session(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("generation"),
		QStringLiteral("path"), QStringLiteral("discard_unsaved")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("load_session does not accept %1").arg(it.key());
			return false;
		}
	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error) ||
		!require_generation(entry, arguments, error))
		return false;
	if (entry.session->get_capture_state() != Session::Stopped) {
		error = QStringLiteral("Cannot load while capture is running");
		return false;
	}
	if (!entry.session->data_saved() &&
		!arguments.value(QStringLiteral("discard_unsaved")).toBool(false)) {
		error = QStringLiteral("Session has unsaved capture data; set discard_unsaved=true to replace it");
		return false;
	}
	const QFileInfo file(arguments.value(QStringLiteral("path")).toString());
	if (!file.isAbsolute() || !file.isFile()) {
		error = QStringLiteral("path must name an existing absolute file");
		return false;
	}
	try {
		entry.session->load_file(file.absoluteFilePath());
	} catch (const sigrok::Error& exception) {
		error = QString::fromUtf8(exception.what());
		return false;
	}
	sessions_.changed(entry.session.get());
	SessionRegistry::Entry updated;
	sessions_.resolve(entry.id, updated);
	result.insert(QStringLiteral("accepted"), true);
	result.insert(QStringLiteral("path"), file.absoluteFilePath());
	result.insert(QStringLiteral("generation"), QString::number(updated.generation));
	return true;
}

bool Tools::query_annotations(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {
		QStringLiteral("session_id"), QStringLiteral("decode_signal_id"),
		QStringLiteral("segment_id"), QStringLiteral("range"),
		QStringLiteral("start_sample"), QStringLiteral("end_sample"),
		QStringLiteral("visible_only"), QStringLiteral("text_filter"),
		QStringLiteral("limit"), QStringLiteral("continuation_token")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("query_annotations does not accept %1").arg(it.key());
			return false;
		}

	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error))
		return false;
	const auto view = trace_view(entry, error);
	if (!view)
		return false;

	if (arguments.contains(QStringLiteral("segment_id")) &&
		(!arguments.value(QStringLiteral("segment_id")).isDouble() ||
		arguments.value(QStringLiteral("segment_id")).toDouble() !=
		arguments.value(QStringLiteral("segment_id")).toInt())) {
		error = QStringLiteral("segment_id must be an integer");
		return false;
	}
	if (arguments.contains(QStringLiteral("range")) &&
		!arguments.value(QStringLiteral("range")).isString()) {
		error = QStringLiteral("range must be a string");
		return false;
	}
	if (arguments.contains(QStringLiteral("decode_signal_id")) &&
		!arguments.value(QStringLiteral("decode_signal_id")).isString()) {
		error = QStringLiteral("decode_signal_id must be a string");
		return false;
	}
	const uint32_t segment_id = arguments.contains(QStringLiteral("segment_id")) ?
		static_cast<uint32_t>(arguments.value(QStringLiteral("segment_id")).toInt(-1)) :
		view->current_segment();
	if (segment_id > entry.session->get_highest_segment_id()) {
		error = QStringLiteral("Unknown segment ID: %1").arg(segment_id);
		return false;
	}

	const uint64_t sample_count = entry.session->get_segment_sample_count(segment_id);
	uint64_t start_sample = 0;
	uint64_t end_sample = sample_count;
	const bool has_start = arguments.contains(QStringLiteral("start_sample"));
	const bool has_end = arguments.contains(QStringLiteral("end_sample"));
	if (has_start != has_end) {
		error = QStringLiteral("start_sample and end_sample must be supplied together");
		return false;
	}
	QString range_name = arguments.value(QStringLiteral("range")).toString();
	if (has_start) {
		if (!sample_value(arguments, QStringLiteral("start_sample"), start_sample, error) ||
			!sample_value(arguments, QStringLiteral("end_sample"), end_sample, error))
			return false;
		range_name = QStringLiteral("explicit");
	} else {
		if (range_name.isEmpty())
			range_name = view->cursors_shown() ? QStringLiteral("cursor") :
				QStringLiteral("visible");
		const double samplerate = entry.session->get_samplerate();
		if (range_name == QStringLiteral("cursor")) {
			if (!view->cursors_shown()) {
				error = QStringLiteral("Cursor range requested but cursors are hidden");
				return false;
			}
			const auto cursors = view->cursors();
			const double first = cursors->first()->time().convert_to<double>() * samplerate;
			const double second = cursors->second()->time().convert_to<double>() * samplerate;
			start_sample = static_cast<uint64_t>(std::max(0.0, std::min(first, second)));
			end_sample = static_cast<uint64_t>(std::max(0.0, std::max(first, second)));
		} else if (range_name == QStringLiteral("visible")) {
			const double start = view->offset().convert_to<double>() * samplerate;
			const double end = start + view->viewport()->width() * view->scale() * samplerate;
			start_sample = static_cast<uint64_t>(std::max(0.0, start));
			end_sample = static_cast<uint64_t>(std::max(0.0, end));
		} else if (range_name != QStringLiteral("all")) {
			error = QStringLiteral("range must be cursor, visible, or all");
			return false;
		}
		start_sample = std::min(start_sample, sample_count);
		end_sample = std::min(end_sample, sample_count);
	}

	if (start_sample > end_sample || end_sample > sample_count) {
		error = QStringLiteral("Annotation range %1..%2 is outside segment 0..%3")
			.arg(start_sample).arg(end_sample).arg(sample_count);
		return false;
	}

	if (arguments.contains(QStringLiteral("limit")) &&
		!arguments.value(QStringLiteral("limit")).isDouble()) {
		error = QStringLiteral("limit must be an integer");
		return false;
	}
	if (arguments.contains(QStringLiteral("visible_only")) &&
		!arguments.value(QStringLiteral("visible_only")).isBool()) {
		error = QStringLiteral("visible_only must be a boolean");
		return false;
	}
	if (arguments.contains(QStringLiteral("text_filter")) &&
		!arguments.value(QStringLiteral("text_filter")).isString()) {
		error = QStringLiteral("text_filter must be a string");
		return false;
	}
	int limit = arguments.value(QStringLiteral("limit")).toInt(500);
	if (limit < 1 || limit > 1000) {
		error = QStringLiteral(
			"limit must be between 1 and 1000; use next_continuation_token for more results");
		return false;
	}
	uint64_t continuation_offset = 0;
	if (arguments.contains(QStringLiteral("continuation_token"))) {
		if (!arguments.value(QStringLiteral("continuation_token")).isString()) {
			error = QStringLiteral("continuation_token must be a string");
			return false;
		}
		bool valid = false;
		continuation_offset = arguments.value(QStringLiteral("continuation_token"))
			.toString().toULongLong(&valid);
		if (!valid || continuation_offset > std::numeric_limits<size_t>::max()) {
			error = QStringLiteral("Invalid continuation_token");
			return false;
		}
	}
	const bool visible_only = arguments.value(QStringLiteral("visible_only")).toBool(false);
	const QString text_filter = arguments.value(QStringLiteral("text_filter")).toString();
	const QString requested_signal = arguments.value(QStringLiteral("decode_signal_id")).toString();
	bool requested_signal_found = requested_signal.isEmpty();
	QJsonArray annotations;
	size_t total_count = 0;
	bool truncated = false;
	QString next_continuation_token;
#ifdef ENABLE_DECODE
	vector<AnnotationEvent> events;
	uint32_t decode_index = 0;
	for (const auto& base : entry.session->signalbases()) {
		if (!base->is_decode_signal())
			continue;
		const uint32_t current_index = decode_index++;
		if (!requested_signal.isEmpty() &&
			requested_signal != QString::number(current_index))
			continue;
		requested_signal_found = true;
		const auto signal = std::dynamic_pointer_cast<data::DecodeSignal>(base);
		for (data::AnnotationSnapshot& annotation :
			signal->get_annotation_snapshots(segment_id, start_sample, end_sample)) {
			if (visible_only && !annotation.visible)
				continue;
			if (!text_filter.isEmpty()) {
				bool matches = false;
				for (const QString& text : annotation.texts)
					matches |= text.contains(text_filter, Qt::CaseInsensitive);
				if (!matches)
					continue;
			}
			events.push_back({current_index, signal->display_name(), std::move(annotation)});
		}
	}
	if (!requested_signal_found) {
		error = QStringLiteral("Unknown decode signal ID: %1").arg(requested_signal);
		return false;
	}

	std::sort(events.begin(), events.end(), [](const AnnotationEvent& a,
		const AnnotationEvent& b) {
		if (a.annotation.start_sample != b.annotation.start_sample)
			return a.annotation.start_sample < b.annotation.start_sample;
		return (a.annotation.end_sample - a.annotation.start_sample) >
			(b.annotation.end_sample - b.annotation.start_sample);
	});
	total_count = events.size();
	const size_t page_start = std::min(
		static_cast<size_t>(continuation_offset), total_count);
	const size_t page_size = std::min(static_cast<size_t>(limit),
		total_count - page_start);
	const size_t page_end = page_start + page_size;
	truncated = page_end < total_count;
	if (truncated)
		next_continuation_token = QString::number(page_end);

	const double samplerate = entry.session->get_samplerate();
	for (size_t index = page_start; index < page_end; index++) {
		const AnnotationEvent& event = events[index];
		const data::AnnotationSnapshot& annotation = event.annotation;
		QJsonArray texts;
		for (const QString& text : annotation.texts)
			texts.append(text);
		QJsonObject item;
		item.insert(QStringLiteral("decode_signal_id"), QString::number(event.signal_index));
		item.insert(QStringLiteral("decode_signal_name"), event.signal_name);
		item.insert(QStringLiteral("decoder_id"), annotation.decoder_id);
		item.insert(QStringLiteral("decoder_name"), annotation.decoder_name);
		item.insert(QStringLiteral("decoder_stack_level"), annotation.decoder_stack_level);
		item.insert(QStringLiteral("row_index"), static_cast<int>(annotation.row_index));
		item.insert(QStringLiteral("row_title"), annotation.row_title);
		item.insert(QStringLiteral("row_description"), annotation.row_description);
		item.insert(QStringLiteral("class_id"), static_cast<int>(annotation.class_id));
		item.insert(QStringLiteral("class_name"), annotation.class_name);
		item.insert(QStringLiteral("class_description"), annotation.class_description);
		item.insert(QStringLiteral("start_sample"), QString::number(annotation.start_sample));
		item.insert(QStringLiteral("end_sample"), QString::number(annotation.end_sample));
		item.insert(QStringLiteral("duration_samples"), QString::number(
			annotation.end_sample - annotation.start_sample));
		item.insert(QStringLiteral("start_seconds"), annotation.start_sample / samplerate);
		item.insert(QStringLiteral("end_seconds"), annotation.end_sample / samplerate);
		item.insert(QStringLiteral("texts"), texts);
		item.insert(QStringLiteral("visible"), annotation.visible);
		annotations.append(item);
	}
#endif

	result.insert(QStringLiteral("session_id"), QString::number(entry.id));
	result.insert(QStringLiteral("generation"), QString::number(entry.generation));
	result.insert(QStringLiteral("segment_id"), static_cast<int>(segment_id));
	result.insert(QStringLiteral("range"), range_name);
	result.insert(QStringLiteral("start_sample"), QString::number(start_sample));
	result.insert(QStringLiteral("end_sample"), QString::number(end_sample));
	result.insert(QStringLiteral("annotations"), annotations);
	result.insert(QStringLiteral("returned_count"), annotations.size());
	result.insert(QStringLiteral("total_count"), QString::number(total_count));
	result.insert(QStringLiteral("truncated"), truncated);
	if (!next_continuation_token.isEmpty())
		result.insert(QStringLiteral("next_continuation_token"),
			next_continuation_token);
	return true;
}

bool Tools::set_cursors(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {QStringLiteral("session_id"),
		QStringLiteral("generation"), QStringLiteral("visible"),
		QStringLiteral("start_sample"), QStringLiteral("end_sample")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("set_cursors does not accept %1").arg(it.key());
			return false;
		}
	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error))
		return false;
	if (arguments.value(QStringLiteral("generation")).toString() !=
		QString::number(entry.generation)) {
		error = QStringLiteral("Stale generation; current generation is %1")
			.arg(entry.generation);
		return false;
	}
	if (!arguments.contains(QStringLiteral("visible"))) {
		error = QStringLiteral("set_cursors requires visible");
		return false;
	}
	if (!arguments.value(QStringLiteral("visible")).isBool()) {
		error = QStringLiteral("visible must be a boolean");
		return false;
	}
	const auto view = trace_view(entry, error);
	if (!view)
		return false;
	const bool visible = arguments.value(QStringLiteral("visible")).toBool();
	if (visible) {
		uint64_t start_sample, end_sample;
		if (!sample_value(arguments, QStringLiteral("start_sample"), start_sample, error) ||
			!sample_value(arguments, QStringLiteral("end_sample"), end_sample, error))
			return false;
		const uint64_t sample_count = entry.session->get_segment_sample_count(
			view->current_segment());
		if (start_sample > end_sample || end_sample > sample_count) {
			error = QStringLiteral("Cursor range is outside the current segment");
			return false;
		}
		const double samplerate = entry.session->get_samplerate();
		pv::util::Timestamp first = start_sample / samplerate;
		pv::util::Timestamp second = end_sample / samplerate;
		view->set_cursors(first, second);
	}
	view->show_cursors(visible);
	SessionRegistry::Entry updated;
	sessions_.resolve(entry.id, updated);
	result = view_context(updated, view);
	return true;
}

bool Tools::zoom_to_range(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	static const QSet<QString> allowed = {QStringLiteral("session_id"),
		QStringLiteral("generation"), QStringLiteral("start_sample"),
		QStringLiteral("end_sample")};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key())) {
			error = QStringLiteral("zoom_to_range does not accept %1").arg(it.key());
			return false;
		}
	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error))
		return false;
	if (arguments.value(QStringLiteral("generation")).toString() !=
		QString::number(entry.generation)) {
		error = QStringLiteral("Stale generation; current generation is %1")
			.arg(entry.generation);
		return false;
	}
	uint64_t start_sample, end_sample;
	if (!sample_value(arguments, QStringLiteral("start_sample"), start_sample, error) ||
		!sample_value(arguments, QStringLiteral("end_sample"), end_sample, error))
		return false;
	const auto view = trace_view(entry, error);
	if (!view)
		return false;
	const uint64_t sample_count = entry.session->get_segment_sample_count(
		view->current_segment());
	if (start_sample >= end_sample || end_sample > sample_count) {
		error = QStringLiteral("Zoom range is outside the current segment or empty");
		return false;
	}
	const int width = view->viewport()->width();
	if (width <= 0) {
		error = QStringLiteral("Trace viewport has no width");
		return false;
	}
	const double samplerate = entry.session->get_samplerate();
	const double scale = (end_sample - start_sample) / samplerate / width;
	view->set_scale_offset(scale, pv::util::Timestamp(start_sample / samplerate));
	SessionRegistry::Entry updated;
	sessions_.resolve(entry.id, updated);
	result = view_context(updated, view);
	return true;
}

} // namespace mcp
} // namespace pv

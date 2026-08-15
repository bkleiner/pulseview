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
#include <limits>

#include <QJsonDocument>
#include <QSet>

#ifdef ENABLE_DECODE
#include "pv/data/decodesignal.hpp"
#endif
#include "pv/devices/device.hpp"
#include "pv/session.hpp"
#include "pv/views/trace/cursor.hpp"
#include "pv/views/trace/cursorpair.hpp"
#include "pv/views/trace/view.hpp"
#include "pv/views/trace/viewport.hpp"
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

QJsonObject empty_schema()
{
	QJsonObject schema;
	schema.insert(QStringLiteral("type"), QStringLiteral("object"));
	schema.insert(QStringLiteral("additionalProperties"), false);
	return schema;
}

QJsonObject string_property(const QString& description)
{
	QJsonObject property;
	property.insert(QStringLiteral("type"), QStringLiteral("string"));
	property.insert(QStringLiteral("description"), description);
	return property;
}

QJsonObject tool_schema(const QJsonObject& properties,
	const QJsonArray& required = QJsonArray())
{
	QJsonObject schema;
	schema.insert(QStringLiteral("type"), QStringLiteral("object"));
	schema.insert(QStringLiteral("properties"), properties);
	schema.insert(QStringLiteral("additionalProperties"), false);
	if (!required.isEmpty())
		schema.insert(QStringLiteral("required"), required);
	return schema;
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
	QJsonObject annotations;
	annotations.insert(QStringLiteral("readOnlyHint"), true);
	annotations.insert(QStringLiteral("destructiveHint"), false);

	QJsonObject list_sessions_tool;
	list_sessions_tool.insert(QStringLiteral("name"), QStringLiteral("list_sessions"));
	list_sessions_tool.insert(QStringLiteral("description"),
		QStringLiteral("List the sessions currently open in this PulseView instance."));
	list_sessions_tool.insert(QStringLiteral("inputSchema"), empty_schema());
	list_sessions_tool.insert(QStringLiteral("annotations"), annotations);

	QJsonObject session_id;
	session_id.insert(QStringLiteral("type"), QStringLiteral("string"));
	session_id.insert(QStringLiteral("description"),
		QStringLiteral("Session ID from list_sessions; omit to use the active session."));
	QJsonObject properties;
	properties.insert(QStringLiteral("session_id"), session_id);
	QJsonObject view_schema;
	view_schema.insert(QStringLiteral("type"), QStringLiteral("object"));
	view_schema.insert(QStringLiteral("properties"), properties);
	view_schema.insert(QStringLiteral("additionalProperties"), false);

	QJsonObject view_tool;
	view_tool.insert(QStringLiteral("name"), QStringLiteral("get_view_context"));
	view_tool.insert(QStringLiteral("description"),
		QStringLiteral("Get the active trace view's exact visible range and cursors."));
	view_tool.insert(QStringLiteral("inputSchema"), view_schema);
	view_tool.insert(QStringLiteral("annotations"), annotations);

	QJsonObject range;
	range.insert(QStringLiteral("type"), QStringLiteral("string"));
	range.insert(QStringLiteral("enum"), QJsonArray{
		QStringLiteral("cursor"), QStringLiteral("visible"), QStringLiteral("all")});
	QJsonObject limit;
	limit.insert(QStringLiteral("type"), QStringLiteral("integer"));
	limit.insert(QStringLiteral("minimum"), 1);
	limit.insert(QStringLiteral("maximum"), 1000);
	limit.insert(QStringLiteral("description"), QStringLiteral(
		"Page size (default 500, maximum 1000). Follow next_continuation_token "
		"to retrieve additional pages."));
	QJsonObject query_properties;
	query_properties.insert(QStringLiteral("session_id"), session_id);
	query_properties.insert(QStringLiteral("decode_signal_id"),
		string_property(QStringLiteral("Decode signal ID from list_sessions; omit for all.")));
	query_properties.insert(QStringLiteral("segment_id"),
		QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
			{QStringLiteral("minimum"), 0}});
	query_properties.insert(QStringLiteral("range"), range);
	query_properties.insert(QStringLiteral("start_sample"),
		string_property(QStringLiteral("Inclusive explicit range start sample.")));
	query_properties.insert(QStringLiteral("end_sample"),
		string_property(QStringLiteral("Inclusive explicit range end sample.")));
	query_properties.insert(QStringLiteral("visible_only"),
		QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}});
	query_properties.insert(QStringLiteral("text_filter"),
		QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
	query_properties.insert(QStringLiteral("limit"), limit);
	query_properties.insert(QStringLiteral("continuation_token"),
		string_property(QStringLiteral(
			"Token returned by the previous page; repeat the same query parameters.")));
	QJsonObject query_tool;
	query_tool.insert(QStringLiteral("name"), QStringLiteral("query_annotations"));
	query_tool.insert(QStringLiteral("description"), QStringLiteral(
		"Query a page of copied protocol-decoder annotations overlapping an explicit, "
		"cursor, visible, or full range. Follow next_continuation_token until absent."));
	query_tool.insert(QStringLiteral("inputSchema"), tool_schema(query_properties));
	query_tool.insert(QStringLiteral("annotations"), annotations);

	QJsonObject write_annotations;
	write_annotations.insert(QStringLiteral("readOnlyHint"), false);
	write_annotations.insert(QStringLiteral("destructiveHint"), false);
	write_annotations.insert(QStringLiteral("idempotentHint"), true);
	QJsonObject cursor_properties;
	cursor_properties.insert(QStringLiteral("session_id"), session_id);
	cursor_properties.insert(QStringLiteral("generation"),
		string_property(QStringLiteral("Current generation from get_view_context.")));
	cursor_properties.insert(QStringLiteral("visible"),
		QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}});
	cursor_properties.insert(QStringLiteral("start_sample"),
		string_property(QStringLiteral("First cursor sample; required when visible is true.")));
	cursor_properties.insert(QStringLiteral("end_sample"),
		string_property(QStringLiteral("Second cursor sample; required when visible is true.")));
	QJsonObject cursor_tool;
	cursor_tool.insert(QStringLiteral("name"), QStringLiteral("set_cursors"));
	cursor_tool.insert(QStringLiteral("description"), QStringLiteral(
		"Show, move, or hide the active trace view cursors. Requires a current generation."));
	cursor_tool.insert(QStringLiteral("inputSchema"), tool_schema(cursor_properties,
		QJsonArray{QStringLiteral("generation"), QStringLiteral("visible")}));
	cursor_tool.insert(QStringLiteral("annotations"), write_annotations);

	QJsonObject zoom_properties;
	zoom_properties.insert(QStringLiteral("session_id"), session_id);
	zoom_properties.insert(QStringLiteral("generation"),
		string_property(QStringLiteral("Current generation from get_view_context.")));
	zoom_properties.insert(QStringLiteral("start_sample"),
		string_property(QStringLiteral("Visible range start sample.")));
	zoom_properties.insert(QStringLiteral("end_sample"),
		string_property(QStringLiteral("Visible range end sample.")));
	QJsonObject zoom_tool;
	zoom_tool.insert(QStringLiteral("name"), QStringLiteral("zoom_to_range"));
	zoom_tool.insert(QStringLiteral("description"), QStringLiteral(
		"Set the active trace view to an exact sample range. Requires a current generation."));
	zoom_tool.insert(QStringLiteral("inputSchema"), tool_schema(zoom_properties,
		QJsonArray{QStringLiteral("generation"), QStringLiteral("start_sample"),
			QStringLiteral("end_sample")}));
	zoom_tool.insert(QStringLiteral("annotations"), write_annotations);

	return QJsonArray{list_sessions_tool, view_tool, query_tool, cursor_tool, zoom_tool};
}

bool Tools::call_tool(const QString& name, const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	QJsonObject structured;
	if (name == QStringLiteral("list_sessions")) {
		if (!arguments.isEmpty()) {
			error = QStringLiteral("list_sessions does not accept arguments");
			return false;
		}
		structured = list_sessions();
	} else if (name == QStringLiteral("get_view_context")) {
		if (!get_view_context(arguments, structured, error))
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

QJsonObject Tools::list_sessions() const
{
	QJsonArray sessions;

	for (const SessionRegistry::Entry& entry : sessions_.entries()) {
		const std::shared_ptr<Session>& session = entry.session;
		QJsonObject item;
		item.insert(QStringLiteral("session_id"), QString::number(entry.id));
		item.insert(QStringLiteral("generation"), QString::number(entry.generation));
		item.insert(QStringLiteral("name"), session->name());
		item.insert(QStringLiteral("active"), entry.active);
		item.insert(QStringLiteral("capture_state"),
			capture_state_name(session->get_capture_state()));
		item.insert(QStringLiteral("samplerate"), session->get_samplerate());

		const std::shared_ptr<devices::Device> device = session->device();
		if (device)
			item.insert(QStringLiteral("device_name"),
				QString::fromStdString(device->full_name()));

		if (!session->save_path().isEmpty())
			item.insert(QStringLiteral("file_path"), session->save_path());

		QJsonArray segments;
		const uint32_t highest_segment = session->get_highest_segment_id();
		for (uint32_t id = 0; id <= highest_segment; id++) {
			QJsonObject segment;
			segment.insert(QStringLiteral("segment_id"), static_cast<int>(id));
			segment.insert(QStringLiteral("sample_count"),
				QString::number(session->get_segment_sample_count(id)));
			segments.append(segment);
		}
		item.insert(QStringLiteral("segments"), segments);

		QJsonArray decode_signals;
#ifdef ENABLE_DECODE
		uint32_t decode_index = 0;
		for (const auto& base : session->signalbases()) {
			if (!base->is_decode_signal())
				continue;
			const auto signal = std::dynamic_pointer_cast<data::DecodeSignal>(base);
			QJsonObject decode_item;
			decode_item.insert(QStringLiteral("decode_signal_id"),
				QString::number(decode_index++));
			decode_item.insert(QStringLiteral("name"), signal->display_name());
			QJsonArray decoders;
			for (const auto& decoder : signal->decoder_stack()) {
				const srd_decoder* srd = decoder->get_srd_decoder();
				decoders.append(QJsonObject{
					{QStringLiteral("id"), QString::fromUtf8(srd->id)},
					{QStringLiteral("name"), QString::fromUtf8(srd->name)},
					{QStringLiteral("stack_level"), decoder->get_stack_level()}});
			}
			decode_item.insert(QStringLiteral("decoders"), decoders);
			decode_signals.append(decode_item);
		}
#endif
		item.insert(QStringLiteral("decode_signals"), decode_signals);
		sessions.append(item);
	}

	QJsonObject result;
	result.insert(QStringLiteral("sessions"), sessions);
	return result;
}

bool Tools::get_view_context(const QJsonObject& arguments,
	QJsonObject& result, QString& error) const
{
	if (arguments.size() > 1 ||
		(arguments.size() == 1 && !arguments.contains(QStringLiteral("session_id")))) {
		error = QStringLiteral("get_view_context only accepts session_id");
		return false;
	}

	SessionRegistry::Entry entry;
	if (!resolve_session(sessions_, arguments, entry, error))
		return false;
	const auto view = trace_view(entry, error);
	if (!view)
		return false;
	result = view_context(entry, view);
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

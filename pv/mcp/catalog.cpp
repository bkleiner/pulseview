/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "catalog.hpp"

#include <QJsonObject>

namespace pv {
namespace mcp {

namespace {

QJsonObject property(const char *type, const QString& description)
{
	return QJsonObject{{QStringLiteral("type"), QString::fromLatin1(type)},
		{QStringLiteral("description"), description}};
}

QJsonObject schema(const QJsonObject& properties = QJsonObject(),
	const QJsonArray& required = QJsonArray())
{
	QJsonObject result{{QStringLiteral("type"), QStringLiteral("object")},
		{QStringLiteral("properties"), properties},
		{QStringLiteral("additionalProperties"), false}};
	if (!required.isEmpty())
		result.insert(QStringLiteral("required"), required);
	return result;
}

QJsonObject tool(const QString& name, const QString& description,
	const QJsonObject& input_schema, bool read_only, bool destructive = false,
	bool idempotent = false)
{
	QJsonObject annotations{{QStringLiteral("readOnlyHint"), read_only},
		{QStringLiteral("destructiveHint"), destructive}};
	if (idempotent)
		annotations.insert(QStringLiteral("idempotentHint"), true);
	return QJsonObject{{QStringLiteral("name"), name},
		{QStringLiteral("description"), description},
		{QStringLiteral("inputSchema"), input_schema},
		{QStringLiteral("annotations"), annotations}};
}

QJsonObject common_session_properties()
{
	return QJsonObject{
		{QStringLiteral("session_id"), property("string", QStringLiteral(
			"Optional session ID; omit to use the active PulseView session."))}};
}

} // namespace

QJsonArray tool_catalog()
{
	QJsonObject session_properties = common_session_properties();
	session_properties.insert(QStringLiteral("all_sessions"), property("boolean",
		QStringLiteral("Include summaries of all other open sessions.")));

	QJsonObject capture_properties = common_session_properties();
	capture_properties.insert(QStringLiteral("segment_id"), property("integer",
		QStringLiteral("Capture segment; omit to use the current segment.")));
	capture_properties.insert(QStringLiteral("start_sample"), property("string",
		QStringLiteral("Inclusive first sample; must be supplied with end_sample.")));
	capture_properties.insert(QStringLiteral("end_sample"), property("string",
		QStringLiteral("Exclusive last sample; must be supplied with start_sample.")));
	capture_properties.insert(QStringLiteral("channels"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("array")},
		{QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
		{QStringLiteral("description"), QStringLiteral("Optional channel-name filter.")}});
	capture_properties.insert(QStringLiteral("mode"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("string")},
		{QStringLiteral("enum"), QJsonArray{QStringLiteral("edges"), QStringLiteral("bits")}},
		{QStringLiteral("description"), QStringLiteral("Exact transitions (default) or packed bits.")}});
	capture_properties.insert(QStringLiteral("limit"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("integer")},
		{QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 1000000},
		{QStringLiteral("description"), QStringLiteral("Maximum returned edges or bit samples.")}});

	QJsonObject start_properties = common_session_properties();
	start_properties.insert(QStringLiteral("generation"), property("string",
		QStringLiteral("Current generation from get_session.")));
	start_properties.insert(QStringLiteral("samplerate"), property("string",
		QStringLiteral("Optional sample rate in Hz.")));
	start_properties.insert(QStringLiteral("num_samples"), property("string",
		QStringLiteral("Optional sample-count limit.")));
	start_properties.insert(QStringLiteral("wait"), property("boolean",
		QStringLiteral("Wait for acquisition completion; defaults to true.")));
	start_properties.insert(QStringLiteral("timeout_ms"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("integer")},
		{QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 120000}});

	QJsonObject set_properties = common_session_properties();
	set_properties.insert(QStringLiteral("generation"), property("string",
		QStringLiteral("Current generation from get_session.")));
	set_properties.insert(QStringLiteral("samplerate"), property("string", QStringLiteral("Sample rate in Hz.")));
	set_properties.insert(QStringLiteral("num_samples"), property("string", QStringLiteral("Sample-count limit.")));
	set_properties.insert(QStringLiteral("capture_ratio"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("integer")},
		{QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), 100}});
	set_properties.insert(QStringLiteral("channels"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("array")},
		{QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
		{QStringLiteral("description"), QStringLiteral(
			"Channel updates with name plus optional label, color, and trigger.")}});

	QJsonObject save_properties = common_session_properties();
	save_properties.insert(QStringLiteral("generation"), property("string",
		QStringLiteral("Current generation from get_session.")));
	save_properties.insert(QStringLiteral("path"), property("string", QStringLiteral("Absolute .sr path.")));
	save_properties.insert(QStringLiteral("overwrite"), property("boolean",
		QStringLiteral("Allow replacing an existing file; defaults to false.")));
	QJsonObject load_properties = common_session_properties();
	load_properties.insert(QStringLiteral("generation"), property("string",
		QStringLiteral("Current generation from get_session.")));
	load_properties.insert(QStringLiteral("path"), property("string", QStringLiteral("Absolute .sr path.")));
	load_properties.insert(QStringLiteral("discard_unsaved"), property("boolean",
		QStringLiteral("Allow load to replace unsaved capture data; defaults to false.")));

	QJsonObject query_properties = common_session_properties();
	query_properties.insert(QStringLiteral("decode_signal_id"), property("string", QStringLiteral("Optional decoder ID.")));
	query_properties.insert(QStringLiteral("segment_id"), property("integer", QStringLiteral("Optional segment.")));
	query_properties.insert(QStringLiteral("range"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("string")},
		{QStringLiteral("enum"), QJsonArray{QStringLiteral("cursor"), QStringLiteral("visible"), QStringLiteral("all")}}});
	query_properties.insert(QStringLiteral("start_sample"), property("string", QStringLiteral("Inclusive explicit start.")));
	query_properties.insert(QStringLiteral("end_sample"), property("string", QStringLiteral("Inclusive explicit end.")));
	query_properties.insert(QStringLiteral("visible_only"), property("boolean", QStringLiteral("Only visible annotation classes.")));
	query_properties.insert(QStringLiteral("text_filter"), property("string", QStringLiteral("Case-insensitive text filter.")));
	query_properties.insert(QStringLiteral("limit"), QJsonObject{
		{QStringLiteral("type"), QStringLiteral("integer")},
		{QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 1000}});
	query_properties.insert(QStringLiteral("continuation_token"), property("string", QStringLiteral("Previous page token.")));

	QJsonObject cursor_properties = common_session_properties();
	cursor_properties.insert(QStringLiteral("generation"), property("string", QStringLiteral("Current generation.")));
	cursor_properties.insert(QStringLiteral("visible"), property("boolean", QStringLiteral("Show or hide cursors.")));
	cursor_properties.insert(QStringLiteral("start_sample"), property("string", QStringLiteral("First cursor sample.")));
	cursor_properties.insert(QStringLiteral("end_sample"), property("string", QStringLiteral("Second cursor sample.")));

	QJsonObject zoom_properties = common_session_properties();
	zoom_properties.insert(QStringLiteral("generation"), property("string", QStringLiteral("Current generation.")));
	zoom_properties.insert(QStringLiteral("start_sample"), property("string", QStringLiteral("Visible range start.")));
	zoom_properties.insert(QStringLiteral("end_sample"), property("string", QStringLiteral("Visible range end.")));

	return QJsonArray{
		tool(QStringLiteral("get_session"), QStringLiteral(
			"Inspect the active session, device, channels, decoders, capture segments, view, and cursors."),
			schema(session_properties), true),
		tool(QStringLiteral("get_capture"), QStringLiteral(
			"Read bounded exact logic edges or packed bit samples from the active capture."),
			schema(capture_properties), true),
		tool(QStringLiteral("start_capture"), QStringLiteral(
			"Start acquisition on the active device, optionally waiting for completion."),
			schema(start_properties, {QStringLiteral("generation")}), false, true),
		tool(QStringLiteral("set_session"), QStringLiteral(
			"Configure acquisition settings and channel labels, colors, or triggers."),
			schema(set_properties, {QStringLiteral("generation")}), false, false, true),
		tool(QStringLiteral("save_session"), QStringLiteral("Save the current capture as a native .sr file."),
			schema(save_properties, {QStringLiteral("generation"), QStringLiteral("path")}), false, true),
		tool(QStringLiteral("load_session"), QStringLiteral("Load a native .sr capture into the active session."),
			schema(load_properties, {QStringLiteral("generation"), QStringLiteral("path")}), false, true),
		tool(QStringLiteral("query_annotations"), QStringLiteral(
			"Query a bounded page of decoder annotations; defaults to the cursor or visible range."),
			schema(query_properties), true),
		tool(QStringLiteral("set_cursors"), QStringLiteral("Show, move, or hide view cursors."),
			schema(cursor_properties, {QStringLiteral("generation"), QStringLiteral("visible")}), false, false, true),
		tool(QStringLiteral("zoom_to_range"), QStringLiteral("Set the visible trace range."),
			schema(zoom_properties, {QStringLiteral("generation"), QStringLiteral("start_sample"), QStringLiteral("end_sample")}),
			false, false, true)};
}

QString server_instructions()
{
	return QStringLiteral(
		"Tools default to the active PulseView session. Do not call get_session as a "
		"mandatory preflight: query_annotations and get_capture already default to the "
		"active session and current view. Call get_session when configuration, channel "
		"metadata, generation, or another open session is actually needed.");
}

} // namespace mcp
} // namespace pv

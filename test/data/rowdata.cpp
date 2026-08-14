/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <libsigrokdecode/libsigrokdecode.h>

#include <boost/test/unit_test.hpp>

#include <pv/data/decode/decoder.hpp>
#include <pv/data/decode/rowdata.hpp>

using pv::data::decode::Annotation;
using pv::data::decode::Decoder;
using pv::data::decode::RowData;

BOOST_AUTO_TEST_SUITE(RowDataTest)

BOOST_AUTO_TEST_CASE(RenderSubsetIsBounded)
{
	char decoder_id[] = "test";
	char decoder_name[] = "Test";
	char annotation_name[] = "data";
	char annotation_description[] = "Data";
	char *annotation_class[] = {annotation_name, annotation_description};
	GSList *annotation_classes = g_slist_append(nullptr, annotation_class);

	srd_decoder decoder_definition = {};
	decoder_definition.id = decoder_id;
	decoder_definition.name = decoder_name;
	decoder_definition.annotations = annotation_classes;

	Decoder decoder(&decoder_definition, 0);
	RowData row_data(decoder.get_rows().front());

	char text[] = "value";
	char *texts[] = {text, nullptr};
	srd_proto_data_annotation annotation_data = {0, texts};
	srd_proto_data protocol_data = {0, 0, nullptr, &annotation_data};

	for (uint64_t i = 0; i < 10000; i++) {
		protocol_data.start_sample = i * 10;
		protocol_data.end_sample = protocol_data.start_sample + 5;
		row_data.emplace_annotation(&protocol_data);
	}

	deque<const Annotation*> render_annotations;
	row_data.get_annotation_subset(render_annotations, 0, 100000, 2000);
	BOOST_CHECK_LE(render_annotations.size(), 2000);

	deque<const Annotation*> exact_annotations;
	row_data.get_annotation_subset(exact_annotations, 40000, 40100);
	BOOST_CHECK_EQUAL(exact_annotations.size(), 11);

	protocol_data.start_sample = 5;
	protocol_data.end_sample = 200000;
	row_data.emplace_annotation(&protocol_data);
	exact_annotations.clear();
	row_data.get_annotation_subset(exact_annotations, 150000, 150100);
	BOOST_REQUIRE_EQUAL(exact_annotations.size(), 1);
	BOOST_CHECK_EQUAL(exact_annotations.front()->start_sample(), 5);

	g_slist_free(annotation_classes);
}

BOOST_AUTO_TEST_SUITE_END()

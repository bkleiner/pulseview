/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <boost/test/unit_test.hpp>

#include <QStandardItemModel>

#include "pv/views/tabular_decoder/view.hpp"

using pv::views::tabular_decoder::CustomFilterProxyModel;
using pv::views::tabular_decoder::AnnotationGroupIdRole;
using pv::views::tabular_decoder::AnnotationTypeIdRole;

BOOST_AUTO_TEST_SUITE(TabularDecoderViewTest)

BOOST_AUTO_TEST_CASE(searches_value_column_only_case_insensitively)
{
	QStandardItemModel source(2, 7);
	source.setData(source.index(0, 0), 100);
	source.setData(source.index(0, 3), QStringLiteral("Current State"));
	source.setData(source.index(0, 5), QStringLiteral("GetAndClearIrqStatus"));
	source.setData(source.index(0, 6), 110);
	source.setData(source.index(1, 0), 200);
	source.setData(source.index(1, 4), QStringLiteral("IRQ status"));
	source.setData(source.index(1, 5), QStringLiteral("RX"));
	source.setData(source.index(1, 6), 210);

	CustomFilterProxyModel proxy;
	proxy.setSourceModel(&source);

	proxy.set_search_text(QStringLiteral("irqstatus"));
	BOOST_CHECK_EQUAL(proxy.rowCount(), 1);
	BOOST_CHECK_EQUAL(proxy.index(0, 0).data().toInt(), 100);

	proxy.set_search_text(QStringLiteral("CURRENT STATE"));
	BOOST_CHECK_EQUAL(proxy.rowCount(), 0);

	proxy.set_search_text(QStringLiteral("IRQSTATUS"));
	BOOST_CHECK_EQUAL(proxy.rowCount(), 1);
	BOOST_CHECK_EQUAL(proxy.index(0, 0).data().toInt(), 100);

	proxy.set_search_text(QString());
	BOOST_CHECK_EQUAL(proxy.rowCount(), 2);
}

BOOST_AUTO_TEST_CASE(combines_group_and_type_multiselect_filters)
{
	QStandardItemModel source(3, 7);
	source.setData(source.index(0, 0), 1, AnnotationGroupIdRole);
	source.setData(source.index(0, 0), 10, AnnotationTypeIdRole);
	source.setData(source.index(1, 0), 1, AnnotationGroupIdRole);
	source.setData(source.index(1, 0), 11, AnnotationTypeIdRole);
	source.setData(source.index(2, 0), 2, AnnotationGroupIdRole);
	source.setData(source.index(2, 0), 12, AnnotationTypeIdRole);

	CustomFilterProxyModel proxy;
	proxy.setSourceModel(&source);
	proxy.set_group_filter({1}, true);
	BOOST_CHECK_EQUAL(proxy.rowCount(), 2);

	proxy.set_type_filter({10}, true);
	BOOST_CHECK_EQUAL(proxy.rowCount(), 1);

	proxy.set_group_filter({}, true);
	BOOST_CHECK_EQUAL(proxy.rowCount(), 0);

	proxy.set_group_filter({}, false);
	proxy.set_type_filter({}, false);
	BOOST_CHECK_EQUAL(proxy.rowCount(), 3);
}

BOOST_AUTO_TEST_CASE(combines_all_filter_kinds)
{
	QStandardItemModel source(3, 7);
	for (int row = 0; row < 3; row++) {
		source.setData(source.index(row, 0), 1, AnnotationGroupIdRole);
		source.setData(source.index(row, 0), 10, AnnotationTypeIdRole);
		source.setData(source.index(row, 5), QStringLiteral("RX"));
	}
	source.setData(source.index(0, 0), 100);
	source.setData(source.index(0, 6), 110);
	source.setData(source.index(1, 0), 200);
	source.setData(source.index(1, 6), 210);
	source.setData(source.index(2, 0), 300);
	source.setData(source.index(2, 6), 310);

	CustomFilterProxyModel proxy;
	proxy.setSourceModel(&source);
	proxy.set_search_text(QStringLiteral("rx"));
	proxy.set_group_filter({1}, true);
	proxy.set_type_filter({10}, true);
	proxy.set_sample_range(190, 220);
	proxy.enable_range_filtering(true);

	BOOST_CHECK_EQUAL(proxy.rowCount(), 1);
	BOOST_CHECK_EQUAL(proxy.index(0, 0).data().toInt(), 200);
}

BOOST_AUTO_TEST_SUITE_END()

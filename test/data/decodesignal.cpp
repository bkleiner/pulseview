/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <boost/test/unit_test.hpp>

#include <pv/data/decodesignal.hpp>
#include <pv/data/logic.hpp>
#include <pv/data/signalbase.hpp>

using pv::data::DecodeSignal;
using pv::data::Logic;
using pv::data::SignalBase;
using pv::data::decode::DecodeChannel;
using std::make_shared;
using std::shared_ptr;
using std::vector;

namespace DecodeSignalTest {

struct InputSelection
{
	static shared_ptr<SignalBase> make_signal(
		const shared_ptr<Logic>& logic, unsigned int bit)
	{
		shared_ptr<SignalBase> signal = make_shared<SignalBase>(
			nullptr, SignalBase::LogicChannel);
		signal->set_index(bit);
		signal->set_data(logic);
		return signal;
	}

	static DecodeChannel make_channel(
		uint16_t id, const shared_ptr<SignalBase>& signal)
	{
		return {id, 99, false, signal, QString(), QString(), 0,
			nullptr, nullptr};
	}

	static void run()
	{
		const shared_ptr<Logic> common_logic = make_shared<Logic>(16);
		const shared_ptr<Logic> other_logic = make_shared<Logic>(8);
		const shared_ptr<SignalBase> signal_3 = make_signal(common_logic, 3);
		const shared_ptr<SignalBase> signal_11 = make_signal(common_logic, 11);
		const shared_ptr<SignalBase> signal_other = make_signal(other_logic, 5);

		vector<DecodeChannel> channels;
		channels.push_back(make_channel(0, signal_11));
		channels.push_back(make_channel(1, nullptr));
		channels.push_back(make_channel(2, signal_3));

		BOOST_CHECK(DecodeSignal::common_input_logic(channels) == common_logic);
		DecodeSignal::update_channel_bit_ids(channels, true);
		BOOST_CHECK_EQUAL(channels[0].bit_id, 11);
		BOOST_CHECK_EQUAL(channels[1].bit_id, 99);
		BOOST_CHECK_EQUAL(channels[2].bit_id, 3);

		channels.push_back(make_channel(3, signal_other));
		BOOST_CHECK(!DecodeSignal::common_input_logic(channels));
		DecodeSignal::update_channel_bit_ids(channels, false);
		BOOST_CHECK_EQUAL(channels[0].bit_id, 0);
		BOOST_CHECK_EQUAL(channels[1].bit_id, 99);
		BOOST_CHECK_EQUAL(channels[2].bit_id, 1);
		BOOST_CHECK_EQUAL(channels[3].bit_id, 2);
	}
};

} // namespace DecodeSignalTest

BOOST_AUTO_TEST_SUITE(DecodeSignalTestSuite)

BOOST_AUTO_TEST_CASE(common_input_uses_source_bit_positions)
{
	DecodeSignalTest::InputSelection::run();
}

BOOST_AUTO_TEST_SUITE_END()

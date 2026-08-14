/*
 * This file is part of the PulseView project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "sessionregistry.hpp"

#include "pv/session.hpp"

namespace pv {
namespace mcp {

SessionRegistry::SessionRegistry() :
	next_id_(1),
	active_id_(0)
{
}

uint64_t SessionRegistry::add(const std::shared_ptr<Session>& session)
{
	const uint64_t id = next_id_++;
	entries_[session.get()] = {id, 1, session};
	active_id_ = id;
	return id;
}

void SessionRegistry::remove(const Session *session)
{
	auto iter = entries_.find(session);
	if (iter == entries_.end())
		return;

	if (iter->second.id == active_id_)
		active_id_ = 0;
	entries_.erase(iter);
}

void SessionRegistry::set_active(const Session *session)
{
	auto iter = entries_.find(session);
	active_id_ = (iter == entries_.end()) ? 0 : iter->second.id;
}

void SessionRegistry::changed(const Session *session)
{
	auto iter = entries_.find(session);
	if (iter != entries_.end())
		iter->second.generation++;
}

std::vector<SessionRegistry::Entry> SessionRegistry::entries() const
{
	std::vector<Entry> result;
	result.reserve(entries_.size());

	for (const auto& item : entries_) {
		const std::shared_ptr<Session> session = item.second.session.lock();
		if (session)
			result.push_back({item.second.id, item.second.generation,
				item.second.id == active_id_, session});
	}

	return result;
}

bool SessionRegistry::resolve(uint64_t id, Entry& entry) const
{
	for (const auto& item : entries_) {
		if (item.second.id != id)
			continue;

		const std::shared_ptr<Session> session = item.second.session.lock();
		if (!session)
			return false;

		entry = {item.second.id, item.second.generation,
			item.second.id == active_id_, session};
		return true;
	}

	return false;
}

bool SessionRegistry::active(Entry& entry) const
{
	return active_id_ != 0 && resolve(active_id_, entry);
}

} // namespace mcp
} // namespace pv
